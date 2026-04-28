#include "util.h"

#include <stdio.h>
#include <stdlib.h>

volatile sig_atomic_t quit_flag;

static void die(const char *msg)
{
	fprintf(stderr, "wat: %s\n", msg);
	exit(1);
}

void *xmalloc(size_t n)
{
	void *p = malloc(n);
	if (!p) die("out of memory");
	return p;
}

void *xrealloc(void *p, size_t n)
{
	p = realloc(p, n);
	if (!p) die("out of memory");
	return p;
}
