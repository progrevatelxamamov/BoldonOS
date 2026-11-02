"""Exercise the actual VFS and devfs path/listing code for /proc."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
vfs = (root / 'kernel/src/fs/vfs.c').read_text()
start = vfs.index('static void normalize_path_raw(', vfs.index('static bool letter_is_root(char c) {'))
end = vfs.index('vfs_node_t *vfs_resolve_path_ext(', start)
normalization = vfs[start:end]
devfs = (root / 'kernel/src/fs/devfs.c').read_text()
start = devfs.index('static const char* clean_name(')
end = devfs.index('static vfs_node_ops_t devfs_dir_ops', start)
lookup = devfs[start:end]

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { char cwd[256]; } process_t;
static process_t proc;
static process_t *get_current_process(void) { return &proc; }
static char boot_drive_letter = 'C';
static void *malloc_alloc(size_t n) { return calloc(1, n); }
static void malloc_free(void *p) { free(p); }
static char vfs_mountpoint_lookup(const char *p, uint32_t *len) {
    (void)p; *len = 0; return 0;
}
#define MAX_DEVFS_NODES 8
typedef enum { VFS_TYPE_DIR, VFS_TYPE_FILE, VFS_TYPE_CHAR_DEV } vfs_node_type_t;
typedef struct vfs_node vfs_node_t;
typedef struct { int unused; } vfs_node_ops_t;
typedef struct { int unused; } vfs_file_ops_t;
struct vfs_node {
    char name[256]; vfs_node_type_t type; int mode;
    vfs_node_ops_t *ops; vfs_file_ops_t *file_ops;
    void *fs_data; unsigned long inode_num;
};
typedef struct { vfs_node_t *node; uint64_t offset; } vfs_file_t;
typedef struct { char name[256]; uint64_t inode_num, size; vfs_node_type_t type; } vfs_dirent_t;
typedef struct { char name[64]; vfs_node_t node; bool used; } devfs_node_entry_t;
static devfs_node_entry_t dev_nodes[MAX_DEVFS_NODES];
'''
suffix = r'''
static void check(const char *path, const char *expected) {
    char *norm = normalize_path(path);
    assert(norm && strcmp(norm, expected) == 0);
    malloc_free(norm);
}
int main(void) {
    strcpy(proc.cwd, "/");
    check("/proc", "D:/proc");
    check("/proc/partitions", "D:/proc/partitions");
    check("/proc/mounts", "D:/proc/mounts");
    check("/proc/self/mountinfo", "D:/proc/mountinfo");
    check("/dev", "D:/");
    check("/dev/null", "D:/null");
    check("proc", "D:/proc");
    check("proc/partitions", "D:/proc/partitions");
    check("dev/null", "D:/null");
    check("C:/proc", "C:/proc");
    strcpy(proc.cwd, "/proc");
    check("partitions", "D:/proc/partitions");
    strcpy(proc.cwd, "/dev");
    check("null", "D:/null");
    strcpy(proc.cwd, "/etc");
    check("hosts", "C:/etc/hosts");

    vfs_node_t root = { .type = VFS_TYPE_DIR, .fs_data = "" };
    dev_nodes[0].used = true;
    strcpy(dev_nodes[0].name, "proc/partitions");
    dev_nodes[0].node.type = VFS_TYPE_FILE;
    dev_nodes[1].used = true;
    strcpy(dev_nodes[1].name, "proc/mounts");
    dev_nodes[1].node.type = VFS_TYPE_FILE;
    dev_nodes[2].used = true;
    strcpy(dev_nodes[2].name, "null");
    dev_nodes[2].node.type = VFS_TYPE_CHAR_DEV;
    vfs_node_t *dir = devfs_vfs_lookup(&root, "proc");
    assert(dir && dir->type == VFS_TYPE_DIR);
    assert(devfs_vfs_lookup(dir, "partitions") == &dev_nodes[0].node);
    assert(devfs_vfs_lookup(dir, "null") == NULL);
    vfs_file_t file = { .node = dir };
    vfs_dirent_t ent = {0};
    assert(devfs_vfs_readdir(&file, &ent) == 0);
    assert(strcmp(ent.name, "partitions") == 0 && ent.type == VFS_TYPE_FILE);
    assert(devfs_vfs_readdir(&file, &ent) == 0);
    assert(strcmp(ent.name, "mounts") == 0);
    assert(devfs_vfs_readdir(&file, &ent) < 0);
    file.node = &root; file.offset = 0;
    assert(devfs_vfs_readdir(&file, &ent) == 0);
    assert(strcmp(ent.name, "proc") == 0);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    src = Path(directory) / 'proc.c'
    binary = Path(directory) / 'proc'
    src.write_text(prefix + normalization + lookup + suffix)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('Absolute/relative /proc routing and virtual directory listing passed')
