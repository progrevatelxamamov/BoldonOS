#ifndef APIC_H
#define APIC_H

#include <stdint.h>
#include <stdbool.h>

#define APIC_MAX_CPUS 256

#define IA32_APIC_BASE_MSR 0x1B
#define IA32_APIC_BASE_MSR_BSP 0x100
#define IA32_APIC_BASE_MSR_ENABLE 0x800
#define IA32_APIC_BASE_MSR_X2APIC 0x400

#define APIC_REG(off, base) (base[(off) / 4])

#define APIC_ID 0x020
#define APIC_TPR 0x080
#define APIC_EOI_REG 0x0B0
#define APIC_SVR 0x0F0
#define APIC_ICR_LOW 0x300
#define APIC_ICR_HIGH 0x310
#define APIC_LVT_TIMER 0x320
#define APIC_LVT_THERMAL 0x330
#define APIC_LVT_PERFMON 0x340
#define APIC_LVT_LINT0 0x350
#define APIC_LVT_LINT1 0x360
#define APIC_LVT_ERROR 0x370
#define APIC_TIMER_INITCNT 0x380
#define APIC_TIMER_CURCNT 0x390
#define APIC_TIMER_DCR 0x3E0

#define APIC_ICR_DELIVERY_PENDING (1U << 12)
#define APIC_ICR_ASSERT (1U << 14)
#define APIC_ICR_DEST_ALL_EXCLUDING_SELF (3U << 18)

#define LVT_MASKED (1U << 16)
#define LVT_PERIODIC (1U << 17)
#define LVT_LINT_EXTINT 0x700
#define LVT_LINT_NMI 0x400

#define APIC_TIMER_DIV 0x3

#define IPI_RESCHEDULE 0xFD
#define IPI_TLB_FLUSH 0xFC

bool check_apic(void);
int enable_apic(uint32_t *apic_base, uint8_t timer_vector, uint32_t timer_hz);
void apic_init_cpu(uint8_t timer_vector, uint32_t timer_hz);
void apic_timer_unmask_cpu(void);
void apic_send_eoi(void);
uint32_t apic_calibrate(volatile uint32_t *apic_base);
void smp_broadcast_ipi(uint8_t vector);
void smp_send_ipi(int logical_id, uint8_t vector);

static inline void cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx,
                         uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile (
        "cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(0)
    );
}

#endif
