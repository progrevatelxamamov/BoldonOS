#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>

void pmm_init(void);
void* pmm_alloc_page(void);
void* pmm_alloc_pages(uint64_t count);
void* pmm_alloc_page_and_clear(void);
void pmm_free_page(void* page);
void pmm_free_pages(void* addr, uint64_t count);
uint64_t pmm_get_total_memory(void);
uint64_t pmm_get_free_memory(void);
uint64_t get_total_pages(void);
uint64_t get_free_pages(void);

#endif