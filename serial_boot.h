#ifndef SERIAL_BOOT_H
#define SERIAL_BOOT_H

#include <stdint.h>

typedef struct serial_log_monitor serial_log_monitor_t;

int serial_baud_supported(unsigned int baud);
int serial_flash_comboot(const char *device, unsigned int baud,
                         const char *image_path, uint32_t address,
                         uint32_t erase_size, int show_log,
                         serial_log_monitor_t **monitor_out);
void serial_log_drain(serial_log_monitor_t *monitor);
void serial_log_close(serial_log_monitor_t *monitor);

#endif
