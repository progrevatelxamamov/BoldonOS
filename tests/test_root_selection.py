"""Run the real early/hotplug root selection against an NVMe ESP and late USB root."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'kernel/src/disk/partitions.c').read_text()
selector = source[source.index('static bool partition_is_root('):source.index('void mount_all_partitions(')]
early = source[source.index('void mount_all_partitions('):source.index('int partitions_add_table(')]
late = source[source.index('static volatile uint32_t root_mount_guard;'):]

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct partition { uint64_t start_lba, sector_count; char label[37]; uint32_t volume_id; };
struct partition_table { const char *block_device_name; uint32_t count; struct partition parts[4]; };
typedef struct { char device[32], fs_name[16]; uint64_t start_lba; bool dead; } vfs_mount_t;
static struct partition_table *part_tables[4];
static uint64_t part_tables_count;
static vfs_mount_t mounts[26];
static bool mounted[26];
static unsigned root_mounts;
static bool fail_root_once;
static uint8_t page[4096];
static struct { uint64_t offset; } hhdm;
static struct { typeof(hhdm) *hhdm; } get_bootinfo(void) { return (typeof(get_bootinfo())){&hhdm}; }
static void *pmm_alloc_page(void) { return page; }
static void pmm_free_page(void *p) { assert(p == page); }
static void console_print_string(const char *s) { (void)s; }
static void console_print_num(uint64_t n) { (void)n; }
static bool partition_is_boot(struct partition *p) { (void)p; return true; } // NVMe boot_serial matches
static bool is_fat32_sector(const void *p) { return ((const uint8_t *)p)[0] == 0xeb; }
static bool is_ext4_sector(const uint8_t *p) { return p[0x38] == 0x53 && p[0x39] == 0xef; }
static uint32_t fat32_read_volume_id(const char *dev, uint64_t lba) {
    (void)dev; (void)lba; return 42;
}
static int block_read(const char *dev, uint64_t lba, uint32_t sectors, void *buf) {
    assert(sectors == 8);
    memset(buf, 0, 4096);
    if (strcmp(dev, "nvme0") == 0 && lba == 2048) ((uint8_t *)buf)[0] = 0xeb;
    else if ((strcmp(dev, "nvme0") == 0 && lba == 4096) ||
             (strcmp(dev, "usb0") == 0 && lba == 8192)) {
        ((uint8_t *)buf)[1024 + 0x38] = 0x53;
        ((uint8_t *)buf)[1024 + 0x39] = 0xef;
    } else assert(0 && "unexpected device/LBA");
    return 0;
}
static const char *partitions_detect_fs(const char *dev, uint64_t lba) {
    if (strcmp(dev, "nvme0") == 0) return lba == 2048 ? "fat32" : "ext4";
    if (strcmp(dev, "usb0") == 0 && lba == 8192) return "ext4";
    return NULL;
}
static vfs_mount_t *vfs_get_mount(char c) { return mounted[c - 'A'] ? &mounts[c - 'A'] : NULL; }
static int vfs_mount_fs(const char *fs, const char *dev, char c, void *data) {
    if (c == 'C' && fail_root_once) { fail_root_once = false; return -5; }
    assert(!mounted[c - 'A']);
    vfs_mount_t *m = &mounts[c - 'A'];
    mounted[c - 'A'] = true;
    strcpy(m->device, dev); strcpy(m->fs_name, fs); m->start_lba = (uintptr_t)data;
    if (c == 'C') root_mounts++;
    return 0;
}
static void vfs_set_boot_drive(char c) { assert(c == 'C'); }
static void vfs_init(void) { memset(mounted, 0, sizeof(mounted)); root_mounts = 0; }
static void fat32_vfs_init(void) {}
static void ext4_vfs_init(void) {}
'''
suffix = r'''
int main(void) {
    struct partition_table nvme = {.block_device_name = "nvme0", .count = 2,
        .parts = {{.start_lba = 2048, .sector_count = 100, .label = "EFI System"},
                  {.start_lba = 4096, .sector_count = 100, .label = "OTHER_ROOT"}}};
    struct partition_table usb = {.block_device_name = "usb0", .count = 1,
        .parts = {{.start_lba = 8192, .sector_count = 100, .label = "BOLDON_ROOT"}}};
    assert(!partition_is_root(&nvme.parts[0], "fat32"));
    assert(!partition_is_root(&nvme.parts[1], "ext4"));
    assert(!partition_is_root(&usb.parts[0], "fat32"));
    assert(partition_is_root(&usb.parts[0], "ext4"));
    part_tables[0] = &nvme; part_tables_count = 1;
    mount_all_partitions();
    assert(!vfs_get_mount('C') && root_mounts == 0);
    assert(vfs_get_mount('E') && !strcmp(vfs_get_mount('E')->device, "nvme0"));
    assert(partitions_try_mount_root(NULL) != 0 && !vfs_get_mount('C'));
    part_tables[1] = &usb; part_tables_count = 2;
    assert(partitions_try_mount_root("usb0") == 0);
    assert(root_mounts == 1 && !strcmp(vfs_get_mount('C')->device, "usb0"));
    assert(vfs_get_mount('C')->start_lba == 8192);
    // Root already set: later NVMe scans must never replace it.
    assert(partitions_try_mount_root("nvme0") == 0 && root_mounts == 1);
    vfs_init(); // Both disks present at early boot: still select USB root.
    mount_all_partitions();
    assert(root_mounts == 1 && !strcmp(vfs_get_mount('C')->device, "usb0"));
    // A transient early mount error must not claim USB root as secondary.
    vfs_init(); fail_root_once = true;
    mount_all_partitions();
    assert(!vfs_get_mount('C') && root_mounts == 0);
    for (char c = 'E'; c <= 'Z'; c++)
        assert(!vfs_get_mount(c) || strcmp(vfs_get_mount(c)->device, "usb0") != 0);
    assert(partitions_try_mount_root("usb0") == 0);
    assert(root_mounts == 1 && !strcmp(vfs_get_mount('C')->device, "usb0"));
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    src = Path(directory) / 'root.c'
    binpath = Path(directory) / 'root'
    src.write_text(prefix + selector + early + late + suffix)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(binpath)], check=True)
    subprocess.run([str(binpath)], check=True)
print('Early and hotplug C: selection rejects NVMe FAT32/other ext4; accepts USB BOLDON_ROOT')
