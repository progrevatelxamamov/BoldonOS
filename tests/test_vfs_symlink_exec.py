"""Run the production VFS path resolver on Arch-style /bin and ELF interpreter links."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
vfs = (root / 'kernel/src/fs/vfs.c').read_text()
resolver = vfs[vfs.index('static vfs_node_t *vfs_resolve_path_options('):
               vfs.index('int vfs_check_access(', vfs.index('static vfs_node_t *vfs_resolve_path_options('))]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define MAX_SYMLINK_DEPTH 8
#define VFS_PERM_EXEC 1
typedef enum { VFS_TYPE_DIR, VFS_TYPE_FILE, VFS_TYPE_SYMLINK } vfs_node_type_t;
typedef struct vfs_node vfs_node_t;
typedef struct {
    vfs_node_t *(*lookup)(vfs_node_t *, const char *);
    int (*readlink)(vfs_node_t *, char *, uint64_t);
} vfs_node_ops_t;
struct vfs_node {
    vfs_node_type_t type;
    vfs_node_ops_t *ops;
    const char *path, *target;
};
typedef struct { vfs_node_t *root; } vfs_mount_t;
static vfs_node_t nodes[32];
static unsigned count;
static vfs_mount_t cmount, dmount;
static vfs_mount_t *drives[26];
typedef struct { bool is_kernel_process; uint32_t fsuid, fsgid; } process_t;
static process_t caller;
static bool deny_usr;
static process_t *get_current_process(void) { return &caller; }
static int vfs_check_access(vfs_node_t *node, uint32_t uid, uint32_t gid, int perms) {
    (void)uid; (void)gid; (void)perms;
    return deny_usr && !strcmp(node->path, "C:/usr") ? -1 : 0;
}
static void malloc_free(void *p) { free(p); }
static vfs_node_t *lookup(vfs_node_t *dir, const char *name) {
    char path[512];
    size_t n = strlen(dir->path);
    assert(n + strlen(name) + 2 < sizeof(path));
    strcpy(path, dir->path);
    if (n && path[n-1] != '/') strcat(path, "/");
    strcat(path, name);
    for (unsigned i = 0; i < count; i++)
        if (!strcmp(nodes[i].path, path)) return &nodes[i];
    return NULL;
}
static int readlink_node(vfs_node_t *node, char *buf, uint64_t cap) {
    size_t n = strlen(node->target);
    if (n > cap) return -1;
    memcpy(buf, node->target, n);
    return (int)n;
}
static vfs_node_ops_t ops = { lookup, readlink_node };
static vfs_node_t *add(const char *path, vfs_node_type_t type, const char *target) {
    assert(count < sizeof(nodes)/sizeof(nodes[0]));
    nodes[count] = (vfs_node_t){ .type=type, .ops=&ops, .path=path, .target=target };
    return &nodes[count++];
}
static char *normalize_path(const char *raw) {
    char *norm = malloc(512);
    assert(norm);
    const char *drive = !strncmp(raw,"/dev",4) && (raw[4]=='/' || !raw[4]) ? "D:" : "C:";
    if (raw[0] == '/') {
        strcpy(norm, drive);
        strcat(norm, drive[0]=='D' ? raw+4 : raw);
    } else strcpy(norm, raw);
    return norm;
}
''' + resolver + r'''
int main(void) {
    cmount.root = add("C:", VFS_TYPE_DIR, NULL);
    dmount.root = add("D:", VFS_TYPE_DIR, NULL);
    drives['C'-'A'] = &cmount; drives['D'-'A'] = &dmount;
    add("C:/bin", VFS_TYPE_SYMLINK, "usr/bin");
    add("C:/usr", VFS_TYPE_DIR, NULL);
    add("C:/usr/bin", VFS_TYPE_DIR, NULL);
    vfs_node_t *shlink = add("C:/usr/bin/sh", VFS_TYPE_SYMLINK, "bash");
    vfs_node_t *bash = add("C:/usr/bin/bash", VFS_TYPE_FILE, NULL);
    add("C:/lib64", VFS_TYPE_SYMLINK, "usr/lib");
    add("C:/usr/lib", VFS_TYPE_DIR, NULL);
    vfs_node_t *ld = add("C:/usr/lib/ld-linux-x86-64.so.2", VFS_TYPE_FILE, NULL);
    add("C:/usr/lib/ld.so", VFS_TYPE_SYMLINK, "/lib64/ld-linux-x86-64.so.2");
    add("C:/etc", VFS_TYPE_DIR, NULL);
    add("C:/etc/shell", VFS_TYPE_SYMLINK, "../usr/bin/bash");
    add("C:/loop-a", VFS_TYPE_SYMLINK, "/loop-b");
    add("C:/loop-b", VFS_TYPE_SYMLINK, "/loop-a");
    vfs_node_t *devnull = add("D:/null", VFS_TYPE_FILE, NULL);
    add("C:/devlink", VFS_TYPE_SYMLINK, "/dev/null");
    assert(vfs_resolve_path("/bin/sh") == bash);
    assert(vfs_resolve_path_nofollow("/bin/sh") == shlink);
    assert(vfs_resolve_path_nofollow("/usr/bin/bash") == bash);
    assert(vfs_resolve_path("C:/bin/sh") == bash);
    assert(vfs_resolve_path("/etc/shell") == bash);
    assert(vfs_resolve_path("/bin/../../lib64/ld-linux-x86-64.so.2") == ld);
    assert(vfs_resolve_path("/usr/lib/ld.so") == ld);
    assert(vfs_resolve_path("/devlink") == devnull);
    assert(vfs_resolve_path("/loop-a") == NULL);
    assert(vfs_resolve_path("/missing") == NULL);
    char name[256] = {0};
    assert(vfs_resolve_path_ext("/bin/new", true, name) == &nodes[4]);
    assert(!strcmp(name, "new"));
    deny_usr = true;
    assert(vfs_resolve_path("/usr/bin/bash") == NULL);
    assert(vfs_resolve_path_ext("/usr/bin/new", true, name) == NULL);
    caller.is_kernel_process = true;
    assert(vfs_resolve_path("/usr/bin/bash") == bash);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    src = Path(directory) / 'test.c'
    exe = Path(directory) / 'test'
    src.write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('Arch /bin/sh, ELF interpreter, relative/absolute symlinks and loops passed')
