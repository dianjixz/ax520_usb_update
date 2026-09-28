#define _DEFAULT_SOURCE
#include "image_package.h"
#include "debug.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_IMAGE_SIZE (16U * 1024U * 1024U)
#define FLASH_START 0x03000000U
#define FLASH_END   0x04000000U
#define COMBOOT_ADDRESS 0x03000000U
#define COMBOOT_CAPACITY 0x00030000U

static const char *manifest_filter =
    "if .IMAGE_TYPE != \"SNOR\" or (.IMAGE_LIST | type) != \"array\" "
    "then error(\"expected SNOR IMAGE_LIST\") "
    "else .IMAGE_LIST[] | [.PARTITION, .IMAGE_NAME, .IMAGE_ADDR, "
    ".IMAGE_SIZE] | @tsv end";

static int write_all(int fd, const char *data, size_t length)
{
    size_t done = 0;
    ssize_t count;
    while (done < length) {
        count = write(fd, data + done, length - done);
        if (count > 0)
            done += (size_t)count;
        else if (count == 0 || (errno != EINTR && errno != EAGAIN))
            return -1;
    }
    return 0;
}

static int run_to_file(const char *program, char *const args[],
                       const char *input, const char *output, size_t limit)
{
    char buffer[8192];
    int pipefd[2], output_fd, status = 0;
    size_t total = 0;
    ssize_t count;
    pid_t child;
    int failed = 0, waited = 0;

    output_fd = open(output, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (output_fd < 0)
        return -1;
    if (pipe(pipefd) != 0) {
        close(output_fd);
        return -1;
    }
    child = fork();
    if (child == 0) {
        int input_fd = -1;
        close(pipefd[0]);
        if (input != NULL)
            input_fd = open(input, O_RDONLY);
        if (input != NULL && input_fd < 0)
            _exit(127);
        if (input_fd >= 0) {
            dup2(input_fd, STDIN_FILENO);
            close(input_fd);
        }
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        execvp(program, args);
        _exit(127);
    }
    close(pipefd[1]);
    if (child < 0) {
        close(pipefd[0]);
        close(output_fd);
        return -1;
    }

    while ((count = read(pipefd[0], buffer, sizeof(buffer))) != 0) {
        if (count < 0) {
            if (errno == EINTR)
                continue;
            failed = 1;
            break;
        }
        if ((size_t)count > limit - total ||
            write_all(output_fd, buffer, (size_t)count) != 0) {
            failed = 1;
            break;
        }
        total += (size_t)count;
    }
    close(pipefd[0]);
    close(output_fd);
    for (;;) {
        if (waitpid(child, &status, 0) >= 0) {
            waited = 1;
            break;
        }
        if (errno != EINTR)
            break;
    }
    if (failed || !waited || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0 || total == 0) {
        if (waited && WIFEXITED(status) && WEXITSTATUS(status) == 127)
            fprintf(stderr, "required command not available: %s\n", program);
        unlink(output);
        return -1;
    }
    return 0;
}

static int safe_name(const char *name)
{
    const unsigned char *p = (const unsigned char *)name;
    if (*p == '\0' || name[0] == '.')
        return 0;
    for (; *p != '\0'; p++) {
        if (!isalnum(*p) && *p != '.' && *p != '_' && *p != '-')
            return 0;
    }
    return 1;
}

static int parse_u32(const char *text, uint32_t *value)
{
    unsigned long parsed;
    char *end;
    errno = 0;
    parsed = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || *text == '-' ||
        parsed == 0 || parsed > UINT32_MAX)
        return -1;
    *value = (uint32_t)parsed;
    return 0;
}

static int copy_field(char *dest, size_t capacity, const char *source)
{
    size_t length = strlen(source);
    if (length == 0 || length >= capacity)
        return -1;
    memcpy(dest, source, length + 1);
    return 0;
}

static int parse_manifest(image_package_t *package, const char *path)
{
    char output[PATH_MAX];
    char *args[] = {"jq", "-er", (char *)manifest_filter, NULL};
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    FILE *stream = NULL;
    int ret = -1;

    if (snprintf(output, sizeof(output), "%s/entries.tsv", package->temp_dir)
        >= (int)sizeof(output) ||
        run_to_file("jq", args, path, output, 64 * 1024) != 0)
        return -1;
    stream = fopen(output, "r");
    if (stream == NULL)
        goto out;
    while ((length = getline(&line, &capacity, stream)) >= 0) {
        image_entry_t *entry;
        char *fields[4], *cursor = line;
        size_t i;

        if (package->count >= IMAGE_MAX_COUNT || length > 512)
            goto out;
        if (length > 0 && line[length - 1] == '\n')
            line[length - 1] = '\0';
        for (i = 0; i < 4; i++) {
            fields[i] = strsep(&cursor, "\t");
            if (fields[i] == NULL)
                goto out;
        }
        if (cursor != NULL)
            goto out;
        entry = &package->entries[package->count];
        if (copy_field(entry->partition, sizeof(entry->partition), fields[0]) != 0 ||
            copy_field(entry->name, sizeof(entry->name), fields[1]) != 0 ||
            !safe_name(entry->name) ||
            parse_u32(fields[2], &entry->address) != 0 ||
            parse_u32(fields[3], &entry->declared_size) != 0 ||
            entry->declared_size > MAX_IMAGE_SIZE ||
            entry->address < FLASH_START ||
            (uint64_t)entry->address + entry->declared_size > FLASH_END)
            goto out;
        for (i = 0; i < package->count; i++) {
            const image_entry_t *other = &package->entries[i];
            if (strcmp(entry->name, other->name) == 0 ||
                strcmp(entry->partition, other->partition) == 0 ||
                ((uint64_t)entry->address <
                 (uint64_t)other->address + other->declared_size &&
                 (uint64_t)other->address <
                 (uint64_t)entry->address + entry->declared_size))
                goto out;
        }
        if (strcmp(entry->partition, "comboot") == 0) {
            if (entry->address != COMBOOT_ADDRESS ||
                entry->declared_size > COMBOOT_CAPACITY)
                goto out;
            package->comboot_index = package->count;
        }
        package->count++;
    }
    if (ferror(stream) || package->count < 2 ||
        package->comboot_index == IMAGE_MAX_COUNT)
        goto out;
    ret = 0;
out:
    free(line);
    if (stream != NULL)
        fclose(stream);
    unlink(output);
    return ret;
}

