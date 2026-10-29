#include <stdint.h>
#include <stdbool.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <kernel.h>
#include <cpu/apic.h>
#include <bootloader.h>
#include <gconsole.h>
#include <memory.h>

page_table_t *kernel_pml4;
bool vmm_initialized = false;

page_table_t* get_kernel_pml4(void) {
    return kernel_pml4;
}

void vmm_init(void) {
    uint64_t limine_pml4_phys;
    __asm__ volatile("mov %%cr3, %0" : "=r"(limine_pml4_phys));

    page_table_t* limine_pml4 = (page_table_t*)(limine_pml4_phys + get_bootinfo().hhdm->offset);
    kernel_pml4 = limine_pml4;

    vmm_switch_address_space((uint64_t)kernel_pml4 - get_bootinfo().hhdm->offset);
    vmm_initialized = true;
}

void vmm_map_page(page_table_t *pml4, uint64_t phys, uint64_t virt, uint64_t flags) {
    if (!pml4) return;

    uint64_t pml4_indx = pml4_index(virt);
    uint64_t pdpt_indx = pdpt_index(virt);
    uint64_t pd_indx   = pd_index(virt);
    uint64_t pt_indx   = pt_index(virt);

    uint64_t dir_flags = PTE_PRESENT | PTE_WRITABLE;
    if (virt < 0x800000000000ULL || (flags & PTE_USER)) {
        dir_flags |= PTE_USER;
    }

    page_table_t *pdpt;
    page_table_t *pd;
    page_table_t *pt;

    if (pml4->entries[pml4_indx] == 0) {
        void* page = pmm_alloc_page_and_clear();
        if (!page) return;
        pml4->entries[pml4_indx] = pte_create((uint64_t)page, dir_flags);
    } else {
        pml4->entries[pml4_indx] |= dir_flags;
    }
    pdpt = (page_table_t*)(pte_get_addr(pml4->entries[pml4_indx]) + get_bootinfo().hhdm->offset);

    if (pdpt->entries[pdpt_indx] == 0) {
        void* page = pmm_alloc_page_and_clear();
        if (!page) return;
        pdpt->entries[pdpt_indx] = pte_create((uint64_t)page, dir_flags);
    } else {
        pdpt->entries[pdpt_indx] |= dir_flags;
    }
    pd = (page_table_t*)(pte_get_addr(pdpt->entries[pdpt_indx]) + get_bootinfo().hhdm->offset);

    if (pd->entries[pd_indx] == 0) {
        void* page = pmm_alloc_page_and_clear();
        if (!page) return;
        pd->entries[pd_indx] = pte_create((uint64_t)page, dir_flags);
    } else {
        pd->entries[pd_indx] |= dir_flags;
    }
    pt = (page_table_t*)(pte_get_addr(pd->entries[pd_indx]) + get_bootinfo().hhdm->offset);

    pt->entries[pt_indx] = pte_create(phys, flags);

    if (vmm_initialized) vmm_invalidate_page(virt);
}

void vmm_unmap_page(page_table_t *pml4, uint64_t virt) {
    uint64_t pml4_indx = pml4_index(virt);
    uint64_t pdpt_indx = pdpt_index(virt);
    uint64_t pd_indx = pd_index(virt);
    uint64_t pt_indx = pt_index(virt);

    page_table_t *pdpt;
    page_table_t *pd;
    page_table_t *pt;

    if (pml4->entries[pml4_indx] == 0) return;
    pdpt = (page_table_t*)(pte_get_addr(pml4->entries[pml4_indx]) + get_bootinfo().hhdm->offset);
    
    if (pdpt->entries[pdpt_indx] == 0) return;
    pd = (page_table_t*)(pte_get_addr(pdpt->entries[pdpt_indx]) + get_bootinfo().hhdm->offset);

    if (pd->entries[pd_indx] == 0) return;
    pt = (page_table_t*)(pte_get_addr(pd->entries[pd_indx]) + get_bootinfo().hhdm->offset);

    pt->entries[pt_indx] = 0;

    if (vmm_initialized) {
        vmm_invalidate_page(virt);
        smp_broadcast_ipi(0xFC);
    }
}

