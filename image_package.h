#ifndef IMAGE_PACKAGE_H
#define IMAGE_PACKAGE_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

#define IMAGE_MAX_COUNT 16

typedef struct image_entry {
    char partition[32];
    char name[128];
    char path[PATH_MAX];
    uint32_t address;
    uint32_t declared_size;
    uint32_t file_size;
    uint32_t erase_size;
} image_entry_t;

typedef struct image_package {
    char temp_dir[PATH_MAX];
    image_entry_t entries[IMAGE_MAX_COUNT];
    size_t count;
    size_t comboot_index;
} image_package_t;

int image_package_load(image_package_t *package, const char *zip_path);
void image_package_release(image_package_t *package);

#endif
