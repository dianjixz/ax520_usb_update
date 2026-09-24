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

#define MAX_LINE_LEN 0x100

static const unsigned char fdl_magic[FDL_MAGIC_SIZE] = {0x9f, 0x8e, 0x6d, 0x5c};

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

static int fdl_send_frame(libusb_device_handle *handle, unsigned short command,
                          const unsigned char *payload, unsigned short payload_len)
{
    unsigned char frame[BUFFER_SIZE];
    int length, transferred, ret;

    length = fdl_build_frame(frame, sizeof(frame), command, payload, payload_len);
    if (length < 0)
        return LIBUSB_ERROR_INVALID_PARAM;
    ret = libusb_bulk_transfer(handle, USB_UPDATE_OUT_ENDPOINT, frame, length,
                               &transferred, 3000);
    if (ret != 0)
        return ret;
    if (transferred != length)
        return LIBUSB_ERROR_IO;
    return 0;
}

static int fdl_receive_frame(libusb_device_handle *handle, unsigned char *data,
                             size_t capacity, int *actual_length, unsigned int timeout)
{
    int ret;

    ret = libusb_bulk_transfer(handle, USB_UPDATE_IN_ENDPOINT, data,
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

static int fdl_expect_ack(libusb_device_handle *handle, unsigned short command,
                          unsigned int timeout)
{
    unsigned char data[BUFFER_SIZE];
    int actual_length, ret;

    ret = fdl_receive_frame(handle, data, sizeof(data), &actual_length, timeout);
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

int cmd_handshake(libusb_device_handle *handle)
{
    unsigned char probe[] = {0x3c, 0x3c, 0x3c};
    unsigned char data[BUFFER_SIZE];
    int transferred, actual_length, ret;

    ret = libusb_bulk_transfer(handle, USB_UPDATE_OUT_ENDPOINT, probe,
                               sizeof(probe), &transferred, 3000);
    if (ret != 0 || transferred != (int)sizeof(probe))
        return ret != 0 ? ret : LIBUSB_ERROR_IO;

    ret = fdl_receive_frame(handle, data, sizeof(data), &actual_length, 6000);
    if (ret != 0)
        return ret;
    if (get_le16(data + 6) != FDL_RESP_VERSION)
        return LIBUSB_ERROR_IO;
    return 0;
}

int cmd_connect(libusb_device_handle *handle)
{
    int ret = fdl_send_frame(handle, FDL_CMD_CONNECT, NULL, 0);
    if (ret != 0)
        return ret;
    return fdl_expect_ack(handle, FDL_RESP_ACK, 6000);
}

int cmd_start_transfer(libusb_device_handle *handle, unsigned int addr,
                       unsigned filesize)
{
    unsigned char payload[8];
    int ret;

    put_le32(payload, addr);
    put_le32(payload + 4, filesize);
    ret = fdl_send_frame(handle, FDL_CMD_START_TRANSFER, payload, sizeof(payload));
    if (ret != 0)
        return ret;
    return fdl_expect_ack(handle, FDL_RESP_ACK, 6000);
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
static int cmd_transfer_data_safe(libusb_device_handle *handle, int pack_size,
                                  unsigned char *buf)
{
    unsigned char payload[12];
    int transferred, ret;

    if (pack_size <= 0 || pack_size > UPDATE_PER_PACK || buf == NULL)
        return LIBUSB_ERROR_INVALID_PARAM;
    put_le32(payload, (unsigned int)pack_size);
    put_le32(payload + 4, 0);
    put_le32(payload + 8, 0);
    ret = fdl_send_frame(handle, FDL_CMD_START_TRANSFER_DATA, payload,
                         sizeof(payload));
    if (ret != 0)
        return ret;
    ret = fdl_expect_ack(handle, FDL_RESP_ACK, 6000);
    if (ret != 0)
        return ret;
    ret = libusb_bulk_transfer(handle, USB_UPDATE_OUT_ENDPOINT, buf, pack_size,
                               &transferred, 3000);
    if (ret != 0)
        return ret;
    if (transferred != pack_size)
        return LIBUSB_ERROR_IO;
    return fdl_expect_ack(handle, FDL_RESP_ACK, 6000);
}

static int cmd_transfer_end_safe(libusb_device_handle *handle)
{
    unsigned char data[BUFFER_SIZE];
    int actual_length, ret;

    ret = fdl_send_frame(handle, FDL_CMD_START_TRANSFER_END, NULL, 0);
    if (ret != 0)
        return ret;
    for (;;) {
        ret = fdl_receive_frame(handle, data, sizeof(data), &actual_length, 10000);
        if (ret != 0)
            return ret;
        switch (get_le16(data + 6)) {
        case FDL_RESP_UPDATE_IN:
            if (get_le16(data + 4) != 1)
                return LIBUSB_ERROR_IO;
            printf("Process: %u%%\n", data[8]);
            break;
        case FDL_RESP_ACK:
            return 0;
        case FDL_RESP_OPERATION_FAIL:
            return LIBUSB_ERROR_IO;
        default:
            return LIBUSB_ERROR_IO;
        }
    }
}

static int cmd_execute_safe(libusb_device_handle *handle)
{
    int ret = fdl_send_frame(handle, FDL_CMD_EXECUTE, NULL, 0);
    if (ret != 0)
        return ret;
    return fdl_expect_ack(handle, FDL_RESP_ACK, 6000);
}

int libusb_update_img(char *input_file, short vid, short pid)
{
    libusb_context       *ctx    = NULL;
    libusb_device_handle *handle = NULL;
    FILE                 *txt_fd;
    unsigned char         buffer[UPDATE_PER_PACK];
    char                  filename[PATH_MAX], line[MAX_LINE_LEN];
    unsigned int          file_size, w_addr, e_size;
    long                  file_end;
    int                   ret, fd, cnt = 0, bytes_read;

    txt_fd = fopen(input_file, "r");
    if (!txt_fd) {
        fprintf(stderr, "no %s found\n", input_file);
        return -1;
    }

    ret = libusb_init(&ctx);
    if (ret < 0) {
        fprintf(stderr, "failed to initialize libusb: %s\n",
                libusb_error_name(ret));
        fclose(txt_fd);
        return EXIT_FAILURE;
    }

    handle = libusb_open_device_with_vid_pid(ctx, vid, pid);
    if (!handle) {
        fprintf(stderr, "err opening device vid-pid: %x-%x\n", vid,
                pid);
        fclose(txt_fd);
        libusb_exit(ctx);
        return EXIT_FAILURE;
    }

    libusb_set_auto_detach_kernel_driver(handle, 1);

    ret = libusb_claim_interface(handle, 0);
    if (ret < 0) {
        fprintf(stderr, "err claiming interface: %s\n",
                libusb_error_name(ret));
        fclose(txt_fd);
        libusb_close(handle);
        libusb_exit(ctx);
        return EXIT_FAILURE;
    }

    /* BSL_CMD_HANDSHAKE */
    ret = cmd_handshake(handle);
    if (ret != 0) {
        fprintf(stderr, "cmd_handshake err\n");
        goto update_end;
    }

    /* BSL_CMD_CONNECT */
    ret = cmd_connect(handle);
    if (ret != 0) {
        fprintf(stderr, "cmd_connect err\n");
        goto update_end;
    }

    while (fgets(line, sizeof(line), txt_fd)) {
        if (line[0] == '#')
            continue;

        ret = sscanf(line, "%1023s %x %x", filename, &w_addr, &e_size);
        if (ret != 3) {
            printf("warning: line format err: %s\n", line);
            continue;
        }

        if (access(filename, F_OK) == 0) {
            if ((fd = open(filename, O_RDONLY)) == -1) {
                fprintf(stderr, "file opening err\n");
                goto update_end;
            }

            file_end = lseek(fd, 0, SEEK_END);
            if (file_end < 0 || (unsigned long)file_end > 0xffffffffUL) {
                fprintf(stderr, "cannot determine size of %s: %s\n", filename,
                        strerror(errno));
                close(fd);
                ret = LIBUSB_ERROR_INVALID_PARAM;
                goto update_end;
            }
            file_size = (unsigned int)file_end;
            if (lseek(fd, 0, SEEK_SET) < 0) {
                fprintf(stderr, "cannot rewind %s: %s\n", filename,
                        strerror(errno));
                close(fd);
                ret = LIBUSB_ERROR_IO;
                goto update_end;
            }

            if (e_size == 0)
                e_size = file_size;

            printf("update name: %s, addr: 0x%x, erasesize: 0x%x\n",
                   filename, w_addr, e_size);
            ret = cmd_start_transfer(handle, w_addr, e_size);
            if (ret != 0) {
                fprintf(stderr, "cmd_start_transfer err\n");
                close(fd);
                goto update_end;
            }

            /* BSL_CMD_START_DATA */
            while ((bytes_read = read(fd, buffer, UPDATE_PER_PACK)) > 0) {
                cnt = 0;
                while (cnt < MAX_ERRORS) {
                    ret = cmd_transfer_data_safe(handle, bytes_read, buffer);
                    if (ret != 0) {
                        fprintf(stderr,
                                "cmd_transfer_data err, cnt %d\n",
                                cnt);
                        cnt++;
                    } else {
                        break;
                    }
                }

                if (MAX_ERRORS == cnt) {
                    ret = LIBUSB_ERROR_IO;
                    close(fd);
                    goto update_end;
                }
            }
            if (bytes_read < 0) {
                fprintf(stderr, "read %s failed: %s\n", filename, strerror(errno));
                ret = LIBUSB_ERROR_IO;
                close(fd);
                goto update_end;
            }

            ret = cmd_transfer_end_safe(handle);
            if (ret != 0) {
                fprintf(stderr, "cmd_ended_data err\n");
                close(fd);
                goto update_end;
            }
            close(fd);
        } else {
            fprintf(stderr, "file %s does not exist\n", filename);
            ret = LIBUSB_ERROR_NOT_FOUND;
            goto update_end;
        }
    }

    ret = cmd_execute_safe(handle);
    if (ret != 0) {
        fprintf(stderr, "cmd_exec_data err\n");
        goto update_end;
    }

update_end:
    fclose(txt_fd);
    // release ctx and dev
    libusb_release_interface(handle, 0);
    libusb_close(handle);
    libusb_exit(ctx);

    return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

int libusb_uvc_reset(short vid, short pid)
{
    libusb_context       *ctx    = NULL;
    libusb_device_handle *handle = NULL;
    int                   ret;
    unsigned char        *data = NULL;

    ret = libusb_init(&ctx);
    if (ret < 0) {
        fprintf(stderr, "failed to initialize libusb: %s\n",
                libusb_error_name(ret));
        return EXIT_FAILURE;
    }

    handle = libusb_open_device_with_vid_pid(ctx, vid, pid);
    if (!handle) {
        fprintf(stderr, "err opening  device\n");
        libusb_exit(ctx);
        return EXIT_FAILURE;
    }

    libusb_set_auto_detach_kernel_driver(handle, 1);

    ret = libusb_claim_interface(handle, 0);  // 假设接口索引为 0
    if (ret < 0) {
        fprintf(stderr, "err claiming interface: %s\n",
                libusb_error_name(ret));
        libusb_close(handle);
        libusb_exit(ctx);
        return EXIT_FAILURE;
    }

    // 发送自定义控制请求
    ret = libusb_control_transfer(handle,
                                  0x21,    // bmRequestType
                                  0x1,     // 请求
                                  0x0,     // wValue - 控制选择符和参数
                                  0x0a00,  // wIndex - 接口编号
                                  data,    // 数据缓冲区
                                  0,       // 数据长度
                                  10);     // 超时时间
    // because udc reboot immediately, so omit the following steps or delay udc
    libusb_release_interface(handle, 0);
    libusb_close(handle);
    libusb_exit(ctx);

    return EXIT_SUCCESS;
}

int main(int argc, char *argv[])
{
    short vid, pid;
    int   ret;

    if (argc != 2 && argc != 3) {
        printf("usage_update:%s update.txt\n", argv[0]);
        printf("usage_update: %s update.txt uvc\n", argv[0]);
        return EXIT_FAILURE;
    }

    /* if reset, do reset */
    if ((argc == 3) && (strcmp(argv[2], "uvc") == 0)) {
        up_debug("reset uvc\n");
        vid = UVC_VENDOR_ID;
        pid = UVC_PRODUCT_ID;
        libusb_uvc_reset(vid, pid);

        sleep(5);
        vid = VENDOR_ID;
        pid = PRODUCT_ID;
    } else {
        /* get vid pid to tell enum done */
        if (libusb_get_vid_pid() != 0) {
            return -1;
        } else {
            usleep(10000);
            vid = VENDOR_ID;
            pid = PRODUCT_ID;
        }
    }

    ret = libusb_update_img(argv[1], vid, pid);
    if (ret != 0) {
        fprintf(stderr, "libusb_update_img failed\n");
        return -1;
    }

    return 0;
}
