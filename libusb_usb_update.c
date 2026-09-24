#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <libusb-1.0/libusb.h>
#include "usb_update.h"
#include "image_package.h"
#include "progress.h"
#include "serial_boot.h"

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

typedef struct fdl_frame {
    unsigned int  magic_num;
    short         data_len;
    short         cmd_index;
    unsigned char data[64];  // checksum add to the end of data
} __attribute__((aligned(4))) fdl_frame_t;

#define FDL_MAGIC_SIZE    4
#define FDL_DATALEN_SIZE  2
#define FDL_CMDIDX_SIZE   2
#define FDL_CHECKSUM_SIZE 2

static const unsigned char fdl_magic[FDL_MAGIC_SIZE] = {0x9f, 0x8e, 0x6d, 0x5c};

typedef struct update_transport {
    libusb_context *usb_context;
    libusb_device_handle *usb_handle;
    int usb_claimed;
} update_transport_t;

static int transport_send(update_transport_t *transport,
                          const unsigned char *data, size_t length,
                          unsigned int timeout)
{
    int transferred;
    int ret;

    ret = libusb_bulk_transfer(transport->usb_handle, USB_UPDATE_OUT_ENDPOINT,
                               (unsigned char *)data, (int)length,
                               &transferred, timeout);
    if (ret != 0)
        return ret;
    return transferred == (int)length ? 0 : LIBUSB_ERROR_IO;
}

static void put_le16(unsigned char *p, unsigned short value)
{
    p[0] = (unsigned char)(value & 0xff);
    p[1] = (unsigned char)(value >> 8);
}

static void put_le32(unsigned char *p, unsigned int value)
{
    p[0] = (unsigned char)(value & 0xff);
    p[1] = (unsigned char)((value >> 8) & 0xff);
    p[2] = (unsigned char)((value >> 16) & 0xff);
    p[3] = (unsigned char)(value >> 24);
}

static unsigned short get_le16(const unsigned char *p)
{
    return (unsigned short)p[0] | ((unsigned short)p[1] << 8);
}

/* FDL sums little-endian 16-bit words from data_len through payload. */
static unsigned short fdl_checksum(const unsigned char *p, size_t len)
{
    unsigned int sum = 0;

    while (len >= 2) {
        sum += (unsigned int)p[0] | ((unsigned int)p[1] << 8);
        p += 2;
        len -= 2;
    }
    if (len != 0)
        sum += *p;

    sum = (sum & 0xffffU) + (sum >> 16);
    sum += sum >> 16;
    return (unsigned short)~sum;
}

static int fdl_build_frame(unsigned char *frame, size_t capacity,
                           unsigned short command, const unsigned char *payload,
                           unsigned short payload_len)
{
    size_t total = FDL_MAGIC_SIZE + FDL_DATALEN_SIZE + FDL_CMDIDX_SIZE +
                   payload_len + FDL_CHECKSUM_SIZE;
    unsigned short checksum;

    if (total > capacity)
        return -1;

    memcpy(frame, fdl_magic, sizeof(fdl_magic));
    put_le16(frame + 4, payload_len);
    put_le16(frame + 6, command);
    if (payload_len != 0 && payload != NULL)
        memcpy(frame + 8, payload, payload_len);
    checksum = fdl_checksum(frame + 4, FDL_DATALEN_SIZE + FDL_CMDIDX_SIZE +
                            payload_len);
    put_le16(frame + 8 + payload_len, checksum);
    return (int)total;
}

static int fdl_send_frame(update_transport_t *transport, unsigned short command,
                          const unsigned char *payload, unsigned short payload_len)
{
    unsigned char frame[BUFFER_SIZE];
    int length;

    length = fdl_build_frame(frame, sizeof(frame), command, payload, payload_len);
    if (length < 0)
        return LIBUSB_ERROR_INVALID_PARAM;
    return transport_send(transport, frame, (size_t)length, 3000);
}

