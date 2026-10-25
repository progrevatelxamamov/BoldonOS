#include "syscall/syscall.h"
#include <kernel.h>
#include <bootloader.h>
#include <cpu/apic.h>
#include <cpu/acpi.h>
#include <desc/gdt.h>
#include <desc/idt.h>
#include <io.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <mm/malloc.h>
#include <memory.h>
#include <gconsole.h>
#include <scheduler/scheduler.h>
#include <stdbool.h>

#define AP_TRAMP_PHYS   0x7000
#define AP_PARAM_CR3    0x7FE0
#define AP_PARAM_RSP    0x7FE8
#define AP_PARAM_ENTRY  0x7FF0
#define AP_PARAM_ARG    0x7FF8
#define MAX_CPUS        256

extern char ap_trampoline_start[];
extern char ap_trampoline_end[];

volatile uint32_t ap_cores_booted = 1;
static uint64_t g_hhdm_offset     = 0;

/* APIC ID → logical ID */
static int     apic_to_logical[256];
static uint8_t logical_to_apic[MAX_CPUS];
static int     num_logical = 0;

int cpu_get_logical_id(void) {
    uint32_t ebx;
    __asm__ volatile("cpuid" : "=b"(ebx) : "a"(1) : "ecx", "edx");
    return apic_to_logical[(ebx >> 24) & 0xFF];
}

static int register_cpu(uint8_t apic_id) {
    if (apic_to_logical[apic_id] >= 0) return apic_to_logical[apic_id];
    if (num_logical >= MAX_CPUS) return -1;
    int id = num_logical++;
    apic_to_logical[apic_id] = id;
    logical_to_apic[id]      = apic_id;
    return id;
}

/* ── LAPIC ── */
static volatile uint32_t *get_lapic_base(void) {
    uint64_t phys = rdmsr(IA32_APIC_BASE_MSR) & 0xFFFFF000ULL;
    if (!phys) phys = 0xFEE00000ULL;
    return (volatile uint32_t *)(phys + g_hhdm_offset);
}

static inline void lapic_write(uint32_t reg, uint32_t val) {
    APIC_REG(reg, get_lapic_base()) = val;
}

static inline uint32_t lapic_read(uint32_t reg) {
    return APIC_REG(reg, get_lapic_base());
}

static void lapic_wait_icr(void) {
    for (int i = 0; i < 100000; i++) {
        if ((lapic_read(0x300) & (1u << 12)) == 0) return;
        __asm__ volatile("pause");
    }
}

static void lapic_send_init(uint8_t apic_id) {
    lapic_wait_icr();
    lapic_write(0x310, (uint32_t)apic_id << 24);
    lapic_write(0x300, 0x0000C500);   /* INIT + Assert + Level */
    lapic_wait_icr();
}

static void lapic_send_sipi(uint8_t apic_id, uint8_t vector) {
    lapic_wait_icr();
    lapic_write(0x310, (uint32_t)apic_id << 24);
    lapic_write(0x300, 0x00004600u | vector);
    lapic_wait_icr();
}

/* ── Трамплин ── */
static void copy_trampoline(void) {
    size_t sz = (size_t)(ap_trampoline_end - ap_trampoline_start);
    if (sz > 0xFE0) return;
    memcpy((void *)(AP_TRAMP_PHYS + g_hhdm_offset), ap_trampoline_start, sz);
    __sync_synchronize();
}

#define AP_TRAMP_PHYS     0x7000
#define AP_PARAM_CR4      0x7FD8
#define AP_PARAM_CR3      0x7FE0
#define AP_PARAM_RSP      0x7FE8
#define AP_PARAM_ENTRY    0x7FF0
#define AP_PARAM_ARG      0x7FF8

