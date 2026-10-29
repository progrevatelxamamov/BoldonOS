#include <stddef.h>
void *lp_malloc(size_t size);
void *lp_calloc(size_t count, size_t size);
void *lp_memalign(size_t align, size_t size);
void lp_free(void *ptr);
#define malloc lp_malloc
#define calloc lp_calloc
#define memalign lp_memalign
#define free lp_free
