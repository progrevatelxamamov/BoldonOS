#include <cpu/apic.h>
#include <cpu/acpi.h>
#include <io.h>
#include <mm/vmm.h>
#include <bootloader.h>
#include <kernel.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

uint32_t *all_apic_bases[APIC_MAX_CPUS] = {0};
int       this_cpu_apic_index = -1;
uint64_t g_ticks_per_ms = 0; 

static uint32_t *apic_local_base(void) {
    uint64_t phys = rdmsr(IA32_APIC_BASE_MSR) & 0xFFFFF000ULL;
    if (!phys) phys = 0xFEE00000ULL;
    bootloader_t bi = get_bootinfo();
    uint64_t offset = bi.hhdm ? bi.hhdm->offset : 0;
    return (uint32_t *)(phys + offset);
}

bool check_apic(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    return (edx & (1U << 9)) != 0;
}

#define IPI_RESCHEDULE 0xFD
#define IPI_TLB_FLUSH  0xFC

// Отправить IPI всем ядрам, кроме себя (Broadcast Excluding Self)
void smp_broadcast_ipi(uint8_t vector) {
    uint32_t icr_low = 0xC4000 | vector; 
    // Бит 18-19 = 11 (All excluding self)
    // Бит 14 = 1 (Assert)
    
    // Ждём, пока LAPIC освободится
    uint32_t *base = apic_local_base();
    while (APIC_REG(0x300, base) & (1 << 12)) {
        __asm__ volatile("pause");
    }
    APIC_REG(0x300, base) = icr_low;
}

// Отправить IPI конкретному логическому ядру
void smp_send_ipi(int logical_id, uint8_t vector) {
    extern uint8_t logical_to_apic[]; // Из smp.c
    uint8_t apic_id = logical_to_apic[logical_id];
    
    uint32_t *base = apic_local_base();
    while (APIC_REG(0x300, base) & (1 << 12)) {
        __asm__ volatile("pause");
    }

    APIC_REG(0x310, base) = ((uint32_t)apic_id) << 24;
    APIC_REG(0x300, base) = 0x4000 | vector;
}

uint32_t apic_calibrate(volatile uint32_t *apic_base) {
    APIC_REG(APIC_TIMER_DCR, apic_base) =
        (APIC_REG(APIC_TIMER_DCR, apic_base) & ~0xFu) | APIC_TIMER_DIV;

    APIC_REG(APIC_LVT_TIMER, apic_base) = LVT_MASKED;
    APIC_REG(APIC_TIMER_INITCNT, apic_base) = 0xFFFFFFFFu;

    hpet_sleep_ms(10);

    uint32_t current       = APIC_REG(APIC_TIMER_CURCNT, apic_base);
    uint32_t elapsed_ticks = 0xFFFFFFFFu - current;

    if (elapsed_ticks < 100) return 100;
    return elapsed_ticks / 10;
}

static void apic_timer_setup(volatile uint32_t *apic_base, uint8_t vector, uint32_t target_hz) {
    uint32_t ticks_per_ms = apic_calibrate(apic_base);
    g_ticks_per_ms = ticks_per_ms;
    if (target_hz == 0) target_hz = 100;

    uint32_t initial_count = (ticks_per_ms * 1000u) / target_hz;
    if (initial_count == 0) initial_count = 1;

    APIC_REG(APIC_LVT_TIMER, apic_base) = (uint32_t)vector | LVT_PERIODIC;
    APIC_REG(APIC_TIMER_INITCNT, apic_base) = initial_count;
}

// Принудительно выключаем x2APIC и включаем обычный MMIO APIC
void apic_msr_enable(void) {
    uint64_t base = rdmsr(IA32_APIC_BASE_MSR);

    // Если UEFI включил x2APIC (бит 10), отключаем его
    if (base & IA32_APIC_BASE_MSR_X2APIC) {
        base &= ~(IA32_APIC_BASE_MSR_X2APIC | IA32_APIC_BASE_MSR_ENABLE);
        wrmsr(IA32_APIC_BASE_MSR, base);
    }

    // Включаем APIC в стандартном xAPIC MMIO режиме
    base |= IA32_APIC_BASE_MSR_ENABLE | IA32_APIC_BASE_MSR_BSP;
    wrmsr(IA32_APIC_BASE_MSR, base);
}

static void apic_init_lvt(volatile uint32_t *apic_base) {
    APIC_REG(APIC_LVT_LINT0,    apic_base) = LVT_LINT_EXTINT;
    APIC_REG(APIC_LVT_LINT1,    apic_base) = LVT_LINT_NMI;
    APIC_REG(APIC_LVT_ERROR,    apic_base) = LVT_MASKED;
    APIC_REG(APIC_LVT_THERMAL,  apic_base) = LVT_MASKED;
    APIC_REG(APIC_LVT_PERFMON,  apic_base) = LVT_MASKED;

    APIC_REG(APIC_TPR, apic_base) = 0;
}

static void apic_init_svr(volatile uint32_t *apic_base) {
    uint32_t svr = APIC_REG(APIC_SVR, apic_base);
    svr &= ~0xFFu;
    svr |=  0xFFu;       /* Spurious vector = 0xFF */
    svr |=  0x100u;      /* APIC Software Enable */
    APIC_REG(APIC_SVR, apic_base) = svr;
}

static int apic_register_base(uint32_t *apic_base) {
    for (int i = 0; i < APIC_MAX_CPUS; i++) {
        if (all_apic_bases[i] == NULL) {
            all_apic_bases[i] = apic_base;
            return i;
        }
        if (all_apic_bases[i] == apic_base) {
            return i;
        }
    }
    return -1;
}

int enable_apic(uint32_t *apic_base, uint8_t timer_vector, uint32_t timer_hz) {
    if (!apic_base || !check_apic()) return -1;

    int index = apic_register_base(apic_base);
    if (index < 0) return -1;

    apic_msr_enable();
    apic_init_svr(apic_base);
    apic_init_lvt(apic_base);
    apic_timer_setup(apic_base, timer_vector, timer_hz);

    this_cpu_apic_index = index;
    return index;
}

void apic_send_eoi(void) {
    uint32_t *base = apic_local_base();
    if (!base) return;

    APIC_REG(APIC_EOI_REG, base) = 0;
}