"""Regression for the exact vfs_write fault at node->file_ops after capability I/O."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / "kernel/src/fs/vfs.c").read_text()
body = source[source.index("int vfs_write(vfs_file_t *"):source.index("int vfs_readdir(", source.index("int vfs_write(vfs_file_t *"))]

code = r'''
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#define VFS_O_WRITE 2
#define IN_MODIFY 2
typedef struct vfs_node vfs_node_t;
typedef struct vfs_file vfs_file_t;
typedef struct { int (*write)(vfs_file_t *, const void *, uint64_t); } vfs_file_ops_t;
struct vfs_node { vfs_file_ops_t *file_ops; };
struct vfs_file { vfs_node_t *node; unsigned flags; };
static int drop_mode, writes, events;
static vfs_file_t *active;
static vfs_node_t other_node;
static int vfs_drop_file_caps(vfs_node_t *node) {
    (void)node;
    if (drop_mode == 1) active->node = NULL;
    if (drop_mode == 2) active->node = &other_node;
    if (drop_mode == 3) return -EIO;
    return 0;
}
static void inotify_vfs_event(vfs_node_t *node, int action, void *unused) {
    (void)node; (void)action; (void)unused; events++;
}
static int write_op(vfs_file_t *f, const void *buf, uint64_t count) {
    (void)f; (void)buf; writes++; return (int)count;
}
''' + body + r'''
int main(void) {
    vfs_file_ops_t ops = { .write = write_op };
    vfs_node_t node = { .file_ops = &ops };
    vfs_file_t f = { .node = &node, .flags = VFS_O_WRITE };
    active = &f;
    if (vfs_write(&f, "a", 1) != 1 || writes != 1 || events != 1) return 1;
    drop_mode = 1;
    if (vfs_write(&f, "a", 1) != -EBADF || writes != 1) return 2;
    f.node = &node;
    drop_mode = 2;
    if (vfs_write(&f, "a", 1) != -EBADF || writes != 1) return 3;
    f.node = &node;
    drop_mode = 3;
    if (vfs_write(&f, "a", 1) != -EIO || writes != 1) return 4;
    return 0;
}
'''

with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / "probe.c"
    exe = Path(tmp) / "probe"
    c.write_text(code)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("vfs_write rejects a node lost during capability I/O")