static bool vmm_lookup_page(page_table_t *pml4, uint64_t virt, uint64_t *phys) {
    uint64_t upper = virt >> 47;
    if (!pml4 || !phys || (upper != 0 && upper != 0x1FFFF)) return false;
    page_table_t *table = pml4;
    for (unsigned shift = 39; ; shift -= 9) {
        uint64_t entry = table->entries[(virt >> shift) & 0x1FF];
        if (!(entry & PTE_PRESENT)) return false;
        if (shift == 12) {
            *phys = pte_get_addr(entry);
            return true;
        }
        if (entry & PTE_HUGE) {
            if (shift == 39) return false;
            uint64_t offset_mask = (1ULL << shift) - 1;
            *phys = (pte_get_addr(entry) & ~offset_mask) |
                    (virt & offset_mask & ~0xFFFULL);
            return true;
        }
        table = (page_table_t *)(pte_get_addr(entry) + get_bootinfo().hhdm->offset);
    }
}

bool vmm_is_mapped(page_table_t *pml4, uint64_t virt) {
    uint64_t phys;
    return vmm_lookup_page(pml4, virt, &phys);
}

uint64_t vmm_get_phys(page_table_t *pml4, uint64_t virt) {
    uint64_t phys;
    return vmm_lookup_page(pml4, virt, &phys) ? phys : 0;
}

#define PTE_PHYS_MASK 0x000FFFFFFFFFF000ULL

page_table_t *vmm_clone_pml4(page_table_t *src_pml4) {
    if (!src_pml4) return NULL;

    uint64_t hhdm = get_bootinfo().hhdm->offset;
    void *dst_page = pmm_alloc_page_and_clear();
    if (!dst_page) return NULL;

    page_table_t *dst_pml4 = (page_table_t *)((uint64_t)dst_page + hhdm);

    for (int i = 256; i < 512; i++) {
        dst_pml4->entries[i] = src_pml4->entries[i];
    }

    for (int i = 0; i < 256; i++) {
        if (!(src_pml4->entries[i] & PTE_PRESENT)) continue;

        uint64_t pdpt_phys = src_pml4->entries[i] & PTE_PHYS_MASK;
        page_table_t *src_pdpt = (page_table_t *)(pdpt_phys + hhdm);

        for (int j = 0; j < 512; j++) {
            if (!(src_pdpt->entries[j] & PTE_PRESENT)) continue;
            if (src_pdpt->entries[j] & 0x80) continue;

            uint64_t pd_phys = src_pdpt->entries[j] & PTE_PHYS_MASK;
            page_table_t *src_pd = (page_table_t *)(pd_phys + hhdm);

            for (int k = 0; k < 512; k++) {
                if (!(src_pd->entries[k] & PTE_PRESENT)) continue;
                if (src_pd->entries[k] & 0x80) continue;

                uint64_t pt_phys = src_pd->entries[k] & PTE_PHYS_MASK;
                page_table_t *src_pt = (page_table_t *)(pt_phys + hhdm);

                for (int l = 0; l < 512; l++) {
                    if (!(src_pt->entries[l] & PTE_PRESENT)) continue;

                    uint64_t virt = ((uint64_t)i << 39) | ((uint64_t)j << 30) |
                                    ((uint64_t)k << 21) | ((uint64_t)l << 12);

                    if (virt == 0) continue;

                    uint64_t src_phys = src_pt->entries[l] & PTE_PHYS_MASK;
                    if (!src_phys) continue;

                    uint64_t pte_flags = src_pt->entries[l] & 0xFFF;
                    if (src_pt->entries[l] & (1ULL << 63)) pte_flags |= PTE_NX;

                    void *dst_phys = pmm_alloc_page_and_clear();
                    if (dst_phys) {
                        memcpy((void *)((uint64_t)dst_phys + hhdm),
                               (void *)(src_phys + hhdm), 4096);
                        vmm_map_page(dst_pml4, (uint64_t)dst_phys, virt, pte_flags);
                    }
                }
            }
        }
    }
    return dst_pml4;
}