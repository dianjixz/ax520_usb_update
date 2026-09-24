#ifndef SERIAL_BOOT_H
#define SERIAL_BOOT_H

#include <stdint.h>

int serial_baud_supported(unsigned int baud);
int serial_flash_comboot(const char *device, unsigned int baud,
                         const char *image_path, uint32_t address,
                         uint32_t erase_size);

#endif
