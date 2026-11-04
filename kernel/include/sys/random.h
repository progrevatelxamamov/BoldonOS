#ifndef BOLDON_RANDOM_H
#define BOLDON_RANDOM_H

#include <stdint.h>

/* Best-effort byte stream; without hardware entropy this is NOT a CSPRNG. */
void random_fill(void *buf, uint64_t count);

#endif
