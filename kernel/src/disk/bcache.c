#include "kernel.h"
#include <disk/bcache.h>
#include <disk/block.h>
#include <mm/malloc.h>
#include <mm/pmm.h>
#include <memory.h>
#include <scheduler/spinlock.h>

#define BCACHE_SLOTS 1024
#define BCACHE_PAGE_SECTORS 8

typedef struct bcache_entry {
    char dev_name[32];
    uint64_t page_lba;
    uint8_t *data;
    bool valid;
    bool dirty;
    uint64_t last_access;
    struct bcache_entry *next;
} bcache_entry_t;

static bcache_entry_t entries[BCACHE_SLOTS];
static uint64_t access_counter = 0;
static spinlock_t bcache_lock = SPINLOCK_INIT;

static int my_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static void my_strcpy(char *dst, const char *src) {
    int i = 0;
    while (src[i] && i < 31) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

void bcache_init(void) {
    uint64_t flags;
    spin_lock_irqsave(&bcache_lock, &flags);
    memset(entries, 0, sizeof(entries));
    access_counter = 0;
    spin_unlock_irqrestore(&bcache_lock, flags);
}

static bcache_entry_t *find_entry(const char *dev, uint64_t page_lba) {
    for (int i = 0; i < BCACHE_SLOTS; i++) {
        if (entries[i].valid && entries[i].page_lba == page_lba && my_strcmp(entries[i].dev_name, dev) == 0) {
            entries[i].last_access = ++access_counter;
            return &entries[i];
        }
    }
    return NULL;
}

static bcache_entry_t *alloc_entry(const char *dev, uint64_t page_lba) {
    bcache_entry_t *victim = NULL;
    uint64_t oldest = (uint64_t)-1;

    for (int i = 0; i < BCACHE_SLOTS; i++) {
        if (!entries[i].valid) {
            victim = &entries[i];
            break;
        }
        if (entries[i].last_access < oldest) {
            oldest = entries[i].last_access;
            victim = &entries[i];
        }
    }

    if (victim->valid && victim->dirty) {
        struct block_device *d = find_device(victim->dev_name);
        if (d && d->write) {
            d->write(d->local_device_struct, victim->page_lba, BCACHE_PAGE_SECTORS, victim->data);
        }
        victim->dirty = false;
    }

    if (!victim->data) {
        void *phys = pmm_alloc_page();
        victim->data = (uint8_t *)((uint64_t)phys + get_bootinfo().hhdm->offset);
    }

    victim->valid = false; 
    my_strcpy(victim->dev_name, dev);
    victim->page_lba = page_lba;
    victim->dirty = false;
    victim->last_access = ++access_counter;
    return victim;
}

int bcache_read(const char *dev_name, uint64_t lba, uint64_t count, void *buf) {
    struct block_device *dev = find_device(dev_name);
    if (!dev || !dev->read) return -1;

    uint64_t flags;
    spin_lock_irqsave(&bcache_lock, &flags);

    uint8_t *out = (uint8_t *)buf;
    uint64_t remaining = count;
    uint64_t cur_lba = lba;

    while (remaining > 0) {
        uint64_t page_lba = (cur_lba / BCACHE_PAGE_SECTORS) * BCACHE_PAGE_SECTORS;
        uint32_t offset_in_page = (cur_lba % BCACHE_PAGE_SECTORS) * dev->sector_size;
        uint32_t sectors_left_in_page = BCACHE_PAGE_SECTORS - (cur_lba % BCACHE_PAGE_SECTORS);
        uint32_t to_transfer = (remaining < sectors_left_in_page) ? remaining : sectors_left_in_page;

        bcache_entry_t *e = find_entry(dev_name, page_lba);
        if (!e) {
            e = alloc_entry(dev_name, page_lba);
            spin_unlock_irqrestore(&bcache_lock, flags);
            
            dev->read(dev->local_device_struct, page_lba, BCACHE_PAGE_SECTORS, e->data);
            
            spin_lock_irqsave(&bcache_lock, &flags);
            e->valid = true;
        }

        memcpy(out, e->data + offset_in_page, to_transfer * dev->sector_size);

        out += to_transfer * dev->sector_size;
        cur_lba += to_transfer;
        remaining -= to_transfer;
    }

    spin_unlock_irqrestore(&bcache_lock, flags);
    return 0;
}

int bcache_write(const char *dev_name, uint64_t lba, uint64_t count, void *buf) {
    struct block_device *dev = find_device(dev_name);
    if (!dev || !dev->write) return -1;

    uint64_t flags;
    spin_lock_irqsave(&bcache_lock, &flags);

    const uint8_t *in = (const uint8_t *)buf;
    uint64_t remaining = count;
    uint64_t cur_lba = lba;

    while (remaining > 0) {
        uint64_t page_lba = (cur_lba / BCACHE_PAGE_SECTORS) * BCACHE_PAGE_SECTORS;
        uint32_t offset_in_page = (cur_lba % BCACHE_PAGE_SECTORS) * dev->sector_size;
        uint32_t sectors_left_in_page = BCACHE_PAGE_SECTORS - (cur_lba % BCACHE_PAGE_SECTORS);
        uint32_t to_transfer = (remaining < sectors_left_in_page) ? remaining : sectors_left_in_page;

        bcache_entry_t *e = find_entry(dev_name, page_lba);
        if (!e) {
            e = alloc_entry(dev_name, page_lba);
            if (to_transfer < BCACHE_PAGE_SECTORS) {
                spin_unlock_irqrestore(&bcache_lock, flags);
                dev->read(dev->local_device_struct, page_lba, BCACHE_PAGE_SECTORS, e->data);
                spin_lock_irqsave(&bcache_lock, &flags);
            }
        }

        memcpy(e->data + offset_in_page, in, to_transfer * dev->sector_size);
        e->dirty = true;

        in += to_transfer * dev->sector_size;
        cur_lba += to_transfer;
        remaining -= to_transfer;
    }

    spin_unlock_irqrestore(&bcache_lock, flags);
    return 0;
}

int bcache_flush(const char *dev_name) {
    struct block_device *dev = find_device(dev_name);
    if (!dev) return -1;

    uint64_t flags;
    spin_lock_irqsave(&bcache_lock, &flags);

    for (int i = 0; i < BCACHE_SLOTS; i++) {
        if (entries[i].valid && entries[i].dirty && my_strcmp(entries[i].dev_name, dev_name) == 0) {
            if (dev->write) {
                dev->write(dev->local_device_struct, entries[i].page_lba, BCACHE_PAGE_SECTORS, entries[i].data);
            }
            entries[i].dirty = false;
        }
    }

    spin_unlock_irqrestore(&bcache_lock, flags);

    if (dev->flush) {
        dev->flush(dev->local_device_struct);
    }
    return 0;
}
int bcache_drop_device(const char *dev_name) {
    if (!dev_name || !dev_name[0]) return -1;

    uint64_t flags;
    spin_lock_irqsave(&bcache_lock, &flags);

    for (int i = 0; i < BCACHE_SLOTS; i++) {
        if (!entries[i].valid || my_strcmp(entries[i].dev_name, dev_name) != 0)
            continue;
        if (entries[i].dirty) {
            struct block_device *d = find_device(dev_name);
            if (d && d->write)
                d->write(d->local_device_struct, entries[i].page_lba,
                         BCACHE_PAGE_SECTORS, entries[i].data);
        }
        entries[i].dirty = false;
        entries[i].valid = false;
        entries[i].dev_name[0] = 0;
    }

    spin_unlock_irqrestore(&bcache_lock, flags);
    return 0;
}

int bcache_invalidate_device(const char *dev_name) {
    if (!dev_name || !dev_name[0]) return -1;
    uint64_t flags;
    spin_lock_irqsave(&bcache_lock, &flags);
    for (int i = 0; i < BCACHE_SLOTS; i++) {
        if (!entries[i].valid || my_strcmp(entries[i].dev_name, dev_name) != 0)
            continue;
        entries[i].dirty = false;
        entries[i].valid = false;
        entries[i].dev_name[0] = 0;
    }
    spin_unlock_irqrestore(&bcache_lock, flags);
    return 0;
}
