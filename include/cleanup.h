#ifndef WAT_CLEANUP_H
#define WAT_CLEANUP_H

/*
 * Scope-bound resource cleanup, à la C23 [[gnu::cleanup(fn)]].
 *
 * GCC/Clang support __attribute__((cleanup(fn))) since GCC 3.3 / any
 * Clang. C23 standardises [[gnu::cleanup(fn)]] as an attribute, but
 * the underlying mechanism is identical, so we keep the C11-compatible
 * spelling and avoid a forced -std=c2x bump.
 *
 * Usage:
 *   _autofclose FILE *f = fopen(...);
 *   _autofree   void *p = malloc(...);
 *   // f is fclose'd, p is free'd at scope exit (incl. early returns).
 *
 * Invariants:
 *   - Cleanup runs even when the variable is NULL (helpers handle it).
 *   - Order is reverse of declaration, mirroring C++ destructors.
 */

#include <stdio.h>
#include <stdlib.h>

#define wat_autocleanup(fn) __attribute__((cleanup(fn)))

static inline void wat_close_filep(FILE **f) { if (*f) fclose(*f); }
static inline void wat_freep(void *pp)
{
	void *p = *(void **)pp;
	if (p) free(p);
}

#define _autofclose wat_autocleanup(wat_close_filep)
#define _autofree   wat_autocleanup(wat_freep)

#endif
