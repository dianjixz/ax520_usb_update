#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include "usb_update.h"
#include <time.h>
#include <limits.h>
#include <errno.h>

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

#define TIMEOUT_SECONDS 60

int read_hex_short(char *path, unsigned short *value)
{
    FILE *fp = fopen(path, "r");
    if (!fp)
        return -1;

    char buf[16];
    if (!fgets(buf, sizeof(buf), fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    // 去掉换行符
    buf[strcspn(buf, "\n")] = 0;

    // 16进制转换
    unsigned int temp;
    if (sscanf(buf, "%x", &temp) != 1)
        return -1;
    if (temp > 0xFFFF)
        return -1;

    *value = (unsigned short)temp;
    return 0;
}

int is_usb_device_dir(const char *name)
{
    // USB device dirs一般格式如 "1-1", "2-1.3", 排除非设备目录如 "usb1", "usb2"
    // 简单判断：名称中有 '-' 视为设备目录
    return strchr(name, '-') != NULL;
}

int check_usb_device_present(void)
{
    struct dirent *entry;
    unsigned short usbup_vid = 0, usbup_pid = 0;
    char           vid_path[PATH_MAX], pid_path[PATH_MAX];

    DIR *dir = opendir(SYS_USB_PATH);
    if (!dir) {
        fprintf(stderr, "opendir\n");
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (!is_usb_device_dir(entry->d_name))
            continue;

        snprintf(vid_path, sizeof(vid_path), "%s/%s/idVendor",
                 SYS_USB_PATH, entry->d_name);
        snprintf(pid_path, sizeof(pid_path), "%s/%s/idProduct",
                 SYS_USB_PATH, entry->d_name);

        if (read_hex_short(vid_path, &usbup_vid) == 0 && read_hex_short(pid_path, &usbup_pid) == 0) {
            if ((usbup_vid == VENDOR_ID) && (usbup_pid == PRODUCT_ID)) {
                closedir(dir);
                return 1;
            }
        }
    }

    closedir(dir);
    return 0;
}

int libusb_get_vid_pid(void)
{
    time_t start = time(NULL);

    while (1) {
        if (check_usb_device_present() > 0) {
            printf("device detected\n");
            return 0;
        }

        if (time(NULL) - start >= TIMEOUT_SECONDS) {
            printf("device detected timeout %d\n", TIMEOUT_SECONDS);
            return 1;
        }

        /* Avoid a tight polling loop while the board re-enumerates. */
        usleep(10000);
    }

    return 1;
}