static void fill_params(uint64_t cr4, uint64_t cr3, uint64_t rsp,
                        uint64_t entry, uint64_t arg)
{
    uint8_t *b = (uint8_t *)g_hhdm_offset;
    *(volatile uint64_t *)(b + AP_PARAM_CR4)   = cr4;
    *(volatile uint64_t *)(b + AP_PARAM_CR3)   = cr3;
    *(volatile uint64_t *)(b + AP_PARAM_RSP)   = rsp;
    *(volatile uint64_t *)(b + AP_PARAM_ENTRY) = entry;
    *(volatile uint64_t *)(b + AP_PARAM_ARG)   = arg;
    __sync_synchronize();
}

#define IA32_GS_BASE        0xC0000101
#define IA32_KERNEL_GS_BASE 0xC0000102

void percpu_init(int logical_id) {
    extern struct cpu_local_data cpu_locals[]; // Из scheduler.c
    
    cpu_locals[logical_id].self = &cpu_locals[logical_id];
    cpu_locals[logical_id].cpu_id = logical_id;
    
    // Загружаем адрес структуры в MSR GS_BASE
    wrmsr(IA32_GS_BASE, (uint64_t)&cpu_locals[logical_id]);
    wrmsr(IA32_KERNEL_GS_BASE, (uint64_t)&cpu_locals[logical_id]);
}

#define LAPIC_TPR            0x0080  // Task Priority Register
#define LAPIC_SVR            0x00F0  // Spurious Interrupt Vector Register
#define LAPIC_LVT_TIMER      0x0320  // Local Vector Table (Timer)
#define LAPIC_TIMER_INITCNT  0x0380  // Initial Count Register
#define LAPIC_TIMER_DIV      0x03E0  // Divide Configuration Register
extern uint64_t g_ticks_per_ms; 

static void ap_entry(uint64_t logical_id) {
    /*
     * CR3 НЕ ТРОГАЕМ!
     * Трамплин загрузил ap_cr3 = BSP CR3 = 0x7f401000.
     * Этот PML4 содержит валидный higher-half маппинг.
     *
     * Предыдущая ошибка: чтение g_hhdm_offset через копию PML4
     * (0x7bb65000) → RSVD #PF → triple fault.
     */

    // Сбрасываем Task Priority, чтобы принимать ВСЕ прерывания (TPR = 0)
    lapic_write(LAPIC_TPR, 0);

    // Включаем APIC (Бит 8 = 1) и задаем Spurious Vector = 0xFF (255)
    lapic_write(LAPIC_SVR, lapic_read(LAPIC_SVR) | 0x1FF);

    // Настраиваем делитель (Divide by 16)
    lapic_write(LAPIC_TIMER_DIV, 0x3);

    // Настраиваем LVT Timer: 
    // Вектор = 32 (0x20)
    // Бит 17 = 1 (Periodic Mode - периодический режим)
    // Бит 16 = 0 (Unmasked - разблокировано)
    lapic_write(LAPIC_LVT_TIMER, 32 | 0x20000);

    lapic_write(LAPIC_TIMER_INITCNT, g_ticks_per_ms);

    /* GDT + TSS */
    gdt_init_cpu((int)logical_id);

    /* IDT */
    idt_reload();

    /* SYSCALL */
    syscall_init();

    vmm_switch_address_space((uint64_t)get_kernel_pml4() - get_bootinfo().hhdm->offset);

    /* SHEDULER */
    percpu_init(logical_id);

    /* Готово */
    __atomic_add_fetch(&ap_cores_booted, 1, __ATOMIC_SEQ_CST);

    console_print_string("[SMP] AP logical=");
    console_print_num(logical_id);
    console_print_string(" alive\n");

    __asm__ volatile ("int $23");

    while (1) {
        __asm__ volatile("sti; hlt");
    }
}

