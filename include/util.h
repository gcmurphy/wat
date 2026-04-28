#ifndef WAT_UTIL_H
#define WAT_UTIL_H

#include <stddef.h>
#include <signal.h>

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);

/* shared sigint flag, set by SIGINT handler */
extern volatile sig_atomic_t quit_flag;

#endif
