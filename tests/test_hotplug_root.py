"""Exercise the actual deferred /media mount helpers with mock partitions and VFS."""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
source = (root / 'kernel/src/disk/hotplug.c').read_text()
helpers = source[source.index('static bool partition_already_mounted('):source.index('static int device_add(', source.index('static bool partition_already_mounted('))]
known = source[source.index('static void auto_mount_known_partitions('):source.index('static void hotplug_scan(', source.index('static void auto_mount_known_partitions('))]

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
struct partition { uint64_t start_lba, sector_count; };
struct partition_table { unsigned count; struct partition parts[2]; };
typedef struct { bool dead; uint64_t start_lba; char device[32]; } vfs_mount_t;
static vfs_mount_t mounts[26];
static bool active[26];
static unsigned calls;
static char known[1][32] = {"usb0"};
static char disk_alias[1][32] = {"sda"};
static int known_count = 1;
static struct partition_table pt = {2, {{2048, 1000}, {4096, 2000}}};
static vfs_mount_t *vfs_get_mount(char letter) {
    return active[letter - 'A'] ? &mounts[letter - 'A'] : NULL;
}
static struct partition_table *get_partition_struct(const char *dev) {
    assert(strcmp(dev, "usb0") == 0); return &pt;
}
static const char *partitions_detect_fs(const char *dev, uint64_t lba) {
    assert(strcmp(dev, "usb0") == 0 && (lba == 2048 || lba == 4096));
    return lba == 4096 ? "ext4" : "fat32";
}
static const char *strip_dev(const char *path) { return path + 5; }
static int vfs_mount(const char *src, const char *target, const char *fs,
                     uint32_t flags, const void *data) {
    (void)flags; (void)data;
    assert(strcmp(src, "/dev/sda1") == 0);
    assert(strcmp(target, "/media/sda1") == 0);
    assert(strcmp(fs, "fat32") == 0);
    calls++;
    active['E' - 'A'] = true;
    mounts['E' - 'A'] = (vfs_mount_t){.start_lba = 2048};
    strcpy(mounts['E' - 'A'].device, "usb0");
    return 0;
}
static void console_print_string(const char *s) { (void)s; }
'''
suffix = r'''
int main(void) {
    auto_mount_known_partitions();
    assert(calls == 0); // C: absent: reserve all candidates for the root probe
    active['C' - 'A'] = true;
    mounts['C' - 'A'] = (vfs_mount_t){.start_lba = 4096};
    strcpy(mounts['C' - 'A'].device, "usb0");
    auto_mount_known_partitions();
    assert(calls == 1 && partition_already_mounted("usb0", 4096));
    auto_mount_known_partitions();
    assert(calls == 1); // neither C: nor /media gets mounted a second time
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    test = pathlib.Path(directory) / 'hotplug.c'
    binary = pathlib.Path(directory) / 'hotplug'
    test.write_text(prefix + helpers + known + suffix)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(test), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('Hotplug defers /media until C: exists and skips mounted partitions')
