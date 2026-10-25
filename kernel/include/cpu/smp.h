#ifndef SMP_H
#define SMP_H

#include <stdint.h>
#include <stdbool.h>

#define SMP_MAX_CPUS 256

void smp_init(void);
int cpu_get_logical_id(void);
uint32_t smp_cpu_count(void);
uint32_t smp_online_count(void);
uint32_t smp_apic_id(int logical_id);
bool smp_cpu_online(int logical_id);

extern volatile uint32_t ap_cores_booted;

#endif
