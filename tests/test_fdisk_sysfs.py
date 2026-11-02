"""Verify the sysfs device-number data consumed by util-linux fdisk -l.

fdisk's next_proc_partition() reads /proc/partitions, looks up each name at
/sys/block/<name>/dev and resolves /sys/dev/block/<major>:<minor> before
opening /dev/<name>. An accessible /proc/partitions alone is not enough.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'kernel/src/disk/hotplug.c').read_text()
start = source.index('static int sysdev_read(')
end = source.index('static vfs_file_ops_t sysdev_ops', start)
assert 'vn->inode_num = 0x0800u + (uint32_t)slot;' in source
assert 'devfs_register_symlink(sys_path, n->sys_target);' in source
assert 'str_eq(name, alias_for(dev))' in source
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define MAX_BLOCK_NODES 8
typedef struct { int used; } block_node_t;
static block_node_t nodes[MAX_BLOCK_NODES];
typedef struct { void *fs_data; } vfs_node_t;
typedef struct { vfs_node_t *node; uint64_t offset; } vfs_file_t;
''' + source[start:end] + r'''
int main(void) {
    nodes[1].used = 1;
    vfs_node_t node = { .fs_data = &nodes[1] };
    vfs_file_t file = { .node = &node };
    char buf[32] = {0};
    assert(sysdev_read(&file, buf, 2) == 2);
    assert(memcmp(buf, "8:", 2) == 0);
    assert(sysdev_read(&file, buf + 2, sizeof(buf) - 2) == 2);
    assert(strcmp(buf, "8:1\n") == 0);
    assert(sysdev_read(&file, buf, sizeof(buf)) == 0);
    nodes[1].used = 0;
    assert(sysdev_read(&file, buf, sizeof(buf)) == -ENODEV);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    src = Path(directory) / 'sysfs.c'
    exe = Path(directory) / 'sysfs'
    src.write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('fdisk sysfs device numbers and partial reads passed')
