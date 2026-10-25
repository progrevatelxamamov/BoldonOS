#include <disk/block.h>
#include <disk/bcache.h>
#include <memory.h>
#include <string.h>
#include <mm/malloc.h>
#include <stdbool.h>

struct block_device block_devices[MAX_BLOCK_DEVICES];

static void int_to_str(int num, char *str) {
    int i = 0;
    if (num == 0) {
        str[i++] = '0';
        str[i] = '\0';
        return;
    }
    char temp[12];
    int j = 0;
    while (num > 0) {
        temp[j++] = (num % 10) + '0';
        num /= 10;
    }
    while (j > 0) {
        str[i++] = temp[--j];
    }
    str[i] = '\0';
}

static char *strcat_simple(char *dest, const char *src) {
    char *temp = dest;
    while (*dest) {
        dest++;
    }
    while ((*dest++ = *src++)) {
    }
    return temp;
}

struct block_device *find_device(const char *name) {
    for (int i = 0; i < MAX_BLOCK_DEVICES; i++) {
        if (block_devices[i].sector_count > 0 && strcmp(block_devices[i].name, name) == 0) {
            return &block_devices[i];
        }
    }
    return NULL;
}

int block_read(const char *name, uint64_t lba, uint64_t count, void *buf_virt) {
    return bcache_read(name, lba, count, buf_virt);
}

int block_write(const char *name, uint64_t lba, uint64_t count, void *buf_virt) {
    return bcache_write(name, lba, count, buf_virt);
}

int block_flush(const char *name) {
    return bcache_flush(name);
}

void register_device(struct block_device *dev) {
    int free_slot = -1;
    for (int i = 0; i < MAX_BLOCK_DEVICES; i++) {
        if (block_devices[i].sector_count == 0 && block_devices[i].name[0] == '\0') {
            free_slot = i;
            break;
        }
    }
    if (free_slot == -1) return;

    const char *prefix = "dev";
    if (dev->type == DISK_TYPE_USB) prefix = "usb";
    else if (dev->type == DISK_TYPE_NVME) prefix = "nvme";
    else if (dev->type == DISK_TYPE_AHCI) prefix = "ahci";

    int index = 0;
    while (1) {
        char test_name[32];
        char idx_str[12];
        int_to_str(index, idx_str);
        strcpy(test_name, prefix);
        strcat_simple(test_name, idx_str);

        bool conflict = false;
        for (int i = 0; i < MAX_BLOCK_DEVICES; i++) {
            if (block_devices[i].sector_count > 0 && strcmp(block_devices[i].name, test_name) == 0) {
                conflict = true;
                break;
            }
        }
        if (!conflict) {
            strcpy(dev->name, test_name);
            break;
        }
        index++;
    }

    block_devices[free_slot] = *dev;
}

void unregister_device(const char *name) {
    for (int i = 0; i < MAX_BLOCK_DEVICES; i++) {
        if (strcmp(block_devices[i].name, name) == 0) {
            memset(&block_devices[i], 0, sizeof(struct block_device));
            break;
        }
    }
}