"""Exercise production permission helpers guarding file mutations."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
sys = (root/'kernel/src/syscall/sys_file.c').read_text()
section = sys[sys.index('static int mutation_parent_access('):sys.index('uint64_t sys_mkdirat_impl(', sys.index('static int mutation_parent_access('))]
for syscall, guard in [('sys_mkdirat_impl','mutation_parent_access(kpath)'),
                       ('sys_renameat_impl','mutation_parent_access(oldpath)'),
                       ('sys_unlinkat_impl','mutation_parent_access(kpath)'),
                       ('sys_fchmodat_impl','mutation_owner_access(vfs_resolve_path(kpath))'),
                       ('sys_fchmod_impl','mutation_owner_access(file->node)'),
                       ('sys_link_impl','mutation_parent_access(knew)')]:
    body=sys[sys.index('uint64_t '+syscall+'('):]
    body=body[:body.index('\nuint64_t ',1)] if '\nuint64_t ' in body[1:] else body
    assert guard in body, (syscall,guard)
for name in ('BOLDON_CAP_SYS_ADMIN','BOLDON_CAP_SYS_BOOT'):
    assert name in sys
code=r'''
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <assert.h>
#define VFS_TYPE_DIR 2
#define VFS_PERM_WRITE 2
#define VFS_PERM_EXEC 1
#define BOLDON_CAP_FOWNER (1ULL<<3)
typedef struct { uint32_t uid,gid; } process_fd_table_t;
typedef struct { uint32_t fsuid,fsgid; uint64_t cap_effective; process_fd_table_t *fd_table; } process_t;
typedef struct { uint32_t uid,gid; int type; } vfs_node_t;
static process_fd_table_t table;
static process_t proc={ .fsuid=1000, .fsgid=1000, .fd_table=&table };
static vfs_node_t parent={ .uid=2000, .type=VFS_TYPE_DIR };
static vfs_node_t node={ .uid=2000 };
static process_t *get_current_process(void) { return &proc; }
static vfs_node_t *vfs_resolve_path_ext(const char *path, bool par, char *last) {
    (void)path;(void)par;(void)last;return &parent;
}
static int permitted;
static int vfs_check_access(vfs_node_t *n, uint32_t uid, uint32_t gid, int perms) {
    assert(n==&parent && uid==proc.fsuid && gid==proc.fsgid);
    assert(perms == (VFS_PERM_WRITE | VFS_PERM_EXEC));return permitted?0:-1;
}
''' + section + r'''
int main(void) {
    assert(mutation_parent_access("/test") == -EACCES);
    permitted=1;
    assert(mutation_parent_access("/test") == 0);
    assert(mutation_owner_access(&node) == -EPERM);
    proc.fsuid = 2000;
    assert(mutation_owner_access(&node) == 0);
    proc.fsuid = 1000;
    proc.cap_effective = BOLDON_CAP_FOWNER;
    assert(mutation_owner_access(&node) == 0);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src=Path(tmp)/'test.c'; exe=Path(tmp)/'test'; src.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('User VFS mutations: parent write/execute and owner/FOWNER checks passed')
