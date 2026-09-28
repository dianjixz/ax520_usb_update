#define _DEFAULT_SOURCE
#include "usb_update.h"
#include "debug.h"

#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define USB_WAIT_TIMEOUT_MS 120000
#define USB_POLL_INTERVAL_US 5000

static int64_t elapsed_ms(const struct timespec *start)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return -1;
    return (int64_t)(now.tv_sec - start->tv_sec) * 1000 +
           (now.tv_nsec - start->tv_nsec) / 1000000;
}

int libusb_wait_open_device(libusb_context *context,
                            libusb_device_handle **handle,
                            void (*on_wait)(void *), void *callback_context)
{
    struct timespec started;
    int64_t elapsed, next_log = 0;
    libusb_device **devices;
    struct libusb_device_descriptor descriptor;
    ssize_t count, i;
    int ret;

    *handle = NULL;
    if (clock_gettime(CLOCK_MONOTONIC, &started) != 0)
        return LIBUSB_ERROR_OTHER;

    for (;;) {
        if (on_wait != NULL)
            on_wait(callback_context);
        count = libusb_get_device_list(context, &devices);
        if (count < 0)
            return (int)count;
        for (i = 0; i < count; i++) {
            if (libusb_get_device_descriptor(devices[i], &descriptor) != 0 ||
                descriptor.idVendor != VENDOR_ID ||
                descriptor.idProduct != PRODUCT_ID)
                continue;

            debug_log("USB 32c9:1000 enumerated on bus %u, address %u; opening now",
                      libusb_get_bus_number(devices[i]),
                      libusb_get_device_address(devices[i]));
            ret = libusb_open(devices[i], handle);
            libusb_free_device_list(devices, 1);
            if (ret == 0)
                return 0;
            if (ret != LIBUSB_ERROR_NO_DEVICE)
                return ret;
            break;
        }
        if (i == count)
            libusb_free_device_list(devices, 1);

        elapsed = elapsed_ms(&started);
        if (elapsed < 0)
            return LIBUSB_ERROR_OTHER;
        if (elapsed >= USB_WAIT_TIMEOUT_MS) {
            fprintf(stderr, "USB device 32c9:1000 not detected within 60 s\n");
            return LIBUSB_ERROR_TIMEOUT;
        }
        if (elapsed >= next_log) {
            debug_log("waiting for USB 32c9:1000 (%lld/60 s elapsed)",
                      (long long)(elapsed / 1000));
            next_log = elapsed + 5000;
        }
        usleep(USB_POLL_INTERVAL_US);
    }
}
