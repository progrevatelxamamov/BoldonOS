"""Compile the production ext4 VFS open adapter against a mode-sensitive backend."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
src=(root/'kernel/src/fs/ext4_vfs.c').read_text()
fragment=src[src.index('static ext4_vfs_handle_t *ext4_ensure_handle('):src.index('static int ext4_vfs_op_read(', src.index('static ext4_vfs_handle_t *ext4_ensure_handle('))]
create=src[src.index('static int ext4_vfs_op_create('):src.index('static int ext4_vfs_op_mkdir(', src.index('static int ext4_vfs_op_create('))]
assert 'ext4_fopen(&f, target_path, "a+")' in create
assert 'ext4_fopen(&f, target_path, "w+")' not in create
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define VFS_O_READ 1
#define VFS_O_WRITE 2
#define VFS_O_CREAT 4
#define VFS_O_TRUNC 8
#define VFS_O_APPEND 16
#define VFS_TYPE_DIR 2
#define VFS_TYPE_FILE 1
#define EOK 0
typedef struct { int type; } vfs_node_t;
typedef struct { vfs_node_t *node; uint32_t flags; void *private_data; } vfs_file_t;
typedef struct { int value; } ext4_file;
typedef struct { int value; } ext4_dir;
typedef struct { ext4_file file; bool is_dir; ext4_dir dir; } ext4_vfs_handle_t;
static char last_mode[8]; static int opens, failures;
static void *malloc_alloc(size_t n) { return malloc(n); }
static void malloc_free(void *p) { free(p); }
static void get_lwext4_path(vfs_node_t *node, char *path, size_t len) {
    (void)node; assert(len>=5); strcpy(path,"/app");
}
static int ext4_dir_open(ext4_dir *dir, const char *path) { (void)dir; (void)path; return EOK; }
static int ext4_fopen(ext4_file *file, const char *path, const char *mode) {
    (void)file; (void)path; opens++; strcpy(last_mode,mode);
    return failures ? EACCES : EOK;
}
''' + fragment + r'''
int main(void) {
    vfs_node_t n={VFS_TYPE_FILE};
    vfs_file_t file={&n,VFS_O_WRITE|VFS_O_CREAT,0};
    assert(ext4_vfs_op_open(&file)==0 && !strcmp(last_mode,"r+"));
    assert(ext4_ensure_handle(&file)==file.private_data && opens==1);
    free(file.private_data); file.private_data=NULL;
    file.flags=VFS_O_WRITE|VFS_O_TRUNC;
    assert(ext4_vfs_op_open(&file)==0 && !strcmp(last_mode,"w+"));
    free(file.private_data); file.private_data=NULL;
    file.flags=VFS_O_READ|VFS_O_CREAT;
    assert(ext4_vfs_op_open(&file)==0 && !strcmp(last_mode,"r"));
    free(file.private_data); file.private_data=NULL;
    file.flags=VFS_O_WRITE|VFS_O_APPEND;
    assert(ext4_vfs_op_open(&file)==0 && !strcmp(last_mode,"a+"));
    free(file.private_data); file.private_data=NULL;
    file.flags=VFS_O_READ|VFS_O_APPEND;
    assert(ext4_vfs_op_open(&file)==0 && !strcmp(last_mode,"r"));
    free(file.private_data); file.private_data=NULL;
    failures=1; file.flags=VFS_O_WRITE;
    int before=opens;
    assert(ext4_vfs_op_open(&file)==-EIO && !file.private_data && opens==before+1);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c=Path(tmp)/'test.c';exe=Path(tmp)/'test';c.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('ext4 O_CREAT preserves existing data; only O_TRUNC selects w+; failed write open does not fall back')
