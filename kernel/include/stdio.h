#ifndef STDIO_H
#define STDIO_H

#include <stddef.h>

typedef void FILE;
#define stdout ((FILE *)1)
#define stderr ((FILE *)2)
#define stdin  ((FILE *)0)

int printf(const char *format, ...);
int sprintf(char *str, const char *format, ...);
int snprintf(char *str, size_t size, const char *format, ...);
int fflush(FILE *stream);

#endif