static int set_partition_sizes(image_package_t *package)
{
    size_t i, j;
    for (i = 0; i < package->count; i++) {
        image_entry_t *entry = &package->entries[i];
        uint32_t end = FLASH_END;
        for (j = 0; j < package->count; j++) {
            uint32_t next = package->entries[j].address;
            if (next > entry->address && next < end)
                end = next;
        }
        entry->erase_size = end - entry->address;
        if (entry->declared_size > entry->erase_size ||
            (strcmp(entry->partition, "comboot") == 0 &&
             entry->erase_size != COMBOOT_CAPACITY))
            return -1;
    }
    return 0;
}

void image_package_release(image_package_t *package)
{
    size_t i;
    char path[PATH_MAX];

    if (package->temp_dir[0] == '\0')
        return;
    for (i = 0; i < package->count; i++) {
        if (package->entries[i].path[0] != '\0')
            unlink(package->entries[i].path);
    }
    if (snprintf(path, sizeof(path), "%s/ax520_image.json", package->temp_dir)
        < (int)sizeof(path))
        unlink(path);
    if (snprintf(path, sizeof(path), "%s/entries.tsv", package->temp_dir)
        < (int)sizeof(path))
        unlink(path);
    rmdir(package->temp_dir);
    package->temp_dir[0] = '\0';
}

int image_package_load(image_package_t *package, const char *zip_path)
{
    char archive[PATH_MAX], manifest[PATH_MAX];
    char *manifest_args[] = {"unzip", "-p", archive,
                             "ax520_image.json", NULL};
    struct stat info;
    size_t i;

    memset(package, 0, sizeof(*package));
    package->comboot_index = IMAGE_MAX_COUNT;
    if (realpath(zip_path, archive) == NULL ||
        snprintf(package->temp_dir, sizeof(package->temp_dir),
                 "/tmp/ax520-update-XXXXXX") >= (int)sizeof(package->temp_dir) ||
        mkdtemp(package->temp_dir) == NULL) {
        fprintf(stderr, "cannot open ZIP or create temporary directory\n");
        return -1;
    }
    debug_log("ZIP opened: %s; temporary directory %s", archive,
              package->temp_dir);
    debug_log("extracting and validating ax520_image.json");
    if (snprintf(manifest, sizeof(manifest), "%s/ax520_image.json",
                 package->temp_dir) >= (int)sizeof(manifest) ||
        run_to_file("unzip", manifest_args, NULL, manifest, 64 * 1024) != 0 ||
        parse_manifest(package, manifest) != 0 ||
        set_partition_sizes(package) != 0) {
        fprintf(stderr, "invalid ax520_image.json in ZIP\n");
        goto fail;
    }
    debug_log("ZIP manifest validated: %zu images", package->count);
    for (i = 0; i < package->count; i++) {
        image_entry_t *entry = &package->entries[i];
        char *image_args[] = {"unzip", "-p", archive, entry->name, NULL};

        debug_log("extracting ZIP image %zu/%zu: %s", i + 1,
                  package->count, entry->name);
        if (snprintf(entry->path, sizeof(entry->path), "%s/%s",
                     package->temp_dir, entry->name) >= (int)sizeof(entry->path) ||
            run_to_file("unzip", image_args, NULL, entry->path,
                        entry->erase_size) != 0 ||
            stat(entry->path, &info) != 0 || info.st_size <= 0 ||
            (uint64_t)info.st_size > entry->erase_size) {
            fprintf(stderr, "invalid ZIP image: %s\n", entry->name);
            goto fail;
        }
        entry->file_size = (uint32_t)info.st_size;
        debug_log("validated %s: %u bytes, address=0x%08x, erase=0x%x",
                  entry->name, entry->file_size, entry->address,
                  entry->erase_size);
        if (entry->file_size > entry->declared_size)
            fprintf(stderr,
                    "warning: %s is %u bytes, larger than IMAGE_SIZE 0x%x; "
                    "using partition boundary 0x%x\n",
                    entry->name, entry->file_size, entry->declared_size,
                    entry->erase_size);
    }
    return 0;
fail:
    image_package_release(package);
    return -1;
}
