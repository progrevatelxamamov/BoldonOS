#include <kernel.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <disk/block.h>
#include <disk/partitions.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <mm/malloc.h>
#include <gconsole.h>
#include <memory.h>
#include <string.h>
#include <stdio.h>
#include <fs/vfs.h>
#include <fs/fat32.h>
#include <fs/ext4_vfs.h>

static uint32_t crc32_table[256];
static int crc32_table_ready = 0;

static void crc32_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
    crc32_table_ready = 1;
}

uint32_t crc32(const void *data, size_t len) {
    if (!crc32_table_ready) crc32_init();
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        crc = crc32_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

static uint8_t partition_type_to_mbr(part_type_t t) {
    switch (t) {
        case PART_TYPE_MICROSOFT_BASIC_DATA_PARTITION: return 0x0C;
        case PART_TYPE_LINUX_SWAP:                     return 0x82;
        case PART_TYPE_LINUX_FILESYSTEM:               return 0x83;
        case PART_TYPE_ESP:                            return 0xEF;
        default:                                       return 0x83;
    }
}

struct partition_table* part_tables[MAX_BLOCK_DEVICES];
uint64_t part_tables_count = 0;

void guid_to_string(const uint8_t g[16], char *out) {
    const char *hex = "0123456789ABCDEF";
    int k = 0;
    for (int i = 3; i >= 0; i--) { out[k++] = hex[g[i]>>4]; out[k++] = hex[g[i]&0xF]; }
    out[k++] = '-';
    for (int i = 5; i >= 4; i--) { out[k++] = hex[g[i]>>4]; out[k++] = hex[g[i]&0xF]; }
    out[k++] = '-';
    for (int i = 7; i >= 6; i--) { out[k++] = hex[g[i]>>4]; out[k++] = hex[g[i]&0xF]; }
    out[k++] = '-';
    for (int i = 8; i < 10; i++) { out[k++] = hex[g[i]>>4]; out[k++] = hex[g[i]&0xF]; }
    out[k++] = '-';
    for (int i = 10; i < 16; i++) { out[k++] = hex[g[i]>>4]; out[k++] = hex[g[i]&0xF]; }
    out[k] = 0;
}

static part_type_t gpt_type_to_part_type(const uint8_t guid[16]) {
    if (memcmp(guid, GPT_GUID_EMPTY, 16) == 0)      return PART_TYPE_UNKNOWN;
    if (memcmp(guid, GPT_GUID_MSR, 16) == 0)        return PART_TYPE_MICROSOFT_RESERVED;
    if (memcmp(guid, GPT_GUID_LINUX_SWAP, 16) == 0) return PART_TYPE_LINUX_SWAP;
    if (memcmp(guid, GPT_GUID_LINUX_FS, 16) == 0)   return PART_TYPE_LINUX_FILESYSTEM;
    if (memcmp(guid, GPT_GUID_BASIC_DATA, 16) == 0) return PART_TYPE_MICROSOFT_BASIC_DATA_PARTITION;
    if (memcmp(guid, GPT_GUID_ESP, 16) == 0)        return PART_TYPE_ESP;
    return PART_TYPE_UNKNOWN;
}

static part_type_t mbr_type_to_part_type(uint8_t t) {
    switch (t) {
        case 0x07: return PART_TYPE_MICROSOFT_BASIC_DATA_PARTITION;
        case 0x0B: return PART_TYPE_MICROSOFT_BASIC_DATA_PARTITION;
        case 0x0C: return PART_TYPE_MICROSOFT_BASIC_DATA_PARTITION;
        case 0x82: return PART_TYPE_LINUX_SWAP;
        case 0x83: return PART_TYPE_LINUX_FILESYSTEM;
        case 0xEF: return PART_TYPE_ESP;
        default:   return PART_TYPE_UNKNOWN;
    }
}

static int my_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

struct partition_table* get_partition_struct(const char *name) {
    for (uint64_t i = 0; i < part_tables_count; i++) {
        if (!part_tables[i]) continue;
        if (my_strcmp(part_tables[i]->block_device_name, name) == 0)
            return part_tables[i];
    }
    return NULL;
}

struct partition_table* osdev_get_partition_struct(const char *name) {
    struct block_device *dev = find_device(name);
    if (!dev || dev->sector_count == 0 || !dev->read) return NULL;

    void *mbr_page = pmm_alloc_page_and_clear();
    if (!mbr_page) return NULL;
    
    void *mbr_virt = (void *)((uint64_t)mbr_page + get_bootinfo().hhdm->offset);

    if (block_read(name, 0, 1, mbr_virt) != 0) {
        pmm_free_page(mbr_page);
        return NULL;
    }

    struct mbr *mbr_part = (struct mbr *)mbr_virt;
    if (mbr_part->signature != 0xAA55) {
        pmm_free_page(mbr_page);
        return NULL;
    }

    struct partition_table* part_table = (struct partition_table*)malloc_alloc(sizeof(struct partition_table));
    if (!part_table) {
        pmm_free_page(mbr_page);
        return NULL;
    }
    memset(part_table, 0, sizeof(struct partition_table));
    part_table->block_device_name = name;

    if (mbr_part->partitions[0].partition_type == 0xEE) {
        void *gpt_page = pmm_alloc_page_and_clear();
        if (!gpt_page) {
            pmm_free_page(mbr_page);
            malloc_free(part_table);
            return NULL;
        }
        void *gpt_virt = (void *)((uint64_t)gpt_page + get_bootinfo().hhdm->offset);

        if (block_read(name, 1, 1, gpt_virt) != 0) {
            pmm_free_page(gpt_page);
            pmm_free_page(mbr_page);
            malloc_free(part_table);
            return NULL;
        }

        struct gpt_header *gpt_part = (struct gpt_header *)gpt_virt;
        if (memcmp(gpt_part->signature, "EFI PART", 8) != 0) {
            pmm_free_page(gpt_page);
            pmm_free_page(mbr_page);
            malloc_free(part_table);
            return NULL;
        }

        part_table->scheme = PART_SCHEME_GPT;

        uint32_t total_bytes = gpt_part->num_partition_entries * gpt_part->size_of_partition_entry;
        uint32_t total_sectors = (total_bytes + 511) / 512;
        uint32_t pages_needed = (total_bytes + 4095) / 4096;

        void *entries_phys = pmm_alloc_pages(pages_needed);
        if (!entries_phys) {
            pmm_free_page(gpt_page);
            pmm_free_page(mbr_page);
            malloc_free(part_table);
            return NULL;
        }
        void *entries_virt = (void *)((uint64_t)entries_phys + get_bootinfo().hhdm->offset);

        if (block_read(name, gpt_part->partition_entries_lba, total_sectors, entries_virt) != 0) {
            pmm_free_pages(entries_phys, pages_needed);
            pmm_free_page(gpt_page);
            pmm_free_page(mbr_page);
            malloc_free(part_table);
            return NULL;
        }

        uint64_t offset = 0;
        for (uint64_t i = 0; i < gpt_part->num_partition_entries; i++) {
            if (part_table->count >= 64) break;

            struct gpt_partition_entry *gpt_part_entry = (struct gpt_partition_entry *)((uint8_t *)entries_virt + offset);
            offset += gpt_part->size_of_partition_entry;

            if (memcmp(gpt_part_entry->type_guid, GPT_GUID_EMPTY, 16) == 0) continue;

            struct partition *p = &part_table->parts[part_table->count];
            memset(p, 0, sizeof(struct partition));

            p->start_lba = gpt_part_entry->first_lba;
            p->sector_count = gpt_part_entry->last_lba - gpt_part_entry->first_lba + 1;

            int k;
            for (k = 0; k < 36 && gpt_part_entry->name[k] != 0; k++) {
                uint16_t c = gpt_part_entry->name[k];
                p->label[k] = (c < 128) ? (char)c : '?';
            }
            p->label[k] = 0;

            p->bootable = (memcmp(gpt_part_entry->type_guid, GPT_GUID_ESP, 16) == 0);
            memcpy(p->guid, gpt_part_entry->unique_guid, 16);
            guid_to_string(gpt_part_entry->unique_guid, p->guid_str);
            memcpy(p->type_guid, gpt_part_entry->type_guid, 16);
            p->type = gpt_type_to_part_type(gpt_part_entry->type_guid);
            p->gpt_index = i;

            part_table->count++;
        }

        pmm_free_pages(entries_phys, pages_needed);
        pmm_free_page(gpt_page);
        pmm_free_page(mbr_page);
    } else {
        part_table->scheme = PART_SCHEME_MBR;
        part_table->count = 0;

        for (int i = 0; i < 4; i++) {
            if (mbr_part->partitions[i].partition_type == 0) continue;
            if (part_table->count >= 64) break;

            struct partition *p = &part_table->parts[part_table->count];
            memset(p, 0, sizeof(struct partition));

            p->start_lba    = mbr_part->partitions[i].start_lba;
            p->sector_count = mbr_part->partitions[i].sector_count;
            p->bootable     = (mbr_part->partitions[i].boot_indicator == 0x80);
            p->type         = mbr_type_to_part_type(mbr_part->partitions[i].partition_type);
            p->gpt_index    = i;

            part_table->count++;
        }

        pmm_free_page(mbr_page);
    }

    return part_table;
}

uint64_t get_partitions_count(struct partition_table* pt) { return pt->count; }

void partitions_level_init(void) {
    part_tables_count = 0;
    for (uint64_t i = 0; i < MAX_BLOCK_DEVICES; i++) {
        if (block_devices[i].sector_count > 0 && block_devices[i].name[0] != 0 && block_devices[i].read != NULL) {
            struct partition_table* pt = osdev_get_partition_struct(block_devices[i].name);
            if (pt) {
                part_tables[part_tables_count++] = pt;
            }
        }
    }
}

static int gpt_add_partition(const char *name,
                             struct gpt_header *hdr,
                             struct partition *new_part) {
    uint32_t entry_size = hdr->size_of_partition_entry;
    uint32_t entry_count = hdr->num_partition_entries; 
    uint32_t entries_bytes = entry_count * entry_size;
    uint32_t entries_sectors = (entries_bytes + 511) / 512;
    uint32_t pages_needed = (entries_bytes + 4095) / 4096;

    uint64_t entries_phys = (uint64_t)pmm_alloc_pages(pages_needed);
    if (!entries_phys) return -1;
    void *entries_virt = (void *)(entries_phys + get_bootinfo().hhdm->offset);
    memset(entries_virt, 0, pages_needed * 4096);

    if (block_read(name, hdr->partition_entries_lba, entries_sectors, entries_virt) != 0) {
        pmm_free_page((void *)entries_phys);
        return -2;
    }

    int free_idx = -1;
    for (uint32_t i = 0; i < entry_count; i++) {
        struct gpt_partition_entry *e =
            (struct gpt_partition_entry *)((uint8_t *)entries_virt + i * entry_size);
        if (memcmp(e->type_guid, GPT_GUID_EMPTY, 16) == 0) {
            free_idx = i;
            break;
        }
    }
    if (free_idx < 0) {
        pmm_free_page((void *)entries_phys);
        return -3;
    }

    struct gpt_partition_entry *e =
        (struct gpt_partition_entry *)((uint8_t *)entries_virt + free_idx * entry_size);

    memcpy(e->type_guid, new_part->type_guid, 16);
    memcpy(e->unique_guid, new_part->guid, 16);
    e->first_lba = new_part->start_lba;
    e->last_lba  = new_part->start_lba + new_part->sector_count - 1;
    e->attributes = 0;

    for (int i = 0; i < 36; i++) {
        char c = new_part->label[i];
        e->name[i] = (uint16_t)(uint8_t)c;
        if (c == 0) {
            for (int j = i + 1; j < 36; j++) e->name[j] = 0;
            break;
        }
    }

    uint32_t entries_crc = crc32(entries_virt, entries_bytes);
    hdr->partition_entries_crc32 = entries_crc;

    hdr->header_crc32 = 0;
    uint32_t hdr_crc = crc32(hdr, hdr->header_size);
    hdr->header_crc32 = hdr_crc;

    if (block_write(name, hdr->partition_entries_lba, entries_sectors, entries_virt) != 0) {
        pmm_free_page((void *)entries_phys);
        return -4;
    }

    if (block_write(name, 1, 1, (void *)((uint64_t)hdr)) != 0) {
        pmm_free_page((void *)entries_phys);
        return -5;
    }

    uint64_t backup_entries_lba = hdr->backup_lba - entries_sectors;
    if (block_write(name, backup_entries_lba, entries_sectors, entries_virt) != 0) {
        pmm_free_page((void *)entries_phys);
        return -6;
    }

    struct gpt_header backup_hdr;
    memcpy(&backup_hdr, hdr, sizeof(backup_hdr));
    backup_hdr.current_lba = hdr->backup_lba;
    backup_hdr.backup_lba  = hdr->current_lba;
    backup_hdr.partition_entries_lba = backup_entries_lba;

    backup_hdr.header_crc32 = 0;
    backup_hdr.header_crc32 = crc32(&backup_hdr, backup_hdr.header_size);

    uint64_t tmp_phys = (uint64_t)pmm_alloc_page_and_clear();
    void *tmp_virt = (void *)(tmp_phys + get_bootinfo().hhdm->offset);
    memcpy(tmp_virt, &backup_hdr, sizeof(backup_hdr));

    if (block_write(name, hdr->backup_lba, 1, tmp_virt) != 0) {
        pmm_free_page((void *)tmp_phys);
        pmm_free_page((void *)entries_phys);
        return -7;
    }
    pmm_free_page((void *)tmp_phys);

    block_flush(name);
    pmm_free_page((void *)entries_phys);
    return 0;
}

static int mbr_add_partition(const char *name,
                             struct mbr *mbr,
                             struct partition *new_part) {
    int free_idx = -1;
    for (int i = 0; i < 4; i++) {
        if (mbr->partitions[i].partition_type == 0) {
            free_idx = i;
            break;
        }
    }
    if (free_idx < 0) return -1;

    struct mbr_partition_entry *e = &mbr->partitions[free_idx];

    e->boot_indicator = new_part->bootable ? 0x80 : 0x00;
    e->start_chs[0] = e->start_chs[1] = e->start_chs[2] = 0xFF;
    e->partition_type = partition_type_to_mbr(new_part->type);
    e->end_chs[0] = e->end_chs[1] = e->end_chs[2] = 0xFF;
    e->start_lba = (uint32_t)new_part->start_lba;
    e->sector_count = (uint32_t)new_part->sector_count;

    uint64_t buf_phys = (uint64_t)pmm_alloc_page_and_clear();
    void *buf = (void *)(buf_phys + get_bootinfo().hhdm->offset);
    memcpy(buf, mbr, sizeof(struct mbr));

    if (block_write(name, 0, 1, buf) != 0) {
        pmm_free_page((void *)buf_phys);
        return -2;
    }
    pmm_free_page((void *)buf_phys);

    block_flush(name);
    return 0;
}

static int gpt_delete_partition(const char *name,
                                struct gpt_header *hdr,
                                uint32_t index) {
    uint32_t entry_size  = hdr->size_of_partition_entry;
    uint32_t entry_count = hdr->num_partition_entries;
    uint32_t entries_bytes   = entry_count * entry_size;
    uint32_t entries_sectors = (entries_bytes + 511) / 512;
    uint32_t pages_needed    = (entries_bytes + 4095) / 4096;

    if (index >= entry_count) return -1;

    uint64_t entries_phys = (uint64_t)pmm_alloc_pages(pages_needed);
    if (!entries_phys) return -1;
    void *entries_virt = (void *)(entries_phys + get_bootinfo().hhdm->offset);
    memset(entries_virt, 0, pages_needed * 4096);

    if (block_read(name, hdr->partition_entries_lba, entries_sectors, entries_virt) != 0) {
        pmm_free_page((void *)entries_phys);
        return -2;
    }

    struct gpt_partition_entry *e =
        (struct gpt_partition_entry *)((uint8_t *)entries_virt + index * entry_size);

    if (memcmp(e->type_guid, GPT_GUID_EMPTY, 16) == 0) {
        pmm_free_page((void *)entries_phys);
        return -3;
    }

    memset(e, 0, entry_size);

    hdr->partition_entries_crc32 = crc32(entries_virt, entries_bytes);
    hdr->header_crc32 = 0;
    hdr->header_crc32 = crc32(hdr, hdr->header_size);

    if (block_write(name, hdr->partition_entries_lba, entries_sectors, entries_virt) != 0) {
        pmm_free_page((void *)entries_phys);
        return -4;
    }
    if (block_write(name, 1, 1, (void *)hdr) != 0) {
        pmm_free_page((void *)entries_phys);
        return -5;
    }

    uint64_t backup_entries_lba = hdr->backup_lba - entries_sectors;
    if (block_write(name, backup_entries_lba, entries_sectors, entries_virt) != 0) {
        pmm_free_page((void *)entries_phys);
        return -6;
    }

    struct gpt_header backup_hdr;
    memcpy(&backup_hdr, hdr, sizeof(backup_hdr));
    backup_hdr.current_lba = hdr->backup_lba;
    backup_hdr.backup_lba  = hdr->current_lba;
    backup_hdr.partition_entries_lba = backup_entries_lba;
    backup_hdr.header_crc32 = 0;
    backup_hdr.header_crc32 = crc32(&backup_hdr, backup_hdr.header_size);

    uint64_t tmp_phys = (uint64_t)pmm_alloc_page_and_clear();
    void *tmp_virt = (void *)(tmp_phys + get_bootinfo().hhdm->offset);
    memcpy(tmp_virt, &backup_hdr, sizeof(backup_hdr));

    if (block_write(name, hdr->backup_lba, 1, tmp_virt) != 0) {
        pmm_free_page((void *)tmp_phys);
        pmm_free_page((void *)entries_phys);
        return -7;
    }
    pmm_free_page((void *)tmp_phys);
    pmm_free_page((void *)entries_phys);

    block_flush(name);
    return 0;
}

static int mbr_delete_partition(const char *name,
                                struct mbr *mbr,
                                uint32_t index) {
    if (index >= 4) return -1;
    if (mbr->partitions[index].partition_type == 0) return -3;

    memset(&mbr->partitions[index], 0, sizeof(struct mbr_partition_entry));

    uint64_t buf_phys = (uint64_t)pmm_alloc_page_and_clear();
    void *buf = (void *)(buf_phys + get_bootinfo().hhdm->offset);
    memcpy(buf, mbr, sizeof(struct mbr));

    if (block_write(name, 0, 1, buf) != 0) {
        pmm_free_page((void *)buf_phys);
        return -2;
    }
    pmm_free_page((void *)buf_phys);

    block_flush(name);
    return 0;
}

int create_partition(const char *name, struct partition *new_part) {
    if (!new_part) return -1;

    struct partition_table *pt = get_partition_struct(name);
    if (!pt) return -2;

    uint64_t ns = new_part->start_lba;
    uint64_t ne = ns + new_part->sector_count;
    for (uint32_t i = 0; i < pt->count; i++) {
        uint64_t s = pt->parts[i].start_lba;
        uint64_t e = pt->parts[i].start_lba + pt->parts[i].sector_count;
        if (ns < e && ne > s) return -3;
    }

    if (pt->scheme == PART_SCHEME_GPT) {
        uint64_t hdr_phys = (uint64_t)pmm_alloc_page_and_clear();
        void *hdr_virt = (void *)(hdr_phys + get_bootinfo().hhdm->offset);

        if (block_read(name, 1, 1, hdr_virt) != 0) {
            pmm_free_page((void *)hdr_phys);
            return -4;
        }

        struct gpt_header *hdr = (struct gpt_header *)hdr_virt;
        int rc = gpt_add_partition(name, hdr, new_part);
        pmm_free_page((void *)hdr_phys);
        if (rc != 0) return rc;
    } else {
        uint64_t mbr_phys = (uint64_t)pmm_alloc_page_and_clear();
        void *mbr_virt = (void *)(mbr_phys + get_bootinfo().hhdm->offset);

        if (block_read(name, 0, 1, mbr_virt) != 0) {
            pmm_free_page((void *)mbr_phys);
            return -4;
        }

        struct mbr *mbr = (struct mbr *)mbr_virt;
        int rc = mbr_add_partition(name, mbr, new_part);
        pmm_free_page((void *)mbr_phys);
        if (rc != 0) return rc;
    }

    memcpy(&pt->parts[pt->count], new_part, sizeof(struct partition));
    pt->count++;
    return 0;
}

int delete_partition(const char *name, uint32_t index) {
    struct partition_table *pt = get_partition_struct(name);
    if (!pt) return -2;
    if (index >= pt->count) return -3;

    if (pt->scheme == PART_SCHEME_GPT) {
        uint64_t hdr_phys = (uint64_t)pmm_alloc_page_and_clear();
        void *hdr_virt = (void *)(hdr_phys + get_bootinfo().hhdm->offset);
        if (block_read(name, 1, 1, hdr_virt) != 0) {
            pmm_free_page((void *)hdr_phys);
            return -4;
        }
        struct gpt_header *hdr = (struct gpt_header *)hdr_virt;

        uint32_t real_idx = pt->parts[index].gpt_index;
        int rc = gpt_delete_partition(name, hdr, real_idx);
        pmm_free_page((void *)hdr_phys);
        if (rc != 0) return rc;
    } else {
        uint64_t mbr_phys = (uint64_t)pmm_alloc_page_and_clear();
        void *mbr_virt = (void *)(mbr_phys + get_bootinfo().hhdm->offset);
        if (block_read(name, 0, 1, mbr_virt) != 0) {
            pmm_free_page((void *)mbr_phys);
            return -4;
        }
        int rc = mbr_delete_partition(name, (struct mbr *)mbr_virt, index);
        pmm_free_page((void *)mbr_phys);
        if (rc != 0) return rc;
    }

    for (uint32_t i = index; i + 1 < pt->count; i++) {
        memcpy(&pt->parts[i], &pt->parts[i + 1], sizeof(struct partition));
    }
    pt->count--;
    return 0;
}

static char boot_serial_str[40];
static uint8_t boot_serial_bytes[16];
static bool boot_serial_str_set = false;
static bool boot_serial_bytes_set = false;

void partitions_set_boot_serial(const char *guid_str) {
    if (!guid_str) return;
    int i = 0;
    while (guid_str[i] && i < 39) {
        boot_serial_str[i] = guid_str[i];
        i++;
    }
    boot_serial_str[i] = 0;
    boot_serial_str_set = true;
}

void partitions_set_boot_serial_bytes(const uint8_t guid[16]) {
    if (!guid) return;
    memcpy(boot_serial_bytes, guid, 16);
    boot_serial_bytes_set = true;
    guid_to_string(guid, boot_serial_str);
    boot_serial_str_set = true;
}

static int str_eq_ci(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static bool guid_bytes_eq(const uint8_t a[16], const uint8_t b[16]) {
    for (int i = 0; i < 16; i++) if (a[i] != b[i]) return false;
    return true;
}

static bool is_fat32_sector(void *sector_buf) {
    uint8_t *b = (uint8_t *)sector_buf;
    if (!(b[0] == 0xEB || b[0] == 0xE9)) return false;
    uint16_t bps = b[11] | (b[12] << 8);
    if (bps != 512 && bps != 1024 && bps != 2048 && bps != 4096) return false;
    if (b[13] == 0) return false;
    if (b[16] == 0) return false;
    uint16_t root_ents = b[17] | (b[18] << 8);
    uint16_t fat16 = b[22] | (b[23] << 8);
    uint32_t fat32 = b[36] | (b[37] << 8) | (b[38] << 16) | (b[39] << 24);
    if (root_ents != 0) return false;
    if (fat16 != 0) return false;
    if (fat32 == 0) return false;
    return true;
}

static uint32_t fat32_read_volume_id(const char *dev, uint64_t lba) {
    uint8_t buf[512];
    if (block_read(dev, lba, 1, buf) != 0) return 0;
    return buf[67] | (buf[68] << 8) | (buf[69] << 16) | (buf[70] << 24);
}

static bool partition_is_boot(struct partition *p) {
    if (boot_serial_bytes_set) {
        bool empty = true;
        for (int i = 0; i < 16; i++) if (p->guid[i]) empty = false;
        if (!empty && guid_bytes_eq(p->guid, boot_serial_bytes)) return true;
    }
    if (boot_serial_str_set && p->guid_str[0]) {
        if (str_eq_ci(p->guid_str, boot_serial_str)) return true;
    }
    return false;
}

void mount_all_partitions(void) {
    console_print_string("[DEBUG] Starting mount_all_partitions()...\n");
    vfs_init();
    fat32_vfs_init();
    ext4_vfs_init();

    char next_letter = 'E';
    bool c_mounted = false;
    int boot_cand_idx = -1;

    typedef struct {
        const char *dev;
        uint64_t lba;
        struct partition *part;
        bool is_boot;
        const char *fs_type;
    } cand_t;

    cand_t cands[256];
    int cand_count = 0;

    console_print_string("[DEBUG] Scanning partition tables...\n");
    for (uint64_t ti = 0; ti < part_tables_count; ti++) {
        struct partition_table *pt = part_tables[ti];
        if (!pt || !pt->block_device_name) {
            console_print_string("[DEBUG] Skip: empty or unnamed partition table\n");
            continue;
        }

        console_print_string("[DEBUG] Checking device: ");
        console_print_string(pt->block_device_name);
        console_print_string("\n");

        for (uint32_t pi = 0; pi < pt->count; pi++) {
            struct partition *p = &pt->parts[pi];
            if (p->sector_count == 0) continue;

            console_print_string("[DEBUG] Found Partition [");
            // Печатаем индекс раздела для понимания
            char idx_str[16];
            snprintf(idx_str, sizeof(idx_str), "%d", pi);
            console_print_string(idx_str);
            console_print_string("] -> Start LBA: ");
            
            char lba_str[32];
            snprintf(lba_str, sizeof(lba_str), "%llu", p->start_lba);
            console_print_string(lba_str);
            console_print_string(", Sectors: ");
            
            char cnt_str[32];
            snprintf(cnt_str, sizeof(cnt_str), "%llu", p->sector_count);
            console_print_string(cnt_str);
            console_print_string("\n");

            void *page_buf = (void *)((uint64_t)pmm_alloc_page() + get_bootinfo().hhdm->offset);
            if (block_read(pt->block_device_name, p->start_lba, 8, page_buf) != 0) {
                console_print_string("[DEBUG] ! ERROR: block_read failed for this partition!\n");
                pmm_free_page((void *)((uint64_t)page_buf - get_bootinfo().hhdm->offset));
                continue;
            }

            uint8_t *raw = (uint8_t *)page_buf;
            const char *fst = NULL;

            // Выведем сигнатуры из секторов для ручного контроля
            uint16_t fat_sig = *(uint16_t *)(raw + 510);
            uint16_t ext4_magic = *(uint16_t *)(raw + 1024 + 0x38);
            
            char sig_str[64];
            snprintf(sig_str, sizeof(sig_str), "[DEBUG] Raw signatures: FAT_end=0x%x, EXT4_magic=0x%x\n", fat_sig, ext4_magic);
            console_print_string(sig_str);

            if (is_fat32_sector(raw)) {
                fst = "fat32";
                p->volume_id = fat32_read_volume_id(pt->block_device_name, p->start_lba);
                console_print_string("[DEBUG] -> Detected FAT32!\n");
            } else if (is_ext4_sector(raw + 1024)) {
                fst = "ext4";
                console_print_string("[DEBUG] -> Detected EXT4!\n");
            } else {
                console_print_string("[DEBUG] -> FS Unknown or BOLDON_ROOT label missing\n");
            }

            if (fst && cand_count < 256) {
                cands[cand_count].dev = pt->block_device_name;
                cands[cand_count].lba = p->start_lba;
                cands[cand_count].part = p;
                cands[cand_count].is_boot = partition_is_boot(p);
                cands[cand_count].fs_type = fst;
                cand_count++;
            }

            pmm_free_page((void *)((uint64_t)page_buf - get_bootinfo().hhdm->offset));
        }
    }

    char cand_str[64];
    snprintf(cand_str, sizeof(cand_str), "[DEBUG] Total valid candidates found: %d\n", cand_count);
    console_print_string(cand_str);

    // Этап 1: Попытка примонтировать Boot-раздел
    console_print_string("[DEBUG] Phase 1: Mounting boot partition...\n");
    for (int i = 0; i < cand_count; i++) {
        if (!cands[i].is_boot) continue;

        console_print_string("[DEBUG] Trying boot cand: ");
        console_print_string(cands[i].fs_type);
        console_print_string(" on ");
        console_print_string(cands[i].dev);
        console_print_string("\n");

        int res = vfs_mount_fs(cands[i].fs_type, cands[i].dev, 'C', (void *)cands[i].lba);
        if (res == 0) {
            vfs_set_boot_drive('C');
            c_mounted = true;
            boot_cand_idx = i;
            console_print_string("[DEBUG] Success mounting C: (Boot flag candidate)\n");
            break;
        } else {
            char err_str[32];
            snprintf(err_str, sizeof(err_str), "[DEBUG] Fail code: %d\n", res);
            console_print_string(err_str);
        }
    }

    // Этап 2: Если с флагом boot не нашлось, ищем любой ext4
    if (!c_mounted) {
        console_print_string("[DEBUG] Phase 2: No boot flag found. Looking for any EXT4 partition...\n");
        for (int i = 0; i < cand_count; i++) {
            if (strcmp(cands[i].fs_type, "ext4") == 0) {
                console_print_string("[DEBUG] Trying EXT4 cand on ");
                console_print_string(cands[i].dev);
                console_print_string("\n");

                int res = vfs_mount_fs("ext4", cands[i].dev, 'C', (void *)cands[i].lba);
                if (res == 0) {
                    vfs_set_boot_drive('C');
                    c_mounted = true;
                    boot_cand_idx = i;
                    console_print_string("[DEBUG] Success mounting C: (Fallback to first EXT4)\n");
                    break;
                } else {
                    char err_str[32];
                    snprintf(err_str, sizeof(err_str), "[DEBUG] Fail code: %d\n", res);
                    console_print_string(err_str);
                }
            }
        }
    }

    // Этап 3: Если всё ещё глухо, берем самый первый попавшийся
    if (!c_mounted && cand_count > 0) {
        console_print_string("[DEBUG] Phase 3: Absolute fallback. Trying candidate index 0...\n");
        int res = vfs_mount_fs(cands[0].fs_type, cands[0].dev, 'C', (void *)cands[0].lba);
        if (res == 0) {
            vfs_set_boot_drive('C');
            c_mounted = true;
            boot_cand_idx = 0;
            console_print_string("[DEBUG] Success mounting C: (Candidate 0)\n");
        } else {
            char err_str[32];
            snprintf(err_str, sizeof(err_str), "[DEBUG] Absolute fail code: %d\n", res);
            console_print_string(err_str);
        }
    }

    // Этап 4: Монтируем всё остальное как D:, E: и т.д.
    console_print_string("[DEBUG] Phase 4: Mounting secondary partitions...\n");
    for (int i = 0; i < cand_count; i++) {
        if (i == boot_cand_idx) continue;

        if (next_letter > 'Z') {
            console_print_string("[DEBUG] Out of drive letters (Z reached)!\n");
            break;
        }

        char mount_log[128];
        snprintf(mount_log, sizeof(mount_log), "[DEBUG] Mounting secondary %s (%s) to %c:\n", cands[i].dev, cands[i].fs_type, next_letter);
        console_print_string(mount_log);

        if (vfs_mount_fs(cands[i].fs_type, cands[i].dev, next_letter, (void *)cands[i].lba) == 0) {
            console_print_string("[DEBUG] Mounted successfully!\n");
            next_letter++;
        } else {
            console_print_string("[DEBUG] Mount failed!\n");
        }
    }

    console_print_string("\n==================================================================\n");
    console_print_string("[MOUNT] Boot Drive C: ");
    if (c_mounted && boot_cand_idx >= 0) {
        console_print_string("MOUNTED (FS: ");
        console_print_string(cands[boot_cand_idx].fs_type);
        console_print_string(")\n");
    } else {
        console_print_string("NOT MOUNTED (FAIL)\n");
    }
    console_print_string("==================================================================\n\n");
}

int partitions_add_table(struct partition_table *pt) {
    if (!pt || !pt->block_device_name) return -1;

    for (uint64_t i = 0; i < part_tables_count; i++) {
        if (!part_tables[i]) continue;
        if (my_strcmp(part_tables[i]->block_device_name, pt->block_device_name) == 0) {
            void *old = part_tables[i];
            part_tables[i] = pt;
            if (old && old != pt) malloc_free(old);
            return 0;
        }
    }

    for (uint64_t i = 0; i < MAX_BLOCK_DEVICES; i++) {
        if (!part_tables[i]) {
            part_tables[i] = pt;
            if (i >= part_tables_count) part_tables_count = i + 1;
            return 0;
        }
    }
    return -1;
}

void partitions_remove_table(const char *name) {
    if (!name) return;
    for (uint64_t i = 0; i < part_tables_count; i++) {
        if (!part_tables[i] || !part_tables[i]->block_device_name) continue;
        if (my_strcmp(part_tables[i]->block_device_name, name) != 0) continue;
        void *old = part_tables[i];
        part_tables[i] = NULL;
        if (old) malloc_free(old);
    }
    while (part_tables_count > 0 && !part_tables[part_tables_count - 1])
        part_tables_count--;
}

const char *partitions_detect_fs(const char *dev, uint64_t lba) {
    void *page = pmm_alloc_page();
    if (!page) return NULL;
    void *buf = (void *)((uint64_t)page + get_bootinfo().hhdm->offset);
    memset(buf, 0, 4096);

    const char *result = NULL;
    if (block_read(dev, lba, 8, buf) == 0) {
        uint8_t *raw = (uint8_t *)buf;
        if (is_fat32_sector(raw)) result = "fat32";
        else if (is_ext4_sector(raw + 1024)) result = "ext4";
    }
    pmm_free_page(page);
    return result;
}
