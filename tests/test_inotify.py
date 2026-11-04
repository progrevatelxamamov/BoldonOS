"""Host C regression for the real kernel inotify implementation (no fake FD)."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'kernel/src/fs/inotify.c').read_text()
source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
handler = (root / 'kernel/src/syscall/syscall.c').read_text()
header = (root / 'kernel/include/syscall/syscall.h').read_text()
vfs = (root / 'kernel/src/fs/vfs.c').read_text()
for name, number in [('INIT', 253), ('ADD_WATCH', 254), ('RM_WATCH', 255), ('INIT1', 294)]:
    assert f'#define SYS_INOTIFY_{name}' in header
    assert f'case SYS_INOTIFY_{name}:' in handler
for mask in ('IN_CREATE', 'IN_DELETE', 'IN_MODIFY', 'IN_CLOSE_WRITE'):
    assert f'inotify_vfs_event(' in vfs and mask in vfs

code = r'''
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <errno.h>
#define VFS_MAX_FD 256
#define VFS_O_READ 1
#define VFS_O_NONBLOCK 32
#define VFS_O_CLOEXEC 64
#define VFS_TYPE_DIR 2
#define VFS_TYPE_CHAR_DEV 5
#define IN_ACCESS 1U
#define IN_MODIFY 2U
#define IN_CLOSE_WRITE 8U
#define IN_CREATE 0x100U
#define IN_DELETE 0x200U
#define IN_DELETE_SELF 0x400U
#define IN_MOVED_FROM 0x40U
#define IN_MOVED_TO 0x80U
#define IN_MOVE_SELF 0x800U
#define IN_UNMOUNT 0x2000U
#define IN_Q_OVERFLOW 0x4000U
#define IN_IGNORED 0x8000U
#define IN_ONLYDIR 0x01000000U
#define IN_DONT_FOLLOW 0x02000000U
#define IN_EXCL_UNLINK 0x04000000U
#define IN_MASK_CREATE 0x10000000U
#define IN_MASK_ADD 0x20000000U
#define IN_ISDIR 0x40000000U
#define IN_ONESHOT 0x80000000U
#define PTE_PRESENT 1
#define PTE_WRITABLE 2
#define PTE_USER 4
typedef struct vfs_mount { int unused; } vfs_mount_t;
typedef struct vfs_node vfs_node_t;
typedef struct vfs_file vfs_file_t;
typedef struct { int (*read)(vfs_file_t *, void *, uint64_t); uint32_t (*poll)(vfs_file_t *, uint32_t); int (*ioctl)(vfs_file_t *, uint64_t, void *); int (*close)(vfs_file_t *); } vfs_file_ops_t;
struct vfs_node { int type; uint64_t inode_num; vfs_mount_t *mount; vfs_file_ops_t *file_ops; };
struct vfs_file { vfs_node_t *node; void *private_data; uint32_t flags, refcount; };
typedef struct { vfs_file_t *fds[VFS_MAX_FD]; } process_fd_table_t;
typedef struct { process_fd_table_t *fd_table; void *pml4; uint64_t sigpending, sigmask; } process_t;
typedef struct { int unused; } spinlock_t;
#define SPINLOCK_INIT {0}
static void spin_lock_irqsave(spinlock_t *lock, uint64_t *flags) { (void)lock; *flags=0; }
static void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) { (void)lock; (void)flags; }
static process_fd_table_t table;
static process_t proc = { .fd_table = &table };
static vfs_mount_t mount;
static vfs_node_t dir = { .type=VFS_TYPE_DIR, .inode_num=42, .mount=&mount };
static vfs_node_t same_dir = { .type=VFS_TYPE_DIR, .inode_num=42, .mount=&mount };
static vfs_node_t file_node = { .inode_num=43, .mount=&mount };
static process_t *get_current_process(void) { return &proc; }
static vfs_node_t *vfs_resolve_path(const char *p) { return strcmp(p, "/tmp") == 0 ? &dir : strcmp(p, "/tmp/f") == 0 ? &file_node : NULL; }
static int nofollow_calls;
static vfs_node_t *vfs_resolve_path_nofollow(const char *p) { nofollow_calls++; return vfs_resolve_path(p); }
static void *malloc_alloc(uint64_t n) { return malloc(n); }
static void malloc_free(void *p) { free(p); }
static void schedule_yield(void) { abort(); }
static int vmm_is_mapped(void *p, uint64_t a) { (void)p; (void)a; return 1; }
static void *pmm_alloc_page_and_clear(void) { abort(); }
static void vmm_map_page(void *p, uint64_t phys, uint64_t addr, uint64_t flags) { (void)p; (void)phys; (void)addr; (void)flags; }
''' + source + r'''
int main(void) {
    uint64_t fd = sys_inotify_init1_impl(04000 | 02000000);
    assert(fd < VFS_MAX_FD);
    vfs_file_t *f = table.fds[fd];
    assert(f->flags & VFS_O_NONBLOCK);
    assert(f->flags & VFS_O_CLOEXEC);
    assert(f->node->file_ops->read(f, (char[16]){0}, 16) == -EAGAIN);
    assert(sys_inotify_init1_impl(1) == (uint64_t)-EINVAL);
    assert(sys_inotify_add_watch_impl(fd, (uintptr_t)"/missing", IN_CREATE) == (uint64_t)-ENOENT);
    assert(sys_inotify_add_watch_impl(fd, (uintptr_t)"/tmp/f", IN_CREATE | IN_ONLYDIR) == (uint64_t)-ENOTDIR);
    assert(sys_inotify_add_watch_impl(fd, (uintptr_t)"/tmp", IN_MODIFY | IN_MASK_ADD | IN_MASK_CREATE) == (uint64_t)-EINVAL);
    assert(sys_inotify_add_watch_impl(fd, (uintptr_t)"/tmp", IN_ISDIR | IN_MODIFY) == (uint64_t)-EINVAL);
    assert(sys_inotify_add_watch_impl(fd, (uintptr_t)"/tmp/f", IN_MODIFY | IN_DONT_FOLLOW) > 0);
    assert(nofollow_calls == 1);
    assert(sys_inotify_add_watch_impl(fd, (uintptr_t)"/tmp", IN_CREATE | IN_EXCL_UNLINK) > 0);
    int wd = (int)sys_inotify_add_watch_impl(fd, (uintptr_t)"/tmp", IN_CREATE | IN_DELETE);
    assert(wd > 0);
    assert(sys_inotify_add_watch_impl(fd, (uintptr_t)"/tmp", IN_MODIFY | IN_MASK_CREATE) == (uint64_t)-EEXIST);
    assert(sys_inotify_add_watch_impl(fd, (uintptr_t)"/tmp", IN_MODIFY | IN_MASK_ADD) == (uint64_t)wd);
    inotify_vfs_event(&same_dir, IN_CREATE, "f");
    int unread = 0;
    assert(f->node->file_ops->ioctl(f, 0x541B, &unread) == 0 && unread == 20);
    assert(f->node->file_ops->poll(f, 1) == 1);
    char buf[1024] = {0};
    assert(f->node->file_ops->read(f, buf, 16) == -EINVAL); /* indivisible event */
    int n = f->node->file_ops->read(f, buf, sizeof(buf));
    assert(n == 20);
    struct in_event *e = (struct in_event *)buf;
    assert(e->wd == wd && e->mask == IN_CREATE && e->len == 4 && strcmp(e->name, "f") == 0);
    assert(f->node->file_ops->poll(f, 1) == 0);
    inotify_vfs_event(&dir, IN_MODIFY, NULL);
    assert(f->node->file_ops->read(f, buf, sizeof(buf)) == 16);
    assert(e->mask == IN_MODIFY && e->len == 0);
    for (int j=0; j<65; j++) inotify_vfs_event(&dir, IN_MODIFY, NULL);
    char many[8192];
    assert(f->node->file_ops->read(f, many, sizeof(many)) == 65 * 16);
    assert(((struct in_event *)(many + 64 * 16))->mask == IN_Q_OVERFLOW);
    assert(sys_inotify_rm_watch_impl(fd, wd) == 0);
    assert(f->node->file_ops->read(f, buf, sizeof(buf)) == 16 && e->mask == IN_IGNORED);
    assert(sys_inotify_rm_watch_impl(fd, wd) == (uint64_t)-EINVAL);
    int file_wd = (int)sys_inotify_add_watch_impl(fd, (uintptr_t)"/tmp/f", IN_DELETE_SELF);
    inotify_vfs_event(&file_node, IN_DELETE_SELF, NULL);
    assert(f->node->file_ops->read(f, buf, sizeof(buf)) == 32);
    assert(e->wd == file_wd && e->mask == IN_DELETE_SELF);
    assert(((struct in_event *)(buf + 16))->mask == IN_IGNORED);
    assert(sys_inotify_rm_watch_impl(fd, file_wd) == (uint64_t)-EINVAL);
    int move_wd = (int)sys_inotify_add_watch_impl(fd, (uintptr_t)"/tmp",
                                                    IN_MOVED_FROM | IN_MOVED_TO);
    inotify_vfs_move(&dir, "old", &same_dir, "new", &file_node);
    assert(f->node->file_ops->read(f, buf, sizeof(buf)) == 40);
    struct in_event *from = (struct in_event *)buf;
    struct in_event *to = (struct in_event *)(buf + 20);
    assert(from->wd == move_wd && from->mask == IN_MOVED_FROM);
    assert(to->wd == move_wd && to->mask == IN_MOVED_TO);
    assert(from->cookie && from->cookie == to->cookie);
    assert(strcmp(from->name, "old") == 0 && strcmp(to->name, "new") == 0);
    assert(sys_inotify_rm_watch_impl(fd, move_wd) == 0);
    assert(f->node->file_ops->read(f, buf, sizeof(buf)) == 16);
    int one_move = (int)sys_inotify_add_watch_impl(fd, (uintptr_t)"/tmp",
                                      IN_MOVED_FROM | IN_MOVED_TO | IN_ONESHOT);
    inotify_vfs_move(&dir, "old", &same_dir, "new", &file_node);
    assert(f->node->file_ops->read(f, buf, sizeof(buf)) == 36);
    assert(e->wd == one_move && e->mask == IN_MOVED_FROM);
    assert(((struct in_event *)(buf + 20))->mask == IN_IGNORED);
    int once = (int)sys_inotify_add_watch_impl(fd, (uintptr_t)"/tmp/f", IN_MODIFY | IN_ONESHOT);
    inotify_vfs_event(&file_node, IN_MODIFY, NULL);
    inotify_vfs_event(&file_node, IN_MODIFY, NULL);
    assert(f->node->file_ops->read(f, buf, sizeof(buf)) == 32);
    assert(e->wd == once && e->mask == IN_MODIFY);
    assert(((struct in_event *)(buf + 16))->mask == IN_IGNORED);
    inotify_vfs_event(&dir, IN_CREATE, "f");
    assert(f->node->file_ops->read(f, buf, 16) == -EAGAIN);
    int unmount_wd = (int)sys_inotify_add_watch_impl(fd, (uintptr_t)"/tmp", IN_MODIFY);
    inotify_vfs_unmount(&mount);
    assert(f->node->file_ops->read(f, buf, sizeof(buf)) == 32);
    assert(e->wd == unmount_wd && e->mask == IN_UNMOUNT);
    assert(((struct in_event *)(buf + 16))->mask == IN_IGNORED);
    f->node->file_ops->close(f); free(f); table.fds[fd] = NULL;
    assert(instances == NULL);
    puts("inotify: watch, events, poll/read, short buffer, remove and close passed");
}
'''

with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / 'inotify_test.c'
    exe = Path(tmp) / 'inotify_test'
    src.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-o', str(exe), str(src)], check=True)
    subprocess.run([str(exe)], check=True)
