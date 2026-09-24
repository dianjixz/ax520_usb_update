#define _DEFAULT_SOURCE
#include "serial_boot.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
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

static int open_serial(const char *device, unsigned int baud)
{
    struct termios tty;
    speed_t speed;
    int fd;

    if (baud_speed(baud, &speed) != 0)
        return -1;
    fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", device, strerror(errno));
        return -1;
    }
    if (tcgetattr(fd, &tty) != 0) {
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
    int ret;

    for (;;) {
        now = now_ms();
        if (now < 0 || now >= deadline)
            return -1;
        ret = poll(&pfd, 1, (int)(deadline - now));
        if (ret > 0)
            return (pfd.revents & events) ? 0 : -1;
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

static int receive_command(int fd, int64_t deadline)
{
    unsigned char byte, command, checksum;
    while (read_byte(fd, &byte, deadline) == 0) {
        if (byte != START_BYTE)
            continue;
        if (read_byte(fd, &command, deadline) != 0 ||
            read_byte(fd, &checksum, deadline) != 0 ||
            read_byte(fd, &byte, deadline) != 0)
            return -1;
        if (byte == END_BYTE && checksum == 0)
            return command;
    }
    return -1;
}

static int expect_ack(int fd, unsigned int timeout_ms)
{
    int response = receive_command(fd, now_ms() + timeout_ms);
    if (response != ACK_OK) {
        fprintf(stderr, "serial ACK error: %d\n", response);
        return -1;
    }
    return 0;
}

static int handshake(int fd)
{
    int64_t deadline = now_ms() + 10000;
    int response;

    while ((response = receive_command(fd, deadline)) >= 0) {
        if (response != HDBOOT_NOTIFY && response != MINIBOOT_NOTIFY)
            continue;
        printf("Serial boot notify: %s\n",
               response == HDBOOT_NOTIFY ? "HDBOOT" : "MINIBOOT");
        if (send_command(fd, DEBUG_CMD, NULL, 0) != 0)
            return -1;
        if (response == MINIBOOT_NOTIFY) {
            /* Some MINIBOOT versions acknowledge DEBUG and others do not. */
            response = receive_command(fd, now_ms() + 200);
            return response == -1 || response == ACK_OK ? 0 : -1;
        }
        return expect_ack(fd, 1000);
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

static int program_chunk(int fd, uint32_t address,
                         const unsigned char *data, size_t length)
{
    unsigned char payload[8 + CHUNK_SIZE];
    uint32_t size_with_crc;

    put_be32(payload, address);
    put_be32(payload + 4, (uint32_t)length / 4);
    memcpy(payload + 8, data, length);
    if (send_command(fd, DLOAD_CMD, payload, 8 + length) != 0 ||
        expect_ack(fd, 1000) != 0)
        return -1;

    size_with_crc = ((uint32_t)crc8(data, length) << 24) | (uint32_t)length;
    put_be32(payload + 4, size_with_crc);
    if (send_command(fd, EXECPROG_CMD, payload, 8) != 0 ||
        expect_ack(fd, 1000) != 0)
        return -1;
    return 0;
}

int serial_flash_comboot(const char *device, unsigned int baud,
                         const char *image_path, uint32_t address,
                         uint32_t erase_size)
{
    unsigned char chunk[CHUNK_SIZE], payload[8];
    struct stat info;
    uint32_t offset = 0;
    size_t length;
    FILE *image = NULL;
    int fd = -1;
    int ret = -1;

    if (stat(image_path, &info) != 0 || info.st_size <= 0 ||
        (uint64_t)info.st_size > erase_size || erase_size % 4 != 0) {
        fprintf(stderr, "invalid comboot image size or erase range\n");
        return -1;
    }
    image = fopen(image_path, "rb");
    if (image == NULL)
        return -1;
    fd = open_serial(device, baud);
    if (fd < 0)
        goto out;
    if (handshake(fd) != 0)
        goto out;

    put_be32(payload, address);
    put_be32(payload + 4, erase_size / 4);
    printf("Serial erase comboot at 0x%08x, size 0x%x\n", address, erase_size);
    if (send_command(fd, ERASE_CMD, payload, sizeof(payload)) != 0 ||
        expect_ack(fd, 180000) != 0)
        goto out;

    while ((length = fread(chunk, 1, sizeof(chunk), image)) != 0) {
        size_t padded = (length + 3) & ~(size_t)3;
        if ((uint64_t)offset + padded > erase_size)
            goto out;
        memset(chunk + length, 0xff, padded - length);
        if (program_chunk(fd, address + offset, chunk, padded) != 0)
            goto out;
        offset += (uint32_t)padded;
        if (offset % (32 * 1024) == 0 || offset >= (uint32_t)info.st_size)
            printf("Serial comboot: %u/%lld bytes\n",
                   offset > (uint32_t)info.st_size ? (uint32_t)info.st_size : offset,
                   (long long)info.st_size);
    }
    if (ferror(image) || offset != ((uint64_t)info.st_size + 3) / 4 * 4)
        goto out;

    put_be32(payload, address);
    if (send_command(fd, RUN_CMD, payload, 4) != 0 ||
        expect_ack(fd, 1000) != 0)
        goto out;
    printf("Comboot started; waiting for USB download device\n");
    ret = 0;
out:
    if (fd >= 0)
        close(fd);
    fclose(image);
    return ret;
}
