#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stdbool.h>

#define PTE_PRESENT    (1ULL << 0)
#define PTE_WRITABLE   (1ULL << 1)
#define PTE_USER       (1ULL << 2)
#define PTE_PWT        (1ULL << 3)
#define PTE_PCD        (1ULL << 4)
#define PTE_ACCESSED   (1ULL << 5)
#define PTE_DIRTY      (1ULL << 6)
#define PTE_HUGE       (1ULL << 7)
#define PTE_GLOBAL     (1ULL << 8)
#define PTE_NX         (1ULL << 63)

#define PTE_COW        (1ULL << 9)
#define PTE_SWAPPED    (1ULL << 10)

#define PTE_ADDR_MASK  0x000FFFFFFFFFF000ULL

static inline uint64_t pte_get_addr(uint64_t entry) {
    return entry & PTE_ADDR_MASK;
}

static inline uint64_t pte_get_flags(uint64_t entry) {
    return (entry & 0xFFF) | (entry & PTE_NX);
}

static inline uint64_t pte_create(uint64_t phys, uint64_t flags) {
    return (phys & PTE_ADDR_MASK) | (flags & (0xFFF | PTE_NX));
}

static inline int pte_is_present(uint64_t entry) {
    return (entry & PTE_PRESENT) != 0;
}

static inline uint64_t pml4_index(uint64_t virt) {
    return (virt >> 39) & 0x1FF;
}

static inline uint64_t pdpt_index(uint64_t virt) {
    return (virt >> 30) & 0x1FF;
}

static inline uint64_t pd_index(uint64_t virt) {
    return (virt >> 21) & 0x1FF;
}

static inline uint64_t pt_index(uint64_t virt) {
    return (virt >> 12) & 0x1FF;
}

static inline void vmm_switch_address_space(uint64_t pml4_phys) {
    __asm__ __volatile__(
        "mov %0, %%cr3"
        :
        : "r"(pml4_phys)
        : "memory"
    );
}

static inline void vmm_invalidate_page(uint64_t virt) {
    __asm__ __volatile__(
        "invlpg (%0)"
        :
        : "r"(virt)
        : "memory"
    );
}

typedef struct {
    uint64_t entries[512];
} __attribute__((packed)) page_table_t;

page_table_t* get_kernel_pml4(void);

void vmm_init(void);
void vmm_map_page(page_table_t *pml4, uint64_t phys, uint64_t virt, uint64_t flags);
void vmm_unmap_page(page_table_t *pml4, uint64_t virt);
bool vmm_is_mapped(page_table_t *pml4, uint64_t virt);
uint64_t vmm_get_phys(page_table_t *pml4, uint64_t virt);
page_table_t *vmm_clone_pml4(page_table_t *src_pml4);

#endif