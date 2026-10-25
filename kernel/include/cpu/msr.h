#ifndef CPU_MSR_H
#define CPU_MSR_H

#include <stdint.h>

static inline void cpuGetMSR(uint32_t msr, uint32_t *eax, uint32_t *edx) {
    __asm__ volatile ("rdmsr" : "=a"(*eax), "=d"(*edx) : "c"(msr));
}

static inline void cpuSetMSR(uint32_t msr, uint32_t eax, uint32_t edx) {
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"(eax), "d"(edx));
}

#endif