static int fdl_receive_frame(update_transport_t *transport, unsigned char *data,
                             size_t capacity, int *actual_length, unsigned int timeout)
{
    int ret;

    ret = libusb_bulk_transfer(transport->usb_handle,
                               USB_UPDATE_IN_ENDPOINT, data,
                               (int)capacity, actual_length, timeout);
    if (ret != 0)
        return ret;
    if (*actual_length < FDL_MAGIC_SIZE + FDL_DATALEN_SIZE +
        FDL_CMDIDX_SIZE + FDL_CHECKSUM_SIZE)
        return LIBUSB_ERROR_IO;
    if (memcmp(data, fdl_magic, FDL_MAGIC_SIZE) != 0)
        return LIBUSB_ERROR_IO;
    if ((size_t)*actual_length != (size_t)(10 + get_le16(data + 4)))
        return LIBUSB_ERROR_IO;
    if (get_le16(data + 4) > capacity - 10)
        return LIBUSB_ERROR_OVERFLOW;
    if (fdl_checksum(data + 4, 4 + get_le16(data + 4)) !=
        get_le16(data + 8 + get_le16(data + 4)))
        return LIBUSB_ERROR_IO;
    return 0;
}

static int fdl_expect_ack(update_transport_t *transport, unsigned short command,
                          unsigned int timeout)
{
    unsigned char data[BUFFER_SIZE];
    int actual_length, ret;

    ret = fdl_receive_frame(transport, data, sizeof(data), &actual_length,
                            timeout);
    if (ret != 0)
        return ret;
    if (get_le16(data + 6) != command)
        return LIBUSB_ERROR_IO;
    return 0;
}

/*
 * mode 0: version ack
 *      1: normal ack
 *      2: process ack
 *      3: failed ack
 */
static int libusb_checkack(unsigned char *data, unsigned int len,
                           unsigned char mode)
{
    unsigned char com_ack[] = {0x9f, 0x8e, 0x6d, 0x5c, 0x00,
                               0x00, 0x80, 0x00, 0x7f, 0xff};
    unsigned char ver_ack[] = {0x9f, 0x8e, 0x6d, 0x5c, 0x10, 0x00, 0x81,
                               0x00, 0x72, 0x6f, 0x6d, 0x63, 0x6f, 0x64,
                               0x65, 0x20, 0x76, 0x31, 0x2e, 0x30, 0x3b,
                               0x72, 0x61, 0x77, 0x79, 0x5c};

    unsigned char fail_ack[] = {0x9f, 0x8e, 0x6d, 0x5c, 0x00,
                                0x00, 0x8b, 0x00, 0x74, 0xff};

    unsigned int i;
    int         ret = -1, chk_sum;
    fdl_frame_t recv_data;

    switch (mode) {
        case 0:
            if (len != 26) {
                fprintf(stderr, "len err %d-26\n", len);
                break;
            }
            ret = memcmp(ver_ack, data, len);
            if (ret != 0)
                fprintf(stderr, "ack format err %d\n", mode);
            break;

        case 1:
            if (len != 10) {
                fprintf(stderr, "len err %d-10\n", len);
                break;
            }

            ret = memcmp(com_ack, data, len);
            if (ret != 0)
                fprintf(stderr, "ack format err %d\n", mode);
            break;

        case 2:
            if (len != 11) {
                fprintf(stderr, "len err %d-11\n", len);
                break;
            }
            memcpy(&recv_data, data, sizeof(recv_data));
            chk_sum = ~(recv_data.data_len + recv_data.cmd_index + recv_data.data[0]);
            if ((chk_sum & 0xFF) != recv_data.data[1] || ((chk_sum >> 8 & 0xFF)) != recv_data.data[2]) {
                fprintf(stderr, "process ack format err %d\n", mode);
                ret = -1;
            } else {
                ret = 0;
            }
            break;

        case 3:
            if (len != 10) {
                fprintf(stderr, "len err %d-10\n", len);
                break;
            }

            ret = memcmp(fail_ack, data, len);
            if (ret != 0)
                fprintf(stderr, "ack format err %d\n", mode);
            break;

        default:
            fprintf(stderr, "never here mode:%d\n", mode);
            break;
    }

    if (ret != 0) {
        for (i = 0; i < len; i++)
            fprintf(stderr, "%02x ", data[i]);
        fprintf(stderr, "\n");
    }

    return ret;
}

static short frame_checksum(fdl_frame_t *pframe)
{
    return (short)fdl_checksum((unsigned char *)pframe + FDL_MAGIC_SIZE,
                               FDL_DATALEN_SIZE + FDL_CMDIDX_SIZE +
                               (unsigned short)pframe->data_len);
}

