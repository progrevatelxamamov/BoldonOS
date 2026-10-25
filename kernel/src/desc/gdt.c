#include <stdint.h>
#include <desc/gdt.h>
#include <mm/pmm.h>
#include <kernel.h>
#include <bootloader.h>
#include <memory.h>

#define MAX_CPUS 256

static uint8_t kernel_stack[MAX_CPUS][16384] __attribute__((aligned(16)));

uint64_t gdt_descriptors[MAX_CPUS][7];
struct gdt_ptr ptr[MAX_CPUS];
struct tss_entry tss[MAX_CPUS];

extern void reloadSegments(void);
extern int cpu_get_logical_id(void);  /* реализована в smp.c */

void create_descriptor(int cpu, uint64_t num, uint32_t base, uint32_t limit, uint16_t flag) {
    uint64_t descriptor;

    descriptor  =  limit       & 0x000F0000;
    descriptor |= (flag <<  8) & 0x00F0FF00;
    descriptor |= (base >> 16) & 0x000000FF;
    descriptor |=  base        & 0xFF000000;

    descriptor <<= 32;

    descriptor |= base  << 16;
    descriptor |= limit  & 0x0000FFFF;

    gdt_descriptors[cpu][num] = descriptor;
}

void create_tss_descriptor(int cpu, uint64_t num, uint64_t base, uint32_t limit) {
    uint64_t low = (limit & 0xFFFF);
    low |= (base & 0x00FFFFFF) << 16;
    low |= (uint64_t)0x89 << 40;
    low |= ((uint64_t)(limit >> 16) & 0x0F) << 48;
    low |= ((base >> 24) & 0xFF) << 56;

    uint64_t high = (base >> 32);

    gdt_descriptors[cpu][num]     = low;
    gdt_descriptors[cpu][num + 1] = high;
}

/* Инициализация GDT/TSS для КОНКРЕТНОГО логического CPU.
   cpu — это ЛОГИЧЕСКИЙ ID (0, 1, 2, ...), а не APIC ID. */
void gdt_init_cpu(int cpu) {
    if (cpu < 0 || cpu >= MAX_CPUS) return;

    create_descriptor(cpu, 0, 0, 0, 0);
    create_descriptor(cpu, 1, 0, 0x000FFFFF, (GDT_CODE_PL0));
    create_descriptor(cpu, 2, 0, 0x000FFFFF, (GDT_DATA_PL0));
    create_descriptor(cpu, 3, 0, 0x000FFFFF, (GDT_DATA_PL3));
    create_descriptor(cpu, 4, 0, 0x000FFFFF, (GDT_CODE_PL3));

    for (uint32_t i = 0; i < sizeof(struct tss_entry); i++) {
        ((uint8_t*)&tss[cpu])[i] = 0;
    }

    tss[cpu].rsp0 = (uint64_t)kernel_stack[cpu] + 16384;
    tss[cpu].iopb_offset = sizeof(struct tss_entry);

    create_tss_descriptor(cpu, 5, (uint64_t)&tss[cpu], sizeof(struct tss_entry) - 1);

    ptr[cpu].base = (uint64_t)&gdt_descriptors[cpu];
    ptr[cpu].limit = sizeof(gdt_descriptors[cpu]) - 1;

    __asm__ __volatile__("lgdt %0" : : "m" (ptr[cpu]));
    reloadSegments();

    __asm__ __volatile__("ltr %0" : : "r" ((uint16_t)0x28));
}

/* Для BSP: предполагаем, что он уже зарегистрирован как логический CPU 0.
   Если smp_init ещё не вызвана, то используем 0 по умолчанию. */
void gdt_init(void) {
    /* До smp_init BSP — всегда 0 */
    gdt_init_cpu(0);
}

void tss_set_stack(uint64_t stack) {
    tss[cpu_get_logical_id()].rsp0 = stack;
}

uint64_t tss_get_stack(void) {
    return tss[cpu_get_logical_id()].rsp0;
}