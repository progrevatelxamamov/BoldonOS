#ifndef BOLDON_CAPABILITY_H
#define BOLDON_CAPABILITY_H
#include <stdint.h>

/* Only privileges enforced in BoldonOS are advertised to Linux programs. */
#define BOLDON_CAP_DAC_OVERRIDE (1ULL << 1)
#define BOLDON_CAP_SETGID       (1ULL << 6)
#define BOLDON_CAP_SETUID       (1ULL << 7)
#define BOLDON_CAP_SETPCAP      (1ULL << 8)
#define BOLDON_CAP_SUPPORTED (BOLDON_CAP_DAC_OVERRIDE | BOLDON_CAP_SETGID | \
                              BOLDON_CAP_SETUID | BOLDON_CAP_SETPCAP)
#endif
