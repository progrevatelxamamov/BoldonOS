#ifndef PARTITIONS_H
#define PARTITIONS_H

#include <stdbool.h>
#include <stdint.h>

static const uint8_t GPT_GUID_EMPTY[16] = {0};

static const uint8_t GPT_GUID_ESP[16] = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};

static const uint8_t GPT_GUID_BASIC_DATA[16] = {
    0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
    0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7
};

static const uint8_t GPT_GUID_LINUX_FS[16] = {
    0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
    0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4
};

static const uint8_t GPT_GUID_LINUX_SWAP[16] = {
    0x6D, 0xFD, 0x57, 0x06, 0xAB, 0xA4, 0xC4, 0x43,
    0x84, 0xE5, 0x09, 0x33, 0xC8, 0x4B, 0x4F, 0x4F
};

static const uint8_t GPT_GUID_MSR[16] = {
    0x16, 0xE3, 0xC9, 0xE3, 0x5C, 0x0B, 0xB8, 0x4D,
    0x81, 0x7D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE
};

typedef enum {
    PART_TYPE_UNKNOWN,
    PART_TYPE_MICROSOFT_RESERVED,
    PART_TYPE_LINUX_SWAP,
    PART_TYPE_LINUX_FILESYSTEM,
    PART_TYPE_MICROSOFT_BASIC_DATA_PARTITION,
    PART_TYPE_ESP,
} part_type_t;

typedef enum {
    PART_SCHEME_MBR,
    PART_SCHEME_GPT,
} part_scheme_t;

struct mbr_partition_entry {
    uint8_t  boot_indicator;
    uint8_t  start_chs[3];
    uint8_t  partition_type;
    uint8_t  end_chs[3];
    uint32_t start_lba;
    uint32_t sector_count;
} __attribute__((packed));

struct mbr {
    uint8_t  bootstrap[446];
    struct mbr_partition_entry partitions[4];
    uint16_t signature;
} __attribute__((packed));

struct gpt_header {
    uint8_t  signature[8];
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t  disk_guid[16];
    uint64_t partition_entries_lba;
    uint32_t num_partition_entries;
    uint32_t size_of_partition_entry;
    uint32_t partition_entries_crc32;
} __attribute__((packed));

struct gpt_partition_entry {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attributes;
    uint16_t name[36];
} __attribute__((packed));

struct partition {
    uint64_t start_lba;
    uint64_t sector_count;
    part_type_t type;
    bool bootable;
    char label[37];
    uint8_t guid[16];
    char guid_str[40];
    uint8_t type_guid[16];
    uint32_t volume_id;
    uint32_t gpt_index;
};

struct partition_table {
    const char *block_device_name;
    part_scheme_t scheme;
    uint32_t count;
    struct partition parts[64];
};

struct partition_table *get_partition_struct(const char *name);
void partitions_level_init(void);
int create_partition(const char *name, struct partition *new_part);
int delete_partition(const char *name, uint32_t index);
void guid_to_string(const uint8_t g[16], char *out);
void mount_all_partitions(void);
void partitions_set_boot_serial(const char *guid_str);
void partitions_set_boot_serial_bytes(const uint8_t guid[16]);

#endif