static int cmd_handshake(update_transport_t *transport)
{
    unsigned char probe[] = {0x3c, 0x3c, 0x3c};
    unsigned char data[BUFFER_SIZE];
    int actual_length, ret;

    ret = transport_send(transport, probe, sizeof(probe), 3000);
    if (ret != 0)
        return ret;

    ret = fdl_receive_frame(transport, data, sizeof(data), &actual_length, 6000);
    if (ret != 0)
        return ret;
    if (get_le16(data + 6) != FDL_RESP_VERSION)
        return LIBUSB_ERROR_IO;
    return 0;
}

static int cmd_connect(update_transport_t *transport)
{
    int ret = fdl_send_frame(transport, FDL_CMD_CONNECT, NULL, 0);
    if (ret != 0)
        return ret;
    return fdl_expect_ack(transport, FDL_RESP_ACK, 6000);
}

static int cmd_start_transfer(update_transport_t *transport, unsigned int addr,
                              unsigned filesize)
{
    unsigned char payload[8];
    int ret;

    put_le32(payload, addr);
    put_le32(payload + 4, filesize);
    ret = fdl_send_frame(transport, FDL_CMD_START_TRANSFER, payload,
                         sizeof(payload));
    if (ret != 0)
        return ret;
    return fdl_expect_ack(transport, FDL_RESP_ACK, 6000);
}

/* Two interactions per single transmission */
int cmd_transfer_data(libusb_device_handle *handle, int pack_size, int enable,
                      unsigned char *buf)
{
    fdl_frame_t   cmd_data;
    short         checksum;
    int           ret, transfered, actual_length;
    unsigned char data[100];

    if (enable == 0) {
        cmd_data.magic_num                 = 0x5C6D8E9F;
        cmd_data.data_len                  = 0xc;
        cmd_data.cmd_index                 = 0x2;
        ((unsigned int *)cmd_data.data)[0] = pack_size;
        ((unsigned int *)cmd_data.data)[1] = enable;
        ((unsigned int *)cmd_data.data)[2] = 0;

        checksum          = frame_checksum(&cmd_data);
        cmd_data.data[12] = checksum & 0xFF;
        cmd_data.data[13] = (checksum >> 8) & 0xFF;
        ret               = libusb_bulk_transfer(handle, USB_UPDATE_OUT_ENDPOINT,
                                                 (unsigned char *)&cmd_data, 22,
                                                 &transfered, 3 * TIMEOUT_S);
        if ((ret != 0) || (transfered != 22)) {
            fprintf(stderr, "send err %d-%s\n", transfered,
                    libusb_strerror(ret));
            return ret;
        }

        ret = libusb_bulk_transfer(handle, USB_UPDATE_IN_ENDPOINT, data,
                                   sizeof(data), &actual_length,
                                   6 * TIMEOUT_S);
        if (ret == 0) {
            ret = libusb_checkack(data, actual_length, 1);
            if (ret != 0)
                fprintf(stderr, "cmd_transfer_data ack err1\n");
        } else {
            fprintf(stderr, "err in bulk_transfer recv1: %s\n",
                    libusb_strerror(ret));
            return ret;
        }

        ret = libusb_bulk_transfer(handle, USB_UPDATE_OUT_ENDPOINT,
                                   (unsigned char *)buf, pack_size,
                                   &transfered, 3 * TIMEOUT_S);
        if ((ret != 0) || (transfered != pack_size)) {
            fprintf(stderr, "send err %d-%s\n", transfered,
                    libusb_strerror(ret));
            return ret;
        }

        ret = libusb_bulk_transfer(handle, USB_UPDATE_IN_ENDPOINT, data,
                                   sizeof(data), &actual_length,
                                   6 * TIMEOUT_S);
        if (ret == 0) {
            ret = libusb_checkack(data, actual_length, 1);
            if (ret != 0) {
                printf("cmd_transfer_data ack err2\n");
            }
        } else {
            fprintf(stderr, "err in bulk_transfer recv2: %s\n",
                    libusb_strerror(ret));
            return ret;
        }
    } else {
        /*  reserve */
    }

    return 0;
}

