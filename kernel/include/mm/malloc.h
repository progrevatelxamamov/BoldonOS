#ifndef MALLOC_H
#define MALLOC_H

#include <stdint.h>

void malloc_init(void);
void *malloc_alloc(uint64_t size);
void malloc_free(void *ptr);

#endif