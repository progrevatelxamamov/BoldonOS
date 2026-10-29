#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <kernel.h>
#include <mm/vmm.h>
extern int puts(const char *);
static page_table_t pml4 __attribute__((aligned(4096)));
static page_table_t pdpt __attribute__((aligned(4096)));
static page_table_t pd __attribute__((aligned(4096)));
static page_table_t pt __attribute__((aligned(4096)));
static struct limine_hhdm_response hhdm;
bootloader_t get_bootinfo(void) { return (bootloader_t){.hhdm = &hhdm}; }
int main(void) {
    uint64_t base = 0xFFFF800000000000ULL;
    pml4.entries[pml4_index(base)] = (uintptr_t)&pdpt | PTE_PRESENT;
    pdpt.entries[0] = (uintptr_t)&pd | PTE_PRESENT;
    pd.entries[0] = (uintptr_t)&pt | PTE_PRESENT;
    pt.entries[1] = 0x12345000 | PTE_PRESENT | PTE_HUGE;
    assert(vmm_get_phys(&pml4, base + 0x1456) == 0x12345000);
    assert(vmm_is_mapped(&pml4, base + 0x1456));
    pt.entries[1] &= ~PTE_PRESENT;
    assert(!vmm_is_mapped(&pml4, base + 0x1456));
    assert(!vmm_get_phys(&pml4, base + 0x1456));
    pd.entries[2] = 0x400000 | PTE_PRESENT | PTE_HUGE | (1ULL << 12);
    assert(vmm_get_phys(&pml4, base + 0x501FE8) == 0x501000);
    assert(vmm_is_mapped(&pml4, base + 0x501FE8));
    assert(vmm_get_phys(&pml4, base + 0x5FFFFF) == 0x5FF000);
    pdpt.entries[0] = 0x100000000ULL | PTE_PRESENT | PTE_HUGE | (1ULL << 12);
    assert(vmm_get_phys(&pml4, base + 0x501FE8) == 0x100501000ULL);
    assert(vmm_get_phys(&pml4, base + 0x3FFFFFFF) == 0x13FFFF000ULL);
    pdpt.entries[0] = PTE_PRESENT | PTE_HUGE;
    assert(vmm_is_mapped(&pml4, base));
    assert(vmm_get_phys(&pml4, base) == 0);
    pdpt.entries[0] = PTE_HUGE;
    assert(!vmm_is_mapped(&pml4, base));
    assert(!vmm_get_phys(&pml4, base));
    assert(!vmm_is_mapped(&pml4, 0x0000800000000000ULL));
    assert(!vmm_is_mapped(&pml4, 0xFFFF7FFFFFFFFFE0ULL));
    assert(!vmm_is_mapped(NULL, base));
    pml4.entries[pml4_index(base)] |= PTE_HUGE;
    assert(!vmm_is_mapped(&pml4, base));
    puts("VMM 4KiB/2MiB/1GiB translation, PAT, high addresses and present-bit tests passed");
    return 0;
}