int cmd_ended_data(libusb_device_handle *handle)
{
    fdl_frame_t   cmd_data, *recv_data;
    int           ret, transfered, actual_length;
    unsigned char data[100] __attribute__((aligned(4)));

    cmd_data.magic_num = 0x5C6D8E9F;
    cmd_data.data_len  = 0;
    cmd_data.cmd_index = 3;
    cmd_data.data[0]   = 0xfc;
    cmd_data.data[1]   = 0xff;
    ret                = libusb_bulk_transfer(handle, USB_UPDATE_OUT_ENDPOINT,
                                              (unsigned char *)&cmd_data, 10, &transfered,
                                              3 * TIMEOUT_S);
    if ((ret != 0) || (transfered != 10)) {
        fprintf(stderr, "send err %d-%s\n", transfered,
                libusb_strerror(ret));
        return ret;
    }

    while (1) {
        ret = libusb_bulk_transfer(handle, USB_UPDATE_IN_ENDPOINT, data,
                                   sizeof(data), &actual_length, 10000);
        if (ret == 0) {
            recv_data = (fdl_frame_t *)data;
            switch (recv_data->cmd_index) {
                case FDL_RESP_UPDATE_IN:
                    ret = libusb_checkack(data, actual_length, 2);
                    if (ret == 0)
                        printf("Process: %d\n",
                               recv_data->data[0]);
                    continue;

                case FDL_RESP_ACK:
                    ret = libusb_checkack(data, actual_length, 1);
                    printf("\n");
                    return ret;

                case FDL_RESP_OPERATION_FAIL:
                    ret = libusb_checkack(data, actual_length, 3);
                    printf("\n");
                    return -1;

                default:
                    fprintf(stderr, "never here\n");
                    return -1;
            }
        } else {
            fprintf(stderr, "err in bulk_transfer recv: %s\n",
                    libusb_strerror(ret));
            return -1;
        }
    }

    return ret;
}

int cmd_exec_data(libusb_device_handle *handle)
{
    fdl_frame_t   cmd_data;
    int           ret, transfered, actual_length;
    unsigned char data[100];

    cmd_data.magic_num = 0x5C6D8E9F;
    cmd_data.data_len  = 0;
    cmd_data.cmd_index = 4;
    cmd_data.data[0]   = 0xfb;
    cmd_data.data[1]   = 0xff;
    ret                = libusb_bulk_transfer(handle, USB_UPDATE_OUT_ENDPOINT,
                                              (unsigned char *)&cmd_data, 10, &transfered,
                                              3 * TIMEOUT_S);
    if ((ret != 0) || (transfered != 10)) {
        fprintf(stderr, "send err %d-%s\n", transfered,
                libusb_strerror(ret));
        return ret;
    }

    ret = libusb_bulk_transfer(handle, USB_UPDATE_IN_ENDPOINT, data,
                               sizeof(data), &actual_length, 6 * TIMEOUT_S);
    if (ret == 0) {
        ret = libusb_checkack(data, actual_length, 1);
        if (ret != 0)
            fprintf(stderr, "cmd_exec_data ack err\n");
    } else {
        fprintf(stderr, "err in bulk_transfer recv: %s\n",
                libusb_strerror(ret));
    }

    return ret;
}

/* Explicit little-endian implementation used by the update path below. */
static int cmd_transfer_data_safe(update_transport_t *transport, int pack_size,
                                  unsigned char *buf)
{
    unsigned char payload[12];
    int ret;

    if (pack_size <= 0 || pack_size > UPDATE_PER_PACK || buf == NULL)
        return LIBUSB_ERROR_INVALID_PARAM;
    put_le32(payload, (unsigned int)pack_size);
    put_le32(payload + 4, 0);
    put_le32(payload + 8, 0);
    ret = fdl_send_frame(transport, FDL_CMD_START_TRANSFER_DATA, payload,
                         sizeof(payload));
    if (ret != 0)
        return ret;
    ret = fdl_expect_ack(transport, FDL_RESP_ACK, 6000);
    if (ret != 0)
        return ret;
    ret = transport_send(transport, buf, (size_t)pack_size, 3000);
    if (ret != 0)
        return ret;
    return fdl_expect_ack(transport, FDL_RESP_ACK, 6000);
}

