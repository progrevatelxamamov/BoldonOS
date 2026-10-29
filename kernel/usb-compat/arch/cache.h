#include <stddef.h>
static inline void dcache_clean(const void *p, size_t n) { (void)p; (void)n; __asm__ volatile("mfence" ::: "memory"); }
static inline void dcache_invalidate(const void *p, size_t n) { (void)p; (void)n; __asm__ volatile("mfence" ::: "memory"); }
