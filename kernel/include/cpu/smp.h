#ifndef SMP_H
#define SMP_H

#include <stdint.h>

void smp_init(void);
extern volatile uint32_t ap_cores_booted;

#endif