static int cmd_transfer_end_safe(update_transport_t *transport)
{
    unsigned char data[BUFFER_SIZE];
    progress_t progress = {0};
    int actual_length, ret;

    ret = fdl_send_frame(transport, FDL_CMD_START_TRANSFER_END, NULL, 0);
    if (ret != 0)
        return ret;
    progress_start(&progress, "USB device write", 100);
    for (;;) {
        ret = fdl_receive_frame(transport, data, sizeof(data), &actual_length,
                                10000);
        if (ret != 0)
            break;
        switch (get_le16(data + 6)) {
        case FDL_RESP_UPDATE_IN:
            if (get_le16(data + 4) != 1) {
                ret = LIBUSB_ERROR_IO;
                goto out;
            }
            progress_update(&progress, data[8]);
            break;
        case FDL_RESP_ACK:
            progress_finish(&progress);
            return 0;
        case FDL_RESP_OPERATION_FAIL:
            ret = LIBUSB_ERROR_IO;
            goto out;
        default:
            ret = LIBUSB_ERROR_IO;
            goto out;
        }
    }
out:
    progress_abort(&progress);
    return ret;
}

static int cmd_execute_safe(update_transport_t *transport)
{
    int ret = fdl_send_frame(transport, FDL_CMD_EXECUTE, NULL, 0);
    if (ret != 0)
        return ret;
    return fdl_expect_ack(transport, FDL_RESP_ACK, 6000);
}

static void transport_close(update_transport_t *transport)
{
    if (transport->usb_handle != NULL) {
        if (transport->usb_claimed)
            libusb_release_interface(transport->usb_handle, 0);
        libusb_close(transport->usb_handle);
        transport->usb_handle = NULL;
    }
    if (transport->usb_context != NULL) {
        libusb_exit(transport->usb_context);
        transport->usb_context = NULL;
    }
}

static int transport_open_usb(update_transport_t *transport, short vid,
                              short pid)
{
    int ret;

    ret = libusb_init(&transport->usb_context);
    if (ret < 0) {
        fprintf(stderr, "failed to initialize libusb: %s\n",
                libusb_error_name(ret));
        return ret;
    }

    transport->usb_handle = libusb_open_device_with_vid_pid(
        transport->usb_context, (uint16_t)vid, (uint16_t)pid);
    if (transport->usb_handle == NULL) {
        fprintf(stderr, "err opening device vid-pid: %x-%x\n",
                (unsigned short)vid, (unsigned short)pid);
        transport_close(transport);
        return LIBUSB_ERROR_NO_DEVICE;
    }

    libusb_set_auto_detach_kernel_driver(transport->usb_handle, 1);
    ret = libusb_claim_interface(transport->usb_handle, 0);
    if (ret < 0) {
        fprintf(stderr, "err claiming interface: %s\n",
                libusb_error_name(ret));
        transport_close(transport);
        return ret;
    }
    transport->usb_claimed = 1;
    return 0;
}

static int update_one_image(update_transport_t *transport,
                            const image_entry_t *entry)
{
    unsigned char buffer[UPDATE_PER_PACK];
    char progress_label[64];
    progress_t progress = {0};
    uint64_t bytes_sent = 0;
    int ret, fd, count, retries;

    fd = open(entry->path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", entry->name, strerror(errno));
        return LIBUSB_ERROR_IO;
    }
    printf("USB update %s at 0x%08x, size 0x%x\n",
           entry->name, entry->address, entry->file_size);
    ret = cmd_start_transfer(transport, entry->address,
                             entry->erase_size);
    if (ret != 0)
        goto out;
    snprintf(progress_label, sizeof(progress_label), "USB transfer %.50s",
             entry->name);
    progress_start(&progress, progress_label, entry->file_size);
    while ((count = read(fd, buffer, sizeof(buffer))) > 0) {
        for (retries = 0; retries < MAX_ERRORS; retries++) {
            ret = cmd_transfer_data_safe(transport, count, buffer);
            if (ret == 0)
                break;
        }
        if (ret != 0)
            goto out;
        bytes_sent += (uint64_t)count;
        progress_update(&progress, bytes_sent);
    }
    if (count < 0) {
        fprintf(stderr, "read %s: %s\n", entry->name, strerror(errno));
        ret = LIBUSB_ERROR_IO;
        goto out;
    }
    if (bytes_sent != entry->file_size) {
        fprintf(stderr, "image size changed while reading %s\n", entry->name);
        ret = LIBUSB_ERROR_IO;
        goto out;
    }
    progress_finish(&progress);
    ret = cmd_transfer_end_safe(transport);
out:
    progress_abort(&progress);
    close(fd);
    return ret;
}

static int update_usb(const image_entry_t *entries, size_t count,
                      size_t skip_index)
{
    update_transport_t transport = {0};
    size_t i;
    int ret;

    ret = transport_open_usb(&transport, VENDOR_ID, PRODUCT_ID);
    if (ret != 0)
        return -1;
    ret = cmd_handshake(&transport);
    if (ret == 0)
        ret = cmd_connect(&transport);
    if (ret != 0) {
        fprintf(stderr, "USB handshake/connect failed: %s\n",
                libusb_error_name(ret));
        goto out;
    }
    for (i = 0; i < count; i++) {
        if (i == skip_index)
            continue;
        ret = update_one_image(&transport, &entries[i]);
        if (ret != 0) {
            fprintf(stderr, "USB update failed for %s: %s\n",
                    entries[i].name, libusb_error_name(ret));
            goto out;
        }
    }
    ret = cmd_execute_safe(&transport);
    if (ret != 0)
        fprintf(stderr, "USB reboot failed: %s\n", libusb_error_name(ret));
out:
    transport_close(&transport);
    return ret == 0 ? 0 : -1;
}

static void print_usage(FILE *stream, const char *program)
{
    fprintf(stream,
            "Usage:\n"
            "  %s [-p tty-device] [-b baud] [-u] ax520_image_spi.zip\n"
            "  %s [-p tty-device] [-b baud] [bootload image] "
            "flash image start [flash image start ...]\n"
            "  %s -p tty-device [-b baud] bootload image\n"
            "  -p DEVICE  serial bootloader tty device (for bootload)\n"
            "  -b BAUD    serial baud rate (default: 115200)\n"
            "  -u         force USB-only ZIP update even when -p is set\n"
            "  -h         show this help\n",
            program, program, program);
}

static int parse_baud_rate(const char *text, unsigned int *baud)
{
    char *end;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0 ||
        value > UINT_MAX)
        return -1;
    *baud = (unsigned int)value;
    return 0;
}

static int wait_and_update_usb(const image_entry_t *entries, size_t count,
                               size_t skip_index)
{
    printf("Waiting for USB download device 32c9:1000\n");
    fflush(stdout);
    if (libusb_get_vid_pid() != 0)
        return -1;
    usleep(100000);
    return update_usb(entries, count, skip_index);
}

static int run_zip(const char *zip_path, const char *serial_device,
                   unsigned int baud, int skip_bootload)
{
    image_package_t package;
    const image_entry_t *comboot;
    int use_serial = serial_device != NULL && !skip_bootload;
    int ret;

    printf("Update mode: ZIP (%s)\n",
           use_serial ? "serial bootload + USB flash" : "USB flash only");
    fflush(stdout);
    if (image_package_load(&package, zip_path) != 0)
        return -1;
    comboot = &package.entries[package.comboot_index];
    printf("ZIP validated: %zu images, comboot %s\n",
           package.count, comboot->name);
    fflush(stdout);
    ret = 0;
    if (use_serial)
        ret = serial_flash_comboot(serial_device, baud, comboot->path,
                                   comboot->address, comboot->erase_size);
    if (ret == 0)
        ret = wait_and_update_usb(package.entries, package.count,
                                   package.comboot_index);
    image_package_release(&package);
    return ret;
}

static int parse_flash_address(const char *text, uint32_t *address)
{
    unsigned long value;
    char *end;

    errno = 0;
    value = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || *text == '-' ||
        value < 0x03000000UL || value >= 0x04000000UL)
        return -1;
    *address = (uint32_t)value;
    return 0;
}

