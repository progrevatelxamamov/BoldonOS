#ifndef STDLIB_H
#define STDLIB_H

#include <stddef.h>
#include <mm/malloc.h>

static inline void *malloc(size_t size) {
    return malloc_alloc(size);
}

static inline void free(void *ptr) {
    malloc_free(ptr);
}

void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *));

#endif