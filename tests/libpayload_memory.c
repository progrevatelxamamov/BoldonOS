#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <kernel.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <assert.h>
extern int puts(const char *);
extern void test_ehci_cleanup(unsigned count, int nonzero_terminated_link);
extern void *lp_memalign(size_t, size_t);
extern void *lp_calloc(size_t, size_t);
extern void lp_free(void *);
extern uintptr_t lp_virt_to_phys(const volatile void *);
extern void *lp_phys_to_virt(uintptr_t);
static uint8_t arena[256 * 4096] __attribute__((aligned(65536)));
static uint64_t used, freed;
static struct limine_hhdm_response hhdm;
bootloader_t get_bootinfo(void) { return (bootloader_t){.hhdm = &hhdm}; }
page_table_t *get_kernel_pml4(void) { return NULL; }
uint64_t vmm_get_phys(page_table_t *p, uint64_t v) { (void)p; (void)v; assert(0); return 0; }
void *pmm_alloc_pages_below(uint64_t pages, uint64_t limit) {
    assert(limit == 0x100000000ULL && used + pages <= 256);
    uintptr_t p = 0x100000 + used * 4096;
    used += pages;
    return (void *)p;
}
void pmm_free_pages(void *p, uint64_t count) {
    assert((uintptr_t)p >= 0x100000 && (uintptr_t)p < 0x200000);
    freed += count;
}
void console_print_string(const char *s) { (void)s; assert(0); }
int main(void) {
    hhdm.offset = (uintptr_t)arena - 0x100000;
    for (size_t align = 16; align <= 65536; align *= 2) {
        uint8_t *p = lp_memalign(align, 12345);
        assert(p && !((uintptr_t)p & (align - 1)));
        uintptr_t phys = lp_virt_to_phys(p);
        assert(!(phys & (align - 1)) && phys < 0x100000000ULL);
        assert(lp_phys_to_virt(phys) == p);
        for (unsigned i = 0; i < 12345; i++) assert(p[i] == 0);
        p[12344] = 0x7F;
        lp_free(p);
    }
    assert(!lp_virt_to_phys(NULL));
    assert(!lp_phys_to_virt(0));
    lp_free(lp_phys_to_virt(0));
    test_ehci_cleanup(0, 0);
    test_ehci_cleanup(1, 0);
    test_ehci_cleanup(4, 0);
    test_ehci_cleanup(3, 1);
    assert(!lp_memalign(24, 16));
    assert(!lp_memalign(16, SIZE_MAX));
    assert(!lp_calloc(SIZE_MAX, 2));
    assert(freed == used);
    puts("libpayload allocator, null/HHDM translation, EHCI cleanup and overflow tests passed");
    return 0;
}
