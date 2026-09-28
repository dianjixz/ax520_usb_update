#define _DEFAULT_SOURCE
#include "serial_boot.h"
#include "progress.h"
#include "debug.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define START_BYTE 0x02
#define END_BYTE 0x03
#define HDBOOT_NOTIFY 0x24
#define MINIBOOT_NOTIFY 0x75
#define ACK_OK 0x05
#define DEBUG_CMD 0x14
#define DLOAD_CMD 0x18
#define RUN_CMD 0x1b
#define EXECPROG_CMD 0x1d
#define ERASE_CMD 0x45
#define CHUNK_SIZE 256
#define USB_CONNECTED_TEXT "usb connected to host"
#define USB_NOT_PLUGGED_TEXT "usb not plug-in"
#define UBOOT_PROMPT "ub#"
#define SERIAL_PROBE_INTERVAL_MS 30
#define SERIAL_REOPEN_INTERVAL_MS 500
#define SERIAL_RESET_LIMIT 3U
#define SERIAL_HANDSHAKE_TIMEOUT_MS 30000

typedef enum serial_recovery_state {
    SERIAL_WAIT_USB,
    SERIAL_WAIT_PROMPT,
    SERIAL_SENDING_RESET
} serial_recovery_state_t;

struct serial_log_monitor {
    int fd;
    const char *device;
    unsigned int baud;
    int show_log;
    int log_line_start;
    int progress_active;
    int stop_monitoring;
    char recent[sizeof(USB_CONNECTED_TEXT)];
    size_t recent_length;
    serial_recovery_state_t recovery_state;
    int64_t next_probe_ms;
    int64_t next_reopen_ms;
    size_t reset_offset;
    unsigned int reset_count;
    int reset_limit_reported;
    unsigned int probe_count;
    int64_t next_debug_ms;
};

typedef serial_log_monitor_t serial_reader_t;

static int baud_speed(unsigned int baud, speed_t *speed)
{
    switch (baud) {
    case 9600: *speed = B9600; return 0;
    case 19200: *speed = B19200; return 0;
    case 38400: *speed = B38400; return 0;
    case 57600: *speed = B57600; return 0;
    case 115200: *speed = B115200; return 0;
    case 230400: *speed = B230400; return 0;
#ifdef B460800
    case 460800: *speed = B460800; return 0;
#endif
#ifdef B921600
    case 921600: *speed = B921600; return 0;
#endif
#ifdef B1000000
    case 1000000: *speed = B1000000; return 0;
#endif
#ifdef B1152000
    case 1152000: *speed = B1152000; return 0;
#endif
#ifdef B1500000
    case 1500000: *speed = B1500000; return 0;
#endif
#ifdef B2000000
    case 2000000: *speed = B2000000; return 0;
#endif
    default: return -1;
    }
}

int serial_baud_supported(unsigned int baud)
{
    speed_t speed;
    return baud_speed(baud, &speed) == 0;
}

