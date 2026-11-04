"""Exercise the production VFS file-capability removal before file writes."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
src=(root/'kernel/src/fs/vfs.c').read_text()
drop=src[src.index('static int vfs_drop_file_caps('):src.index('void vfs_init(',src.index('static int vfs_drop_file_caps('))]
write=src[src.index('int vfs_write('):src.index('int vfs_readdir(',src.index('int vfs_write('))]
opening=src[src.index('vfs_file_t *vfs_open('):src.index('int vfs_read(',src.index('vfs_file_t *vfs_open('))]
assert opening.index('vfs_drop_file_caps(node)') < opening.index('file_ops->open(file)')
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#define VFS_TYPE_FILE 1
#define VFS_O_WRITE 2
#define IN_ATTRIB 4
#define IN_MODIFY 2
typedef struct vfs_node vfs_node_t;
typedef struct vfs_file vfs_file_t;
typedef struct { int (*getxattr)(vfs_node_t *,const char *,void *,uint64_t);
                 int (*removexattr)(vfs_node_t *,const char *); } vfs_node_ops_t;
typedef struct { int (*write)(vfs_file_t *,const void *,uint64_t); } vfs_file_ops_t;
struct vfs_node { int type; vfs_node_ops_t *ops; vfs_file_ops_t *file_ops; };
struct vfs_file { vfs_node_t *node; uint32_t flags; };
static int present=1, failure=0, writes=0, removed=0, attrs=0, modifies=0;
static int get_attr(vfs_node_t *node,const char *name,void *value,uint64_t len) {
    (void)node; assert(!strcmp(name,"security.capability") && len==24);
    if (failure) return -EIO;
    if (!present) return -ENODATA;
    memset(value,0,20); return 20;
}
static int remove_attr(vfs_node_t *node,const char *name) {
    (void)node; assert(!strcmp(name,"security.capability"));
    present=0; removed++; return 0;
}
static int backend_write(vfs_file_t *file,const void *buf,uint64_t len) {
    (void)file; (void)buf; writes++; return (int)len;
}
static void inotify_vfs_event(vfs_node_t *node,uint32_t mask,const char *name) {
    (void)node; (void)name; if (mask==IN_ATTRIB) attrs++; if (mask==IN_MODIFY) modifies++;
}
''' + drop + write + r'''
int main(void) {
    vfs_node_ops_t ops={get_attr,remove_attr};
    vfs_file_ops_t fops={backend_write};
    vfs_node_t node={VFS_TYPE_FILE,&ops,&fops};
    vfs_file_t file={&node,VFS_O_WRITE};
    assert(vfs_write(&file,"a",1)==1 && removed==1 && writes==1 && attrs==1 && modifies==1);
    assert(vfs_write(&file,"b",1)==1 && removed==1 && writes==2);
    present=1;failure=1;
    assert(vfs_write(&file,"c",1)==-EIO && writes==2 && removed==1);
    assert(vfs_write(&file,"",0)==0 && writes==3 && removed==1);
    file.flags=0;failure=0;
    assert(vfs_write(&file,"d",1)==-EBADF && writes==3 && removed==1);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c=Path(tmp)/'test.c';exe=Path(tmp)/'test';c.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('File capabilities dropped on VFS write and errors stop the mutation')