static int prepare_flash_image(image_entry_t *entry, const char *path,
                               const char *start)
{
    struct stat info;
    const char *name;

    if (parse_flash_address(start, &entry->address) != 0) {
        fprintf(stderr, "invalid flash start address: %s\n", start);
        return -1;
    }
    if (realpath(path, entry->path) == NULL ||
        stat(entry->path, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size <= 0 || info.st_size > UINT32_MAX ||
        (uint64_t)entry->address + (uint64_t)info.st_size > 0x04000000ULL) {
        fprintf(stderr, "invalid flash image or address range: %s\n", path);
        return -1;
    }
    name = strrchr(entry->path, '/');
    name = name == NULL ? entry->path : name + 1;
    if (snprintf(entry->name, sizeof(entry->name), "%s", name) >=
        (int)sizeof(entry->name)) {
        fprintf(stderr, "image filename too long: %s\n", path);
        return -1;
    }
    entry->file_size = (uint32_t)info.st_size;
    entry->erase_size = entry->file_size;
    return 0;
}

static int run_commands(int argc, char *argv[], int first,
                        const char *serial_device, unsigned int baud)
{
    image_entry_t *entries;
    const char *boot_image = NULL;
    struct stat info;
    size_t count = 0;
    int i, ret = -1;

    entries = calloc((size_t)(argc - first), sizeof(*entries));
    if (entries == NULL)
        return -1;
    for (i = first; i < argc;) {
        if (strcmp(argv[i], "bootload") == 0) {
            if (boot_image != NULL || count != 0) {
                fprintf(stderr, "bootload must appear once before flash\n");
                goto out;
            }
            if (i + 1 >= argc) {
                fprintf(stderr, "bootload requires an image\n");
                goto out;
            }
            boot_image = argv[i + 1];
            if (stat(boot_image, &info) != 0 || !S_ISREG(info.st_mode) ||
                info.st_size <= 0 || info.st_size > 0x30000) {
                fprintf(stderr, "invalid bootload image: %s\n", boot_image);
                goto out;
            }
            i += 2;
        } else if (strcmp(argv[i], "flash") == 0) {
            if (i + 2 >= argc) {
                fprintf(stderr, "flash requires an image and start address\n");
                goto out;
            }
            if (prepare_flash_image(&entries[count], argv[i + 1],
                                    argv[i + 2]) != 0)
                goto out;
            count++;
            i += 3;
        } else {
            fprintf(stderr, "unknown command: %s\n", argv[i]);
            goto out;
        }
    }
    if (boot_image != NULL && serial_device == NULL) {
        fprintf(stderr, "bootload requires -p tty-device\n");
        goto out;
    }
    printf("Update mode: %s\n", boot_image != NULL ?
           (count != 0 ? "serial bootload + USB flash" : "serial bootload") :
           "USB flash only");
    fflush(stdout);
    if (boot_image != NULL &&
        serial_flash_comboot(serial_device, baud, boot_image,
                             0x03000000U, 0x30000U) != 0)
        goto out;
    if (count != 0 && wait_and_update_usb(entries, count, SIZE_MAX) != 0)
        goto out;
    ret = 0;
out:
    free(entries);
    return ret;
}

int main(int argc, char *argv[])
{
    const char *serial_device = NULL;
    unsigned int baud = 115200;
    int skip_bootload = 0;
    int option, ret;

    while ((option = getopt(argc, argv, "+b:hp:u")) != -1) {
        switch (option) {
        case 'b':
            if (parse_baud_rate(optarg, &baud) != 0) {
                fprintf(stderr, "invalid baud rate: %s\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case 'p':
            serial_device = optarg;
            break;
        case 'u':
            skip_bootload = 1;
            break;
        case 'h':
            print_usage(stdout, argv[0]);
            return EXIT_SUCCESS;
        default:
            print_usage(stderr, argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (optind >= argc) {
        print_usage(stderr, argv[0]);
        return EXIT_FAILURE;
    }
    if (skip_bootload &&
        (strcmp(argv[optind], "bootload") == 0 ||
         strcmp(argv[optind], "flash") == 0)) {
        fprintf(stderr, "-u is only valid for ZIP updates\n");
        return EXIT_FAILURE;
    }
    if (!skip_bootload && serial_device != NULL &&
        strcmp(argv[optind], "flash") != 0 &&
        !serial_baud_supported(baud)) {
        fprintf(stderr, "unsupported baud rate: %u\n", baud);
        return EXIT_FAILURE;
    }

    if (strcmp(argv[optind], "bootload") == 0 ||
        strcmp(argv[optind], "flash") == 0)
        ret = run_commands(argc, argv, optind, serial_device, baud);
    else if (argc - optind == 1)
        ret = run_zip(argv[optind], serial_device, baud, skip_bootload);
    else {
        print_usage(stderr, argv[0]);
        return EXIT_FAILURE;
    }
    if (ret != 0) {
        fprintf(stderr, "firmware update failed\n");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
