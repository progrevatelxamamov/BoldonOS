"""Relative cd proc must keep a logical absolute cwd for /proc routing."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'kernel/src/syscall/sys_proc.c').read_text()
function = source[source.index('uint64_t sys_chdir_impl('):source.index('uint64_t sys_execve_impl(', source.index('uint64_t sys_chdir_impl('))]
code = r'''
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#define VFS_TYPE_DIR 1
typedef struct { int type; } vfs_node_t;
typedef struct { char cwd[256]; } process_t;
static process_t current;
static vfs_node_t dir = {VFS_TYPE_DIR};
static process_t *get_current_process(void) { return &current; }
static vfs_node_t *vfs_resolve_path(const char *s) { (void)s; return &dir; }
''' + function + r'''
int main(void) {
    strcpy(current.cwd, "/");
    assert(sys_chdir_impl((uint64_t)"proc") == 0);
    assert(strcmp(current.cwd, "/proc") == 0);
    assert(sys_chdir_impl((uint64_t)"mounts") == 0);
    assert(strcmp(current.cwd, "/proc/mounts") == 0);
    assert(sys_chdir_impl((uint64_t)"/dev") == 0);
    assert(strcmp(current.cwd, "/dev") == 0);
    assert(sys_chdir_impl((uint64_t)"C:/etc") == 0);
    assert(strcmp(current.cwd, "C:/etc") == 0);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    src = Path(directory) / 'chdir.c'
    exe = Path(directory) / 'chdir'
    src.write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('Relative chdir keeps absolute logical cwd passed')
