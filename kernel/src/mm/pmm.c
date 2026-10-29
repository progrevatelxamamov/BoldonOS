#include <kernel.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <bootloader.h>
#include <mm/pmm.h>
#include <memory.h>
#include <scheduler/spinlock.h>

static uint64_t *bitmap = NULL;
static uint64_t bitmap_size = 0;
static uint64_t total_pages = 0;
static uint64_t free_pages = 0;
static uint64_t last_alloc_index = 0;
static uint64_t highest_page = 0;

static spinlock_t pmm_lock = SPINLOCK_INIT;

uint64_t get_total_pages(void) { return total_pages; }
uint64_t get_free_pages(void) { return free_pages; }

static inline void bitmap_set(uint64_t bit) {
    bitmap[bit / 64] |= (1ULL << (bit % 64));
}

static inline void bitmap_clear(uint64_t bit) {
    bitmap[bit / 64] &= ~(1ULL << (bit % 64));
}

static inline bool bitmap_test(uint64_t bit) {
    return (bitmap[bit / 64] & (1ULL << (bit % 64))) != 0;
}

void pmm_init(void) {
    struct limine_memmap_response *memmap = get_bootinfo().memmap;
    uint64_t hhdm = get_bootinfo().hhdm->offset;

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type != LIMINE_MEMMAP_USABLE) continue;
        uint64_t top = entry->base + entry->length;
        if (top > highest_page) highest_page = top;
    }

    total_pages = highest_page / 4096;
    bitmap_size = (total_pages + 7) / 8;
    bitmap_size = (bitmap_size + 7) & ~7ULL;

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type != LIMINE_MEMMAP_USABLE) continue;
        if (entry->length >= bitmap_size) {
            bitmap = (uint64_t *)(entry->base + hhdm);
            entry->base += bitmap_size;
            entry->length -= bitmap_size;
            break;
        }
    }

    memset(bitmap, 0xFF, bitmap_size);

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type != LIMINE_MEMMAP_USABLE) continue;
        for (uint64_t p = 0; p < entry->length; p += 4096) {
            uint64_t page = (entry->base + p) / 4096;
            bitmap_clear(page);
            free_pages++;
        }
    }
}

void* pmm_alloc_page(void) {
    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);

    for (uint64_t i = last_alloc_index; i < total_pages; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            last_alloc_index = i + 1;
            free_pages--;
            spin_unlock_irqrestore(&pmm_lock, flags);
            return (void *)(i * 4096);
        }
    }

    for (uint64_t i = 0; i < last_alloc_index; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            last_alloc_index = i + 1;
            free_pages--;
            spin_unlock_irqrestore(&pmm_lock, flags);
            return (void *)(i * 4096);
        }
    }

    spin_unlock_irqrestore(&pmm_lock, flags);
    return NULL;
}

void* pmm_alloc_pages(uint64_t count) {
    if (count == 0) return NULL;
    if (count == 1) return pmm_alloc_page();

    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);

    uint64_t consecutive = 0;
    uint64_t start = 0;

    for (uint64_t i = 0; i < total_pages; i++) {
        if (!bitmap_test(i)) {
            if (consecutive == 0) start = i;
            consecutive++;
            if (consecutive == count) {
                for (uint64_t j = start; j < start + count; j++) {
                    bitmap_set(j);
                }
                free_pages -= count;
                last_alloc_index = start + count;
                spin_unlock_irqrestore(&pmm_lock, flags);
                return (void *)(start * 4096);
            }
        } else {
            consecutive = 0;
        }
    }

    spin_unlock_irqrestore(&pmm_lock, flags);
    return NULL;
}

void* pmm_alloc_pages_below(uint64_t count, uint64_t limit) {
    uint64_t end = limit / 4096;
    if (end > total_pages) end = total_pages;
    if (!count || count >= end) return NULL;
    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);
    uint64_t run = 0;
    for (uint64_t i = 1; i < end; i++) {
        if (bitmap_test(i)) { run = 0; continue; }
        if (++run == count) {
            uint64_t start = i + 1 - count;
            for (uint64_t j = start; j <= i; j++) bitmap_set(j);
            free_pages -= count;
            spin_unlock_irqrestore(&pmm_lock, flags);
            return (void *)(start * 4096);
        }
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
    return NULL;
}

void* pmm_alloc_page_and_clear(void) {
    void *phys = pmm_alloc_page();
    if (phys == NULL) return NULL;
    uint64_t virt = ((uint64_t)phys) + get_bootinfo().hhdm->offset;
    memset((void*)virt, 0, 4096);
    return phys;
}

void pmm_free_page(void* page) {
    if (page == NULL) return;
    uint64_t idx = (uint64_t)page / 4096;

    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);
    if (bitmap_test(idx)) {
        bitmap_clear(idx);
        free_pages++;
        if (idx < last_alloc_index) last_alloc_index = idx;
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
}

void pmm_free_pages(void* addr, uint64_t count) {
    if (addr == NULL || count == 0) return;
    uint64_t start = (uint64_t)addr / 4096;

    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);
    for (uint64_t i = 0; i < count; i++) {
        if (bitmap_test(start + i)) {
            bitmap_clear(start + i);
            free_pages++;
        }
    }
    if (start < last_alloc_index) last_alloc_index = start;
    spin_unlock_irqrestore(&pmm_lock, flags);
}

uint64_t pmm_get_total_memory(void) {
    return total_pages * 4096;
}

uint64_t pmm_get_free_memory(void) {
    return free_pages * 4096;
}