void smp_init(void) {
    g_hhdm_offset = get_bootinfo().hhdm->offset;

    for (int i = 0; i < 256; i++) apic_to_logical[i] = -1;

    struct MADT *madt = (struct MADT *)acpi_find_table("APIC");
    if (!madt) {
        console_print_string("[SMP] No MADT\n");
        return;
    }

    /* BSP */
    uint32_t ebx;
    __asm__ volatile("cpuid" : "=b"(ebx) : "a"(1) : "ecx", "edx");
    uint8_t bsp_apic = (ebx >> 24) & 0xFF;
    register_cpu(bsp_apic);

    console_print_string("[SMP] BSP APIC ID = ");
    console_print_num(bsp_apic);
    console_print_string("\n");

    percpu_init(bsp_apic);

    /* Identity-map 1-го МБ */
    for (uint64_t a = 0; a < 0x100000; a += 0x1000) {
        vmm_map_page(get_kernel_pml4(), a, a,
                     PTE_PRESENT | PTE_WRITABLE);
        vmm_map_page(get_kernel_pml4(), a, g_hhdm_offset + a,
                     PTE_PRESENT | PTE_WRITABLE);
    }

    /* Читаем CR3 BSP — это ОРИГИНАЛЬНЫЙ PML4 ядра */
    uint64_t cr3, cr4;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr3 &= ~0xFFFULL;

    /* Сброс TLB после identity-map */
    __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");

    console_print_string("[SMP] BSP CR3 = ");
    console_print_hex(cr3);
    console_print_string("\n");

    /*
     * AP CR3 = BSP CR3. БЕЗ КОПИРОВАНИЯ!
     *
     * Если CR3 < 4 ГБ — передаём напрямую (наш случай: 0x7f401000).
     * Если CR3 >= 4 ГБ — нужно будет делать deep copy всех уровней
     * page table, а не только PML4. Но это отдельная задача.
     */
    uint64_t ap_cr3 = cr3;

    if (cr3 >= 0x100000000ULL) {
        console_print_string("[SMP] WARNING: CR3 > 4GB, AP boot may fail!\n");
        /* TODO: deep copy PML4 + PDPT + PD + PT для higher-half */
    }

    copy_trampoline();

    /* Запуск AP */
    uint8_t *p   = (uint8_t *)madt + sizeof(struct MADT);
    uint8_t *end = (uint8_t *)madt + madt->header.Length;

    while (p < end) {
        struct MADT_entry *e = (struct MADT_entry *)p;
        if (e->length < sizeof(struct MADT_entry)) break;

        if (e->type == MADT_TYPE_LAPIC) {
            struct MADT_lapic *la = (struct MADT_lapic *)e;

            if ((la->flags & MADT_LAPIC_ENABLED) &&
                 la->apic_id != bsp_apic)
            {
                int lid = register_cpu(la->apic_id);
                if (lid < 0) { p += e->length; continue; }

                void *stk = malloc_alloc(16384);
                if (!stk) { p += e->length; continue; }
                uint64_t stk_top = ((uint64_t)stk + 16384) & ~(uint64_t)0xF;

                uint32_t prev = ap_cores_booted;

                fill_params(cr4, cr3, stk_top, (uint64_t)ap_entry, (uint64_t)lid);

                console_print_string("[SMP] Starting AP ");
                console_print_num(la->apic_id);
                console_print_string(" (logical=");
                console_print_num(lid);
                console_print_string(")...\n");

                lapic_send_init(la->apic_id);
                hpet_sleep_ms(10);

                lapic_send_sipi(la->apic_id, AP_TRAMP_PHYS >> 12);

                bool ok = false;
                for (int t = 0; t < 100; t++) {
                    if (ap_cores_booted > prev) { ok = true; break; }
                    hpet_sleep_ms(1);
                }
                if (!ok) {
                    lapic_send_sipi(la->apic_id, AP_TRAMP_PHYS >> 12);
                    for (int t = 0; t < 200; t++) {
                        if (ap_cores_booted > prev) { ok = true; break; }
                        hpet_sleep_ms(1);
                    }
                }
                if (!ok) {
                    console_print_string("[SMP] AP ");
                    console_print_num(la->apic_id);
                    console_print_string(" TIMEOUT\n");
                }
                hpet_sleep_ms(5);
            }
        }
        p += e->length;
    }

    console_print_string("[SMP] Cores: ");
    console_print_num(ap_cores_booted);
    console_print_string("\n");
}