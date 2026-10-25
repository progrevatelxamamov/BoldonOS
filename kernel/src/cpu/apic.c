#include <cpu/apic.h>
#include <cpu/acpi.h>
#include <cpu/smp.h>
#include <io.h>
#include <bootloader.h>
#include <kernel.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

static uint32_t apic_timer_initial_count = 0;
static uint8_t apic_timer_vector = 0;

static volatile uint32_t *apic_local_base(void) {
    uint64_t phys = rdmsr(IA32_APIC_BASE_MSR) & 0xFFFFF000ULL;
    if (!phys) phys = 0xFEE00000ULL;
    bootloader_t bi = get_bootinfo();
    uint64_t offset = bi.hhdm ? bi.hhdm->offset : 0;
    return (volatile uint32_t *)(phys + offset);
}

static inline void lapic_write(uint32_t reg, uint32_t val) {
    APIC_REG(reg, apic_local_base()) = val;
}

static inline uint32_t lapic_read(uint32_t reg) {
    return APIC_REG(reg, apic_local_base());
}

bool check_apic(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    return (edx & (1U << 9)) != 0;
}

static void apic_wait_icr_idle(void) {
    for (int i = 0; i < 100000; i++) {
        if ((lapic_read(APIC_ICR_LOW) & APIC_ICR_DELIVERY_PENDING) == 0) return;
        __asm__ volatile("pause");
    }
}

void smp_broadcast_ipi(uint8_t vector) {
    if (smp_online_count() < 2) return;
    apic_wait_icr_idle();
    lapic_write(APIC_ICR_LOW, APIC_ICR_DEST_ALL_EXCLUDING_SELF | APIC_ICR_ASSERT | vector);
}

void smp_send_ipi(int logical_id, uint8_t vector) {
    uint32_t apic_id = smp_apic_id(logical_id);
    if (apic_id > 0xFFu) return;
    if (!smp_cpu_online(logical_id)) return;
    apic_wait_icr_idle();
    lapic_write(APIC_ICR_HIGH, apic_id << 24);
    lapic_write(APIC_ICR_LOW, APIC_ICR_ASSERT | vector);
}

uint32_t apic_calibrate(volatile uint32_t *apic_base) {
    APIC_REG(APIC_TIMER_DCR, apic_base) =
        (APIC_REG(APIC_TIMER_DCR, apic_base) & ~0xFu) | APIC_TIMER_DIV;
    APIC_REG(APIC_LVT_TIMER, apic_base) = LVT_MASKED;
    APIC_REG(APIC_TIMER_INITCNT, apic_base) = 0xFFFFFFFFu;

    hpet_sleep_ms(10);

    uint32_t current = APIC_REG(APIC_TIMER_CURCNT, apic_base);
    uint32_t elapsed_ticks = 0xFFFFFFFFu - current;

    if (elapsed_ticks < 100) return 100;
    return elapsed_ticks / 10;
}

static uint32_t apic_timer_count_for(uint32_t ticks_per_ms, uint32_t hz) {
    if (hz == 0) hz = 100;
    uint32_t count = (ticks_per_ms * 1000u) / hz;
    return count ? count : 1;
}

static void apic_timer_program(volatile uint32_t *apic_base, bool masked) {
    APIC_REG(APIC_TIMER_DCR, apic_base) =
        (APIC_REG(APIC_TIMER_DCR, apic_base) & ~0xFu) | APIC_TIMER_DIV;
    uint32_t lvt = (uint32_t)apic_timer_vector | LVT_PERIODIC;
    if (masked) lvt |= LVT_MASKED;
    APIC_REG(APIC_LVT_TIMER, apic_base) = lvt;
    APIC_REG(APIC_TIMER_INITCNT, apic_base) = apic_timer_initial_count;
}

static void apic_msr_enable(void) {
    uint64_t base = rdmsr(IA32_APIC_BASE_MSR);
    if (base & IA32_APIC_BASE_MSR_X2APIC) {
        base &= ~(IA32_APIC_BASE_MSR_X2APIC | IA32_APIC_BASE_MSR_ENABLE);
        wrmsr(IA32_APIC_BASE_MSR, base);
        base = rdmsr(IA32_APIC_BASE_MSR);
    }
    base |= IA32_APIC_BASE_MSR_ENABLE;
    wrmsr(IA32_APIC_BASE_MSR, base);
}

static void apic_init_svr(volatile uint32_t *apic_base) {
    uint32_t svr = APIC_REG(APIC_SVR, apic_base);
    svr &= ~0xFFu;
    svr |= 0xFFu;
    svr |= 0x100u;
    APIC_REG(APIC_SVR, apic_base) = svr;
}

static void apic_init_lvt(volatile uint32_t *apic_base, bool is_bsp) {
    APIC_REG(APIC_LVT_LINT0, apic_base) = is_bsp ? LVT_LINT_EXTINT : LVT_MASKED;
    APIC_REG(APIC_LVT_LINT1, apic_base) = LVT_LINT_NMI;
    APIC_REG(APIC_LVT_ERROR, apic_base) = LVT_MASKED;
    APIC_REG(APIC_LVT_THERMAL, apic_base) = LVT_MASKED;
    APIC_REG(APIC_LVT_PERFMON, apic_base) = LVT_MASKED;
    APIC_REG(APIC_TPR, apic_base) = 0;
}

int enable_apic(uint32_t *apic_base, uint8_t timer_vector, uint32_t timer_hz) {
    if (!apic_base || !check_apic()) return -1;

    apic_msr_enable();
    apic_init_svr((volatile uint32_t *)apic_base);
    apic_init_lvt((volatile uint32_t *)apic_base, true);

    uint32_t ticks_per_ms = apic_calibrate((volatile uint32_t *)apic_base);
    apic_timer_vector = timer_vector;
    apic_timer_initial_count = apic_timer_count_for(ticks_per_ms, timer_hz);
    apic_timer_program((volatile uint32_t *)apic_base, false);

    return 0;
}

void apic_init_cpu(uint8_t timer_vector, uint32_t timer_hz) {
    (void)timer_hz;
    if (!check_apic()) return;
    if (apic_timer_initial_count == 0) return;

    apic_timer_vector = timer_vector;
    apic_msr_enable();
    volatile uint32_t *base = apic_local_base();
    apic_init_svr(base);
    apic_init_lvt(base, false);
    apic_timer_program(base, true);
}

void apic_timer_unmask_cpu(void) {
    volatile uint32_t *base = apic_local_base();
    APIC_REG(APIC_LVT_TIMER, base) =
        (uint32_t)apic_timer_vector | LVT_PERIODIC;
    APIC_REG(APIC_TIMER_INITCNT, base) = apic_timer_initial_count;
}

void apic_send_eoi(void) {
    lapic_write(APIC_EOI_REG, 0);
}
