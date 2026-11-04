"""Reject unsupported relative dirfd instead of silently opening in cwd."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
src=(root/'kernel/src/syscall/sys_file.c').read_text()
fragment=src[src.index('static inline uint32_t translate_linux_open_flags('):src.index('uint64_t sys_close_impl(')]
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#define VFS_O_READ 1
#define VFS_O_WRITE 2
#define VFS_O_CREAT 4
#define VFS_O_TRUNC 8
#define VFS_O_APPEND 16
#define VFS_O_NONBLOCK 32
#define VFS_O_CLOEXEC 64
typedef struct { int unused; } process_fd_table_t;
typedef struct { process_fd_table_t *fd_table; } process_t;
static process_fd_table_t table;
static process_t proc = {&table};
static process_t *get_current_process(void) { return &proc; }
static int copy_str_from_user(char *dst, uint64_t src, size_t cap) {
    if (!src || strlen((const char *)src) >= cap) return -EFAULT;
    strcpy(dst,(const char *)src); return 0;
}
static int opens; static char last[512]; static unsigned lastflags;
static int sys_open(process_fd_table_t *fds, const char *path, uint32_t flags) {
    assert(fds==&table); opens++; strcpy(last,path); lastflags=flags; return 3;
}
''' + fragment + r'''
int main(void) {
    assert((int64_t)sys_openat_impl(4,(uint64_t)"relative",0)==-ENOSYS && opens==0);
    assert(sys_openat_impl((uint64_t)-100,(uint64_t)"relative",1)==3);
    assert(!strcmp(last,"relative") && lastflags==VFS_O_WRITE);
    assert(sys_openat_impl((uint32_t)-100,(uint64_t)"relative",0)==3);
    assert(sys_openat_impl(4,(uint64_t)"/absolute",0)==3);
    assert(!strcmp(last,"/absolute") && lastflags==VFS_O_READ);
    assert((int64_t)sys_openat_impl(4,0,0)==-EFAULT);
    assert(sys_open_impl((uint64_t)"/legacy",0,0)==3);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c=Path(tmp)/'test.c'; exe=Path(tmp)/'test'; c.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('openat unsupported relative dirfd and legacy open path checks PASS')
