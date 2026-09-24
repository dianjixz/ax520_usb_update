#include "progress.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define BAR_WIDTH 20

static void progress_render(progress_t *progress, uint64_t current,
                            int percent)
{
    char bar[BAR_WIDTH + 1];
    int filled = percent * BAR_WIDTH / 100;
    int i;

    for (i = 0; i < BAR_WIDTH; i++)
        bar[i] = i < filled ? '=' : ' ';
    bar[BAR_WIDTH] = '\0';

    if (progress->terminal)
        fputc('\r', stdout);
    printf("%-26.26s [%s] %3d%% %llu/%llu",
           progress->label, bar, percent,
           (unsigned long long)current,
           (unsigned long long)progress->total);
    if (!progress->terminal)
        fputc('\n', stdout);
    fflush(stdout);
    progress->last_percent = percent;
}

void progress_start(progress_t *progress, const char *label, uint64_t total)
{
    memset(progress, 0, sizeof(*progress));
    snprintf(progress->label, sizeof(progress->label), "%s", label);
    progress->total = total;
    progress->last_percent = -1;
    progress->terminal = isatty(STDOUT_FILENO);
    progress->active = 1;
    progress_update(progress, 0);
}

void progress_update(progress_t *progress, uint64_t current)
{
    int percent;

    if (!progress->active || progress->total == 0)
        return;
    if (current > progress->total)
        current = progress->total;
    percent = (int)(current * 100 / progress->total);
    if (percent == progress->last_percent ||
        (!progress->terminal && percent != 100 &&
         progress->last_percent >= 0 &&
         percent / 10 == progress->last_percent / 10))
        return;
    progress_render(progress, current, percent);
}

void progress_finish(progress_t *progress)
{
    if (!progress->active)
        return;
    if (progress->last_percent != 100)
        progress_render(progress, progress->total, 100);
    if (progress->terminal)
        fputc('\n', stdout);
    fflush(stdout);
    progress->active = 0;
}

void progress_abort(progress_t *progress)
{
    if (!progress->active)
        return;
    if (progress->terminal) {
        fputc('\n', stdout);
        fflush(stdout);
    }
    progress->active = 0;
}
