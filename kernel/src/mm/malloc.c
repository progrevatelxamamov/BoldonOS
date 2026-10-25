#include "kernel.h"
#include "mm/vmm.h"
#include <mm/malloc.h>
#include <mm/pmm.h>
#include <memory.h>
#include <stdint.h>
#include <stddef.h>
#include <scheduler/spinlock.h>

#define MALLOC_MAGIC 0xDEADBEEFCAFEBABEULL

typedef struct malloc_header {
    uint64_t magic;
    uint64_t size;
    struct malloc_header *next;
    struct malloc_header *prev;
    uint8_t free;
    uint8_t pad[7];
} malloc_header_t;

static malloc_header_t *heap_head = NULL;
static spinlock_t malloc_lock = SPINLOCK_INIT;

void malloc_init(void) {
    heap_head = NULL;
}

static malloc_header_t *expand_heap(uint64_t min_size) {
    uint64_t total = min_size + sizeof(malloc_header_t);
    uint64_t pages = (total + 4095) / 4096;
    if (pages < 4) pages = 4;

    void *phys = pmm_alloc_pages(pages);
    if (!phys) return NULL;

    malloc_header_t *blk = (malloc_header_t *)((uint64_t)phys + get_bootinfo().hhdm->offset);
    blk->magic = MALLOC_MAGIC;
    blk->size = pages * 4096 - sizeof(malloc_header_t);
    blk->free = 1;
    blk->next = heap_head;
    blk->prev = NULL;
    if (heap_head) heap_head->prev = blk;
    heap_head = blk;

    return blk;
}

static void split_block(malloc_header_t *blk, uint64_t size) {
    if (blk->size < size + sizeof(malloc_header_t) + 32) return;

    malloc_header_t *new_blk = (malloc_header_t *)((uint8_t *)blk + sizeof(malloc_header_t) + size);
    new_blk->magic = MALLOC_MAGIC;
    new_blk->size = blk->size - size - sizeof(malloc_header_t);
    new_blk->free = 1;
    new_blk->next = blk->next;
    new_blk->prev = blk;

    if (blk->next) blk->next->prev = new_blk;
    blk->next = new_blk;
    blk->size = size;
}

static void coalesce(malloc_header_t *blk) {
    if (!blk) return;

    if (blk->next && blk->next->free) {
        uint64_t expected = (uint64_t)blk + sizeof(malloc_header_t) + blk->size;
        if ((uint64_t)blk->next == expected) {
            blk->size += sizeof(malloc_header_t) + blk->next->size;
            malloc_header_t *nn = blk->next->next;
            blk->next = nn;
            if (nn) nn->prev = blk;
        }
    }

    if (blk->prev && blk->prev->free) {
        uint64_t expected = (uint64_t)blk->prev + sizeof(malloc_header_t) + blk->prev->size;
        if ((uint64_t)blk == expected) {
            blk->prev->size += sizeof(malloc_header_t) + blk->size;
            malloc_header_t *nn = blk->next;
            blk->prev->next = nn;
            if (nn) nn->prev = blk->prev;
        }
    }
}

void *malloc_alloc(uint64_t size) {
    if (size == 0) return NULL;

    size = (size + 15) & ~15ULL;

    uint64_t flags;
    spin_lock_irqsave(&malloc_lock, &flags);

    malloc_header_t *cur = heap_head;
    while (cur) {
        if (cur->free && cur->size >= size) {
            split_block(cur, size);
            cur->free = 0;
            void *user = (void *)((uint8_t *)cur + sizeof(malloc_header_t));
            memset(user, 0, size);
            spin_unlock_irqrestore(&malloc_lock, flags);
            return user;
        }
        cur = cur->next;
    }

    malloc_header_t *new_blk = expand_heap(size);
    if (!new_blk) {
        spin_unlock_irqrestore(&malloc_lock, flags);
        return NULL;
    }

    split_block(new_blk, size);
    new_blk->free = 0;
    void *user = (void *)((uint8_t *)new_blk + sizeof(malloc_header_t));
    memset(user, 0, size);
    spin_unlock_irqrestore(&malloc_lock, flags);
    return user;
}

void malloc_free(void *ptr) {
    if (!ptr) return;

    malloc_header_t *blk = (malloc_header_t *)((uint8_t *)ptr - sizeof(malloc_header_t));
    if (blk->magic != MALLOC_MAGIC) return;

    uint64_t flags;
    spin_lock_irqsave(&malloc_lock, &flags);
    blk->free = 1;
    coalesce(blk);
    spin_unlock_irqrestore(&malloc_lock, flags);
}