static int open_serial(const char *device, unsigned int baud, int quiet)
{
    struct termios tty;
    speed_t speed;
    int fd;

    if (baud_speed(baud, &speed) != 0)
        return -1;
    fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (!quiet)
            fprintf(stderr, "open %s: %s\n", device, strerror(errno));
        return -1;
    }
    if (tcgetattr(fd, &tty) != 0) {
        if (!quiet)
            fprintf(stderr, "tcgetattr %s: %s\n", device, strerror(errno));
        close(fd);
        return -1;
    }
    cfmakeraw(&tty);
    tty.c_cflag &= ~(CSIZE | PARENB | CSTOPB | CRTSCTS);
    tty.c_cflag |= CS8 | CLOCAL | CREAD;
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;
    if (cfsetispeed(&tty, speed) != 0 || cfsetospeed(&tty, speed) != 0 ||
        tcsetattr(fd, TCSANOW, &tty) != 0) {
        if (!quiet)
            fprintf(stderr, "configure %s: %s\n", device, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static int64_t now_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int wait_fd(int fd, short events, int64_t deadline)
{
    struct pollfd pfd = {.fd = fd, .events = events};
    int64_t now;
    int ret, timeout;

    for (;;) {
        now = now_ms();
        if (now < 0 || now >= deadline)
            return -1;
        timeout = (int)(deadline - now);
        if (debug_enabled() && timeout > 5000)
            timeout = 5000;
        ret = poll(&pfd, 1, timeout);
        if (ret > 0)
            return (pfd.revents & events) ? 0 : -1;
        if (ret == 0 && debug_enabled() && now_ms() < deadline) {
            debug_log("serial %s still waiting (fd=%d, %lld ms left)",
                      events == POLLIN ? "receive" : "send", fd,
                      (long long)(deadline - now_ms()));
            continue;
        }
        if (ret == 0 || errno != EINTR)
            return -1;
    }
}

static int read_byte(int fd, unsigned char *byte, int64_t deadline)
{
    ssize_t count;
    for (;;) {
        if (wait_fd(fd, POLLIN, deadline) != 0)
            return -1;
        count = read(fd, byte, 1);
        if (count == 1)
            return 0;
        if (count == 0 || (errno != EINTR && errno != EAGAIN))
            return -1;
    }
}

static int write_all(int fd, const unsigned char *data, size_t length,
                     int64_t deadline)
{
    size_t done = 0;
    ssize_t count;
    while (done < length) {
        if (wait_fd(fd, POLLOUT, deadline) != 0)
            return -1;
        count = write(fd, data + done, length - done);
        if (count > 0)
            done += (size_t)count;
        else if (count < 0 && errno != EINTR && errno != EAGAIN)
            return -1;
    }
    return 0;
}

static void put_be32(unsigned char *data, uint32_t value)
{
    data[0] = (unsigned char)(value >> 24);
    data[1] = (unsigned char)(value >> 16);
    data[2] = (unsigned char)(value >> 8);
    data[3] = (unsigned char)value;
}

static int send_command(int fd, unsigned char command,
                        const unsigned char *payload, size_t length)
{
    unsigned char frame[4 + 8 + CHUNK_SIZE];
    unsigned int checksum = 0;
    size_t i;

    if (length > sizeof(frame) - 4)
        return -1;
    for (i = 0; i < length; i++)
        checksum += payload[i];
    frame[0] = START_BYTE;
    frame[1] = command;
    frame[2] = (unsigned char)checksum;
    frame[3] = END_BYTE;
    if (length != 0)
        memcpy(frame + 4, payload, length);
    return write_all(fd, frame, length + 4, now_ms() + 3000);
}

static void print_serial_log_byte(serial_reader_t *reader, unsigned char byte)
{
    if (!reader->show_log)
        return;
    if (byte == '\r' || byte == '\n') {
        if (!reader->log_line_start) {
            fputc('\n', stderr);
            reader->log_line_start = 1;
        }
    } else if (byte == '\t' || (byte >= 0x20 && byte != 0x7f)) {
        if (reader->log_line_start) {
            if (reader->progress_active && isatty(STDOUT_FILENO))
                fputc('\n', stderr);
            fputs("[serial] ", stderr);
            reader->log_line_start = 0;
        }
        fputc(byte, stderr);
    }
    fflush(stderr);
}

static void finish_serial_log_line(serial_reader_t *reader)
{
    if (reader->show_log && !reader->log_line_start) {
        fputc('\n', stderr);
        fflush(stderr);
        reader->log_line_start = 1;
    }
}

static int serial_recent_ends_with(const serial_log_monitor_t *monitor,
                                   const char *text, size_t length)
{
    return monitor->recent_length >= length &&
           memcmp(monitor->recent + monitor->recent_length - length,
                  text, length) == 0;
}

static int serial_monitor_note_byte(serial_log_monitor_t *monitor,
                                    unsigned char byte)
{
    if (byte == '\r' || byte == '\n' || byte < 0x20 || byte == 0x7f) {
        monitor->recent_length = 0;
        return 0;
    }
    if (monitor->recent_length == sizeof(monitor->recent)) {
        memmove(monitor->recent, monitor->recent + 1,
                sizeof(monitor->recent) - 1);
        monitor->recent_length--;
    }
    if (byte >= 'A' && byte <= 'Z')
        byte = (unsigned char)(byte + ('a' - 'A'));
    monitor->recent[monitor->recent_length++] = (char)byte;

    if (serial_recent_ends_with(monitor, USB_CONNECTED_TEXT,
                                sizeof(USB_CONNECTED_TEXT) - 1)) {
        finish_serial_log_line(monitor);
        printf("Device reports USB connected to host; closing serial monitor\n");
        fflush(stdout);
        monitor->stop_monitoring = 1;
        return 1;
    }
    if (monitor->recovery_state == SERIAL_WAIT_USB &&
        serial_recent_ends_with(monitor, USB_NOT_PLUGGED_TEXT,
                                sizeof(USB_NOT_PLUGGED_TEXT) - 1)) {
        finish_serial_log_line(monitor);
        if (monitor->reset_count < SERIAL_RESET_LIMIT) {
            monitor->recovery_state = SERIAL_WAIT_PROMPT;
            monitor->next_probe_ms = 0;
            monitor->probe_count = 0;
            monitor->next_debug_ms = 0;
            printf("Device reports USB not plugged in; probing U-Boot prompt\n");
        } else if (!monitor->reset_limit_reported) {
            monitor->reset_limit_reported = 1;
            printf("Serial reset limit reached (%u); still waiting for USB\n",
                   SERIAL_RESET_LIMIT);
        }
        fflush(stdout);
        monitor->recent_length = 0;
    } else if (monitor->recovery_state == SERIAL_WAIT_PROMPT &&
               serial_recent_ends_with(monitor, UBOOT_PROMPT,
                                       sizeof(UBOOT_PROMPT) - 1)) {
        finish_serial_log_line(monitor);
        monitor->recovery_state = SERIAL_SENDING_RESET;
        monitor->reset_offset = 0;
        monitor->recent_length = 0;
        printf("U-Boot prompt detected; preparing reset (%u/%u)\n",
               monitor->reset_count + 1, SERIAL_RESET_LIMIT);
        fflush(stdout);
    }
    return 0;
}

static int serial_monitor_send(serial_log_monitor_t *monitor)
{
    static const char reset_command[] = "reset\r";
    const char probe = '\r';
    ssize_t count;
    int64_t now = now_ms();

    if (monitor->recovery_state == SERIAL_WAIT_PROMPT &&
        now >= monitor->next_probe_ms) {
        count = write(monitor->fd, &probe, 1);
        if (count == 1) {
            monitor->next_probe_ms = now + SERIAL_PROBE_INTERVAL_MS;
            monitor->probe_count++;
            if (debug_enabled() && now >= monitor->next_debug_ms) {
                finish_serial_log_line(monitor);
                debug_log("serial U-Boot prompt probe: %u CR bytes sent, reset %u/%u",
                          monitor->probe_count, monitor->reset_count,
                          SERIAL_RESET_LIMIT);
                monitor->next_debug_ms = now + 1000;
            }
        } else if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                 errno != EINTR)
            return -1;
    } else if (monitor->recovery_state == SERIAL_SENDING_RESET) {
        count = write(monitor->fd, reset_command + monitor->reset_offset,
                      sizeof(reset_command) - 1 - monitor->reset_offset);
        if (count > 0) {
            monitor->reset_offset += (size_t)count;
            if (monitor->reset_offset == sizeof(reset_command) - 1) {
                monitor->reset_count++;
                monitor->recovery_state = SERIAL_WAIT_USB;
                printf("Serial reset sent (%u/%u)\n",
                       monitor->reset_count, SERIAL_RESET_LIMIT);
                fflush(stdout);
            }
        } else if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                   errno != EINTR) {
            return -1;
        }
    }
    return 0;
}

void serial_log_drain(serial_log_monitor_t *monitor)
{
    unsigned char buffer[256];
    struct pollfd pfd;
    size_t total = 0, i;
    ssize_t count;
    int ready;

    if (monitor == NULL || monitor->stop_monitoring)
        return;
    if (monitor->fd < 0) {
        if (now_ms() < monitor->next_reopen_ms)
            return;
        monitor->fd = open_serial(monitor->device, monitor->baud, 1);
        if (monitor->fd < 0) {
            if (debug_enabled() && now_ms() >= monitor->next_debug_ms) {
                debug_log("serial reopen %s failed: %s", monitor->device,
                          strerror(errno));
                monitor->next_debug_ms = now_ms() + 5000;
            }
            monitor->next_reopen_ms = now_ms() + SERIAL_REOPEN_INTERVAL_MS;
            return;
        }
        printf("Serial device reconnected: %s\n", monitor->device);
        fflush(stdout);
    }
    while (total < 4096) {
        pfd.fd = monitor->fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        ready = poll(&pfd, 1, 0);
        if (ready == 0)
            break;
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0 || !(pfd.revents & POLLIN)) {
            if (ready < 0 ||
                (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)))
                goto disconnected;
            break;
        }
        count = read(monitor->fd, buffer, sizeof(buffer));
        if (count > 0) {
            for (i = 0; i < (size_t)count; i++) {
                print_serial_log_byte(monitor, buffer[i]);
                if (serial_monitor_note_byte(monitor, buffer[i]))
                    goto disconnected;
            }
            total += (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        /* VMIN=0 permits a zero-byte read even when the tty is still open. */
        if (count == 0 && !(pfd.revents & (POLLHUP | POLLERR | POLLNVAL)))
            break;
        if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
            goto disconnected;
        break;
    }
    if (serial_monitor_send(monitor) != 0)
        goto disconnected;
    return;

disconnected:
    finish_serial_log_line(monitor);
    debug_log("serial monitor %s: %s", monitor->stop_monitoring ?
              "closed after USB connection report" : "disconnected",
              monitor->device);
    close(monitor->fd);
    monitor->fd = -1;
    monitor->recent_length = 0;
    if (monitor->recovery_state == SERIAL_SENDING_RESET) {
        monitor->recovery_state = SERIAL_WAIT_PROMPT;
        monitor->reset_offset = 0;
        monitor->next_probe_ms = 0;
    }
    monitor->next_reopen_ms = now_ms() + SERIAL_REOPEN_INTERVAL_MS;
}

