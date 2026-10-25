#ifndef BLOCK_H
#define BLOCK_H

#include <stdint.h>
#include <mm/vmm.h>

#define MAX_BLOCK_DEVICES 1024

struct block_device;

typedef enum {
    DISK_TYPE_NVME,
    DISK_TYPE_AHCI,
    DISK_TYPE_USB
} disk_type_t;

typedef int (*block_read_fn)(void* local_device_struct, uint64_t lba, uint32_t count, void *buf_virt);
typedef int (*block_write_fn)(void* local_device_struct, uint64_t lba, uint32_t count, void *buf_virt);
typedef int (*block_flush_fn)(void* local_device_struct);

struct block_device {
    disk_type_t type;
    char name[32];
    uint32_t sector_size;
    uint64_t sector_count;
    void* local_device_struct;
    block_read_fn  read;
    block_write_fn write;
    block_flush_fn flush;
};

extern struct block_device block_devices[MAX_BLOCK_DEVICES];

struct block_device *find_device(const char *name);
int block_read(const char *name, uint64_t lba, uint64_t count, void *buf_virt);
int block_write(const char *name, uint64_t lba, uint64_t count, void *buf_virt);
int block_flush(const char *name);
void register_device(struct block_device *dev);
void unregister_device(const char *name);

#endif