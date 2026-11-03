#ifndef BOLDON_CAPABILITY_H
#define BOLDON_CAPABILITY_H
#include <stdint.h>

/* Only privileges enforced in BoldonOS are advertised to Linux programs. */
#define BOLDON_CAP_DAC_OVERRIDE (1ULL << 1)
#define BOLDON_CAP_FOWNER       (1ULL << 3)
#define BOLDON_CAP_KILL         (1ULL << 5)
#define BOLDON_CAP_SETGID       (1ULL << 6)
#define BOLDON_CAP_SETUID       (1ULL << 7)
#define BOLDON_CAP_SETPCAP      (1ULL << 8)
#define BOLDON_CAP_SYS_ADMIN    (1ULL << 21)
#define BOLDON_CAP_SYS_BOOT     (1ULL << 22)
#define BOLDON_CAP_SUPPORTED (BOLDON_CAP_DAC_OVERRIDE | BOLDON_CAP_FOWNER | \
                              BOLDON_CAP_KILL | BOLDON_CAP_SETGID | \
                              BOLDON_CAP_SETUID | BOLDON_CAP_SETPCAP | \
                              BOLDON_CAP_SYS_ADMIN | BOLDON_CAP_SYS_BOOT)
#endif
