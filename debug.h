#ifndef DEBUG_H
#define DEBUG_H

void debug_enable(void);
int debug_enabled(void);
void debug_log(const char *format, ...);

#endif
