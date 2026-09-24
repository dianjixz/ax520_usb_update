#ifndef PROGRESS_H
#define PROGRESS_H

#include <stdint.h>

typedef struct progress {
    char label[64];
    uint64_t total;
    int last_percent;
    int active;
    int terminal;
} progress_t;

void progress_start(progress_t *progress, const char *label, uint64_t total);
void progress_update(progress_t *progress, uint64_t current);
void progress_finish(progress_t *progress);
void progress_abort(progress_t *progress);

#endif
