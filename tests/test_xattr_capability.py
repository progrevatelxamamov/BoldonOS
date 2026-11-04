"""Exercise the production guest xattr dispatcher with a host VFS mock."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
sys_file = (root / 'kernel/src/syscall/sys_file.c').read_text()
body = sys_file[sys_file.index('uint64_t sys_xattr_impl('):]
code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#define VFS_MAX_FD 256
#define VFS_PERM_READ 4
#define VFS_PERM_WRITE 2
#define VFS_TYPE_FILE 1
#define BOLDON_CAP_SETFCAP (1ULL << 31)
typedef struct vfs_node vfs_node_t;
typedef struct { int (*getxattr)(vfs_node_t *, const char *, void *, uint64_t); } vfs_node_ops_t;
struct vfs_node { vfs_node_ops_t *ops; int type; };
typedef struct { vfs_node_t *node; } vfs_file_t;
typedef struct { vfs_file_t *fds[VFS_MAX_FD]; } process_fd_table_t;
typedef struct { process_fd_table_t *fd_table; uint32_t fsuid, fsgid; uint64_t cap_effective; } process_t;
typedef struct { bool present, effective; uint64_t permitted, inheritable; } boldon_file_caps_t;
static process_fd_table_t table;
static process_t proc = { .fd_table = &table };
static uint8_t stored[24];
static int stored_len = -ENODATA, writes, removes;
static int deny_write;
static int get_attr(vfs_node_t *n, const char *name, void *out, uint64_t size) {
    (void)n; assert(!strcmp(name, "security.capability"));
    if (stored_len < 0) return stored_len;
    if ((int)size < stored_len) return -ERANGE;
    memcpy(out, stored, stored_len); return stored_len;
}
static vfs_node_ops_t ops = { get_attr };
static vfs_node_t node = { &ops, VFS_TYPE_FILE };
static vfs_file_t file = { &node };
static process_t *get_current_process(void) { return &proc; }
static vfs_node_t *vfs_resolve_path(const char *p) { return !strcmp(p, "/bin/app") ? &node : NULL; }
static vfs_node_t *vfs_resolve_path_nofollow(const char *p) { return vfs_resolve_path(p); }
static int user_range_ok(uint64_t p, uint64_t n) { return p && n < 4096; }
static int copy_str_from_user(char *out, uint64_t in, size_t size) {
    if (!in || strlen((const char *)in) >= size) return -EFAULT;
    strcpy(out, (const char *)in); return 0;
}
static int vfs_check_access(vfs_node_t *n, uint32_t uid, uint32_t gid, int permission) {
    (void)n; (void)uid; (void)gid;
    return permission == VFS_PERM_WRITE && deny_write ? -EACCES : 0;
}
static int capability_parse_file(const void *raw, int len, boldon_file_caps_t *cap) {
    uint32_t magic;
    if (len != 20) return -EINVAL;
    memcpy(&magic, raw, 4);
    if (magic != 0x02000001U) return -EINVAL;
    cap->present = true; return 0;
}
static int vfs_setxattr(vfs_node_t *n, const char *name, const void *value, uint64_t size) {
    (void)n; (void)name; writes++; stored_len = (int)size;
    memcpy(stored, value, size); return 0;
}
static int vfs_removexattr(vfs_node_t *n, const char *name) {
    (void)n; (void)name; removes++; stored_len = -ENODATA; return 0;
}
''' + body + r'''
int main(void) {
    table.fds[3] = &file;
    const char *path = "/bin/app", *name = "security.capability";
    uint8_t value[24] = {0}, result[24] = {0};
    uint32_t magic = 0x02000001U;
    memcpy(value, &magic, 4);
    assert((int64_t)sys_xattr_impl(188,(uint64_t)path,(uint64_t)name,(uint64_t)value,20,0)==-EPERM);
    proc.cap_effective = BOLDON_CAP_SETFCAP;
    assert((int64_t)sys_xattr_impl(188,(uint64_t)path,(uint64_t)name,(uint64_t)value,20,1)==-EOPNOTSUPP);
    assert((int64_t)sys_xattr_impl(188,(uint64_t)path,(uint64_t)name,(uint64_t)value,19,0)==-EINVAL);
    deny_write=1;
    assert((int64_t)sys_xattr_impl(188,(uint64_t)path,(uint64_t)name,(uint64_t)value,20,0)==-EACCES);
    deny_write=0;
    assert(sys_xattr_impl(188,(uint64_t)path,(uint64_t)name,(uint64_t)value,20,0)==0 && writes==1);
    assert(sys_xattr_impl(193,3,(uint64_t)name,0,0,0)==20);
    assert((int64_t)sys_xattr_impl(191,(uint64_t)path,(uint64_t)name,(uint64_t)result,1,0)==-ERANGE);
    assert(sys_xattr_impl(191,(uint64_t)path,(uint64_t)name,(uint64_t)result,20,0)==20);
    assert(!memcmp(value,result,20));
    char names[32]={0};
    assert(sys_xattr_impl(194,(uint64_t)path,(uint64_t)names,sizeof(names),0,0)==strlen(name)+1);
    assert(!strcmp(names,name));
    assert(sys_xattr_impl(197,(uint64_t)path,(uint64_t)name,0,0,0)==0 && removes==1);
    assert(sys_xattr_impl(194,(uint64_t)path,0,0,0,0)==0);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / 'test.c'
    exe = Path(tmp) / 'test'
    src.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('File-capability xattr syscall variants, permission checks and attribute removal PASS')