void serial_log_close(serial_log_monitor_t *monitor)
{
    if (monitor == NULL)
        return;
    finish_serial_log_line(monitor);
    if (monitor->fd >= 0)
        close(monitor->fd);
    free(monitor);
}

static int receive_command(serial_reader_t *reader, int64_t deadline)
{
    unsigned char frame[4];
    size_t i;

    while (read_byte(reader->fd, &frame[0], deadline) == 0) {
        if (frame[0] != START_BYTE) {
            print_serial_log_byte(reader, frame[0]);
            continue;
        }
        for (i = 1; i < sizeof(frame); i++) {
            if (read_byte(reader->fd, &frame[i], deadline) != 0)
                break;
        }
        if (i == sizeof(frame) && frame[3] == END_BYTE && frame[2] == 0) {
            finish_serial_log_line(reader);
            return frame[1];
        }
        for (size_t j = 0; j < i; j++)
            print_serial_log_byte(reader, frame[j]);
        if (i != sizeof(frame)) {
            finish_serial_log_line(reader);
            return -1;
        }
    }
    finish_serial_log_line(reader);
    return -1;
}

static int expect_ack(serial_reader_t *reader, unsigned int timeout_ms)
{
    int response = receive_command(reader, now_ms() + timeout_ms);
    if (response != ACK_OK) {
        fprintf(stderr, "serial ACK error: %d\n", response);
        return -1;
    }
    return 0;
}

static int handshake(serial_reader_t *reader)
{
    int64_t deadline = now_ms() + SERIAL_HANDSHAKE_TIMEOUT_MS;
    int response;

    debug_log("waiting up to %d s for ROM boot notification on %s",
              SERIAL_HANDSHAKE_TIMEOUT_MS / 1000, reader->device);
    while ((response = receive_command(reader, deadline)) >= 0) {
        if (response != HDBOOT_NOTIFY && response != MINIBOOT_NOTIFY)
            continue;
        printf("Serial boot notify: %s\n",
               response == HDBOOT_NOTIFY ? "HDBOOT" : "MINIBOOT");
        debug_log("sending serial DEBUG command");
        if (send_command(reader->fd, DEBUG_CMD, NULL, 0) != 0)
            return -1;
        if (response == MINIBOOT_NOTIFY) {
            /* Some MINIBOOT versions acknowledge DEBUG and others do not. */
            response = receive_command(reader, now_ms() + 200);
            return response == -1 || response == ACK_OK ? 0 : -1;
        }
        return expect_ack(reader, 1000);
    }
    fprintf(stderr, "serial boot notification timed out\n");
    return -1;
}

static unsigned char crc8(const unsigned char *data, size_t length)
{
    uint16_t crc = 0;
    size_t i;
    int bit;

    for (i = 0; i < length; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (bit = 0; bit < 8; bit++) {
            if (crc & 0x8000)
                crc ^= 0x8380;
            crc <<= 1;
        }
    }
    return (unsigned char)(crc >> 8);
}

