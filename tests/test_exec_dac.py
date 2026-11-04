"""Exercise the real exec/interpreter permission helper on the host."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'kernel/src/sys/elf.c').read_text()
helper = source[source.index('static int exec_file_access('):source.index('static int load_interp(', source.index('static int exec_file_access('))]
assert 'if (exec_file_access(ifile))' in source
assert 'if (exec_file_access(f))' in source
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#define VFS_TYPE_FILE 1
#define VFS_TYPE_DIR 2
#define VFS_PERM_EXEC 1
typedef struct { int type; uint32_t mode; } vfs_node_t;
typedef struct { vfs_node_t *node; } vfs_file_t;
typedef struct { bool is_kernel_process; uint32_t fsuid, fsgid; } process_t;
static process_t proc = { .fsuid=1000, .fsgid=1000 };
static process_t *get_current_process(void) { return &proc; }
static int vfs_check_access(vfs_node_t *node, uint32_t uid, uint32_t gid, int perms) {
    assert(uid == 1000 && gid == 1000 && perms == VFS_PERM_EXEC);
    return (node->mode & 0111) ? 0 : -1;
}
''' + helper + r'''
int main(void) {
    vfs_node_t node={VFS_TYPE_FILE,0644};
    vfs_file_t file={&node};
    assert(exec_file_access(NULL)==-EACCES);
    assert(exec_file_access(&file)==-EACCES);
    node.mode=0755;
    assert(exec_file_access(&file)==0);
    node.type=VFS_TYPE_DIR;
    assert(exec_file_access(&file)==-EACCES);
    node.type=VFS_TYPE_FILE; node.mode=0644;
    proc.is_kernel_process=true;
    assert(exec_file_access(&file)==0);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src=Path(tmp)/'test.c'; binary=Path(tmp)/'test'
    src.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(src),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('ELF/script and interpreter exec DAC helper PASS')
