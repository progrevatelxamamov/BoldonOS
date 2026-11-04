"""Host regression for VFS rename dispatch and inotify success-only notifications."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
vfs = (root / 'kernel/src/fs/vfs.c').read_text()
rename = vfs[vfs.index('int vfs_rename('):vfs.index('int vfs_fchmod(', vfs.index('int vfs_rename('))]
backend = (root / 'kernel/src/fs/ext4_vfs.c').read_text()
assert 'ext4_frename(from, to)' in backend
handler = (root / 'kernel/src/syscall/syscall.c').read_text()
assert 'case SYS_RENAME:' in handler and 'case SYS_RENAMEAT:' in handler
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define VFS_TYPE_DIR 2
#define VFS_TYPE_FILE 1
#define IN_DELETE_SELF 0x400
typedef struct vfs_node vfs_node_t;
typedef struct vfs_mount { int id; } vfs_mount_t;
typedef struct { int (*rename)(vfs_node_t *,const char *,vfs_node_t *,const char *); } vfs_node_ops_t;
struct vfs_node { int type; vfs_mount_t *mount; vfs_node_ops_t *ops; uint64_t inode_num; };
static vfs_mount_t mount1={1}, mount2={2};
static vfs_node_t parent={VFS_TYPE_DIR,&mount1,NULL,1}, other={VFS_TYPE_DIR,&mount2,NULL,2};
static vfs_node_t item={VFS_TYPE_FILE,&mount1,NULL,42};
static vfs_node_t target={VFS_TYPE_FILE,&mount1,NULL,99};
static int backend_result, calls, notifications, deleted, exists;
static int rename_backend(vfs_node_t *a,const char *b,vfs_node_t *c,const char *d) {
    (void)a;(void)b;(void)c;(void)d;calls++;return backend_result;
}
static vfs_node_ops_t ops={rename_backend};
static vfs_node_t *vfs_resolve_path_nofollow(const char *path) {
    if (!strcmp(path,"old")) return &item;
    if (!strcmp(path,"new") && exists) return &target;
    return NULL;
}
static vfs_node_t *vfs_resolve_path_ext(const char *path, int parent_only, char *name) {
    (void)parent_only;
    if (!strcmp(path,"old")) { strcpy(name,"old");return &parent; }
    if (!strcmp(path,"new")) { strcpy(name,"new");return &parent; }
    if (!strcmp(path,"other")) { strcpy(name,"other");return &other; }
    return NULL;
}
static void inotify_vfs_move(vfs_node_t *a,const char *b,vfs_node_t *c,const char *d,vfs_node_t *e) {
    (void)a;(void)b;(void)c;(void)d;(void)e;notifications++;
}
static void inotify_vfs_event(vfs_node_t *n, unsigned mask, const char *name) {
    assert(n == &target && mask == IN_DELETE_SELF && !name); deleted++;
}
''' + rename + r'''
int main(void) {
    parent.ops=&ops;
    assert(vfs_rename("old","old")==0 && !calls);
    assert(vfs_rename("missing","new")==-ENOENT);
    assert(vfs_rename("old","other")==-EXDEV);
    exists=1;
    target.inode_num = item.inode_num;
    assert(vfs_rename("old","new")==0 && !calls && !notifications);
    target.inode_num = 99;
    backend_result=-EIO;
    assert(vfs_rename("old","new")==-EIO && calls==1 && !notifications && !deleted);
    backend_result=0;
    assert(vfs_rename("old","new")==0 && calls==2 && notifications==1 && deleted==1);
    exists=0;
    assert(vfs_rename("old","new")==0 && calls==3 && notifications==2 && deleted==1);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp)/'test.c'; exe=Path(tmp)/'test'
    src.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('VFS rename: no-op, cross-device, replacement and success-only events passed')