static int program_chunk(serial_reader_t *reader, uint32_t address,
                         const unsigned char *data, size_t length)
{
    unsigned char payload[8 + CHUNK_SIZE];
    uint32_t size_with_crc;

    put_be32(payload, address);
    put_be32(payload + 4, (uint32_t)length / 4);
    memcpy(payload + 8, data, length);
    if (send_command(reader->fd, DLOAD_CMD, payload, 8 + length) != 0 ||
        expect_ack(reader, 1000) != 0)
        return -1;

    size_with_crc = ((uint32_t)crc8(data, length) << 24) | (uint32_t)length;
    put_be32(payload + 4, size_with_crc);
    if (send_command(reader->fd, EXECPROG_CMD, payload, 8) != 0 ||
        expect_ack(reader, 1000) != 0)
        return -1;
    return 0;
}

int serial_flash_comboot(const char *device, unsigned int baud,
                         const char *image_path, uint32_t address,
                         uint32_t erase_size, int show_log,
                         serial_log_monitor_t **monitor_out)
{
    unsigned char chunk[CHUNK_SIZE], payload[8];
    serial_reader_t local_reader = {.fd = -1, .device = device, .baud = baud,
                                    .show_log = show_log,
                                    .log_line_start = 1};
    serial_reader_t *reader = &local_reader;
    progress_t progress = {0};
    struct stat info;
    uint32_t offset = 0;
    size_t length;
    FILE *image = NULL;
    int fd = -1;
    int ret = -1;

    if (monitor_out != NULL) {
        *monitor_out = NULL;
        reader = malloc(sizeof(*reader));
        if (reader == NULL)
            return -1;
        *reader = local_reader;
    }
    if (stat(image_path, &info) != 0 || info.st_size <= 0 ||
        (uint64_t)info.st_size > erase_size || erase_size % 4 != 0) {
        fprintf(stderr, "invalid comboot image size or erase range\n");
        goto out;
    }
    image = fopen(image_path, "rb");
    if (image == NULL)
        goto out;
    fd = open_serial(device, baud, 0);
    if (fd < 0)
        goto out;
    debug_log("serial opened: %s at %u baud; comboot=%s (%lld bytes)",
              device, baud, image_path, (long long)info.st_size);
    reader->fd = fd;
    if (handshake(reader) != 0)
        goto out;

    put_be32(payload, address);
    put_be32(payload + 4, erase_size / 4);
    printf("Serial erase comboot at 0x%08x, size 0x%x\n", address, erase_size);
    debug_log("sending serial ERASE command; waiting up to 180 s for ACK");
    if (send_command(fd, ERASE_CMD, payload, sizeof(payload)) != 0 ||
        expect_ack(reader, 180000) != 0)
        goto out;

    progress_start(&progress, "Serial comboot", (uint64_t)info.st_size);
    debug_log("serial comboot transfer started");
    reader->progress_active = 1;
    while ((length = fread(chunk, 1, sizeof(chunk), image)) != 0) {
        size_t padded = (length + 3) & ~(size_t)3;
        if ((uint64_t)offset + padded > erase_size)
            goto out;
        memset(chunk + length, 0xff, padded - length);
        if (program_chunk(reader, address + offset, chunk, padded) != 0)
            goto out;
        offset += (uint32_t)padded;
        progress_update(&progress, offset);
    }
    if (ferror(image) || offset != ((uint64_t)info.st_size + 3) / 4 * 4)
        goto out;
    progress_finish(&progress);
    reader->progress_active = 0;
    debug_log("serial comboot transfer complete (%u bytes)", offset);

    put_be32(payload, address);
    debug_log("sending serial RUN command at 0x%08x", address);
    if (send_command(fd, RUN_CMD, payload, 4) != 0 ||
        expect_ack(reader, 1000) != 0)
        goto out;
    printf("Comboot started\n");
    ret = 0;
out:
    finish_serial_log_line(reader);
    progress_abort(&progress);
    if (ret == 0 && monitor_out != NULL) {
        *monitor_out = reader;
        fd = -1;
    } else if (reader != &local_reader) {
        free(reader);
    }
    if (fd >= 0)
        close(fd);
    if (image != NULL)
        fclose(image);
    return ret;
}
