#include "gconsole.h"
#include "io.h"
#include "mm/malloc.h"
#include <cpu/time.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <fs/vfs.h>
#include <sys/capability.h>
#include <fs/mount.h>
#include <disk/block.h>
#include <disk/bcache.h>
#include <scheduler/scheduler.h>
#include <syscall/xorgtrace.h>
#include <errno.h>
#include <memory.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <string.h>

struct iovec { void *iov_base; uint64_t iov_len; };

struct linux_stat {
    uint64_t st_dev; uint64_t st_ino; uint64_t st_nlink;
    uint32_t st_mode; uint32_t st_uid; uint32_t st_gid; uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size; int64_t st_blksize; int64_t st_blocks;
    uint64_t st_atime; uint64_t st_atime_nsec;
    uint64_t st_mtime; uint64_t st_mtime_nsec;
    uint64_t st_ctime; uint64_t st_ctime_nsec;
    int64_t  __unused[3];
};

struct statx_timestamp {
    int64_t tv_sec;
    uint32_t tv_nsec;
    int32_t __reserved;
};

struct linux_statx {
    uint32_t stx_mask;
    uint32_t stx_blksize;
    uint64_t stx_attributes;
    uint32_t stx_nlink;
    uint32_t stx_uid;
    uint32_t stx_gid;
    uint16_t stx_mode;
    uint16_t __spare0[1];
    uint64_t stx_ino;
    uint64_t stx_size;
    uint64_t stx_blocks;
    uint64_t stx_attributes_mask;
    struct statx_timestamp stx_atime;
    struct statx_timestamp stx_btime;
    struct statx_timestamp stx_ctime;
    struct statx_timestamp stx_mtime;
    uint32_t stx_rdev_major;
    uint32_t stx_rdev_minor;
    uint32_t stx_dev_major;
    uint32_t stx_dev_minor;
    uint64_t stx_mnt_id;
    uint32_t stx_dio_mem_align;
    uint32_t stx_dio_offset_align;
    uint64_t __spare3[12];
};

struct linux_winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

struct linux_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

typedef struct {
    uint64_t fds_bits[16];
} linux_fd_set;

typedef struct {
    uint8_t buf[4096];
    uint32_t head;
    uint32_t tail;
} pipe_ring_t;

struct pollfd { int fd; short events; short revents; };

struct linux_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];
};

#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12

extern int pty_poll_events(vfs_file_t *file, short events);

extern int sys_memfd_create_impl(const char *name, uint32_t flags);
extern int sys_ftruncate_impl(int fd, int64_t length);
extern int sys_timerfd_create_impl(int clockid, int flags);
extern int sys_timerfd_settime_impl(int fd, int flags, const void *nv, void *ov);
extern int sys_timerfd_gettime_impl(int fd, void *cur);
extern int sys_signalfd4_impl(int fd, const void *mask, uint64_t sz, int flags);
extern int sys_eventfd2_impl(int initval, int flags);

static void ensure_user_range_mapped(uint64_t ptr, uint64_t len) {
    process_t *p = get_current_process();
    if (!p || !p->pml4 || ptr < 0x10000ULL) return;

    uint64_t start_page = ptr & ~4095ULL;
    uint64_t end_page   = (ptr + len + 4095) & ~4095ULL;

    for (uint64_t page = start_page; page < end_page; page += 4096) {
        if (page >= 0x00007FFFFFFFFFFFULL) break;
        if (!vmm_is_mapped(p->pml4, page)) {
            void *phys = pmm_alloc_page_and_clear();
            if (phys) {
                vmm_map_page(p->pml4, (uint64_t)phys, page, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
            }
        }
    }
}

static inline bool user_range_ok(uint64_t ptr, uint64_t len) {
    if (ptr == 0) return len == 0;
    if (ptr < 0x10000ULL) return false;

    uint64_t end;
    if (__builtin_add_overflow(ptr, len, &end)) return false;
    if (end > 0x00007FFFFFFFFFFFULL) return false;

    ensure_user_range_mapped(ptr, len);
    return true;
}

static int copy_str_from_user(char *dst, uint64_t src_user_ptr, size_t max_len) {
    if (!src_user_ptr || !user_range_ok(src_user_ptr, 1)) return -EFAULT;
    if (!max_len) return -ENAMETOOLONG;

    for (size_t i = 0; i < max_len; i++) {
        if (!user_range_ok(src_user_ptr + i, 1)) return -EFAULT;
        char c = *(const char *)(src_user_ptr + i);
        dst[i] = c;
        if (c == '\0') return 0;
    }
    dst[max_len - 1] = '\0';
    /* Never execute a mutation against a silently truncated pathname. */
    return -ENAMETOOLONG;
}

uint64_t sys_memfd_create_w(uint64_t name, uint64_t flags) {
    return (uint64_t)(int64_t)sys_memfd_create_impl((const char *)name, (uint32_t)flags);
}
uint64_t sys_ftruncate_w(uint64_t fd, uint64_t len) {
    return (uint64_t)(int64_t)sys_ftruncate_impl((int)fd, (int64_t)len);
}
uint64_t sys_timerfd_create_w(uint64_t c, uint64_t f) {
    return (uint64_t)(int64_t)sys_timerfd_create_impl((int)c, (int)f);
}
uint64_t sys_timerfd_settime_w(uint64_t fd, uint64_t flags, uint64_t nv, uint64_t ov) {
    return (uint64_t)(int64_t)sys_timerfd_settime_impl((int)fd, (int)flags, (const void *)nv, (void *)ov);
}
uint64_t sys_timerfd_gettime_w(uint64_t fd, uint64_t cur) {
    return (uint64_t)(int64_t)sys_timerfd_gettime_impl((int)fd, (void *)cur);
}
uint64_t sys_signalfd4_w(uint64_t fd, uint64_t mask, uint64_t sz, uint64_t fl) {
    return (uint64_t)(int64_t)sys_signalfd4_impl((int)fd, (const void *)mask, sz, (int)fl);
}
uint64_t sys_eventfd2_w(uint64_t initval, uint64_t flags) {
    return (uint64_t)(int64_t)sys_eventfd2_impl((int)initval, (int)flags);
}

uint64_t sys_mprotect_impl(uint64_t addr, uint64_t len, uint64_t prot) {
    (void)addr; (void)len; (void)prot;
    return 0;
}

uint64_t sys_munmap_impl(uint64_t addr, uint64_t len) {
    (void)addr; (void)len;
    return 0;
}

uint64_t sys_access_impl(uint64_t path, uint64_t mode) {
    if (!path || !user_range_ok(path, 1)) return (uint64_t)-EFAULT;
    if (mode & ~7ULL) return (uint64_t)-EINVAL;

    vfs_node_t *node = vfs_resolve_path((const char *)path);
    if (!node) return (uint64_t)-ENOENT;

    if (mode == 0) return 0;

    process_t *p = get_current_process();
    if (!p) return (uint64_t)-ESRCH;

    int res = vfs_check_access(node, p->euid, p->egid, (int)mode);
    if (res != 0) return (uint64_t)-EACCES;

    return 0;
}

uint64_t sys_lseek_impl(uint64_t fd, uint64_t offset, uint64_t whence) {
    process_t *p = get_current_process();
    int ifd = (int32_t)fd;
    if (!p || !p->fd_table || ifd < 0 || ifd >= VFS_MAX_FD ||
        !p->fd_table->fds[ifd])
        return (uint64_t)-EBADF;
    return (uint64_t)vfs_lseek(p->fd_table->fds[ifd], (int64_t)offset, (int)whence);
}

uint64_t sys_poll_impl(uint64_t fds_ptr, uint64_t nfds, uint64_t timeout) {
    if (nfds > VFS_MAX_FD) return (uint64_t)-EINVAL;
    if (nfds && !user_range_ok(fds_ptr, nfds * sizeof(struct pollfd)))
        return (uint64_t)-EFAULT;

    process_t *p = get_current_process();
    if (!p || !p->fd_table) return (uint64_t)-EINVAL;

    struct pollfd *fds = (struct pollfd *)fds_ptr;
    uint64_t start = get_monotonic_ms();

    while (1) {
        int ready = 0;
        for (uint64_t i = 0; i < nfds; i++) {
            fds[i].revents = 0;
            int fd = fds[i].fd;
            if (fd < 0) continue; /* Linux ignores negative descriptors. */
            if (fd >= VFS_MAX_FD || !p->fd_table->fds[fd]) {
                fds[i].revents = 0x20; /* POLLNVAL (Linux ABI) */
                ready++;
                continue;
            }

            vfs_file_t *file = p->fd_table->fds[fd];
            short rev = 0;
            if (file->node && file->node->file_ops && file->node->file_ops->poll) {
                rev = file->node->file_ops->poll(file, fds[i].events);
            } else {
                short pty_rev = pty_poll_events(file, fds[i].events);
                if (pty_rev) rev = pty_rev;
                else {
                    if (fds[i].events & 4) rev |= 4;
                    if (fds[i].events & 1) {
                        if (file->node &&
                            (file->node->type == VFS_TYPE_CHAR_DEV ||
                            file->offset < file->node->size))
                            rev |= 1;
                    }
                }
            }
            if (rev) { fds[i].revents = rev; ready++; }
        }
        if (ready > 0) return (uint64_t)ready;
        if (p->sigpending & ~p->sigmask) return (uint64_t)-EINTR;
        if (timeout == 0) return 0;
        if (timeout != (uint64_t)-1 && get_monotonic_ms() - start >= timeout)
            return 0;
        schedule_yield();
    }
}

uint64_t sys_ppoll_impl(uint64_t fds_ptr, uint64_t nfds, uint64_t ts_ptr,
                        uint64_t sigmask_ptr, uint64_t sigsetsize) {
    uint64_t timeout = (uint64_t)-1;
    if (ts_ptr) {
        if (!user_range_ok(ts_ptr, sizeof(struct linux_timespec)))
            return (uint64_t)-EFAULT;
        const struct linux_timespec *ts = (const struct linux_timespec *)ts_ptr;
        if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000LL)
            return (uint64_t)-EINVAL;
        /* The kernel timer is millisecond-resolution; round UP so a
           sub-millisecond ppoll never returns before its deadline. */
        if ((uint64_t)ts->tv_sec > (INT64_MAX - 999) / 1000)
            timeout = INT64_MAX;
        else {
            timeout = (uint64_t)ts->tv_sec * 1000 +
                      ((uint64_t)ts->tv_nsec + 999999) / 1000000;
        }
    }
    process_t *p = get_current_process();
    if (!p) return (uint64_t)-ESRCH;
    if (!sigmask_ptr) return sys_poll_impl(fds_ptr, nfds, timeout);
    if (sigsetsize != sizeof(uint64_t)) return (uint64_t)-EINVAL;
    if (!user_range_ok(sigmask_ptr, sizeof(uint64_t))) return (uint64_t)-EFAULT;

    /* The current kernel tracks a process-wide mask. Restore it even on
       timeout/error; a per-thread mask is required for full Linux parity. */
    uint64_t previous = p->sigmask;
    p->sigmask = *(const uint64_t *)sigmask_ptr &
                 ~((1ULL << (9 - 1)) | (1ULL << (19 - 1))); /* KILL, STOP */
    uint64_t result = sys_poll_impl(fds_ptr, nfds, timeout);
    p->sigmask = previous;
    return result;
}

/* Linux F_* command numbers. */
#define LINUX_F_DUPFD          0
#define LINUX_F_GETFD          1
#define LINUX_F_SETFD          2
#define LINUX_F_GETFL          3
#define LINUX_F_SETFL          4
#define LINUX_F_DUPFD_CLOEXEC  1030
#define LINUX_FD_CLOEXEC       1

uint64_t sys_fcntl_impl(uint64_t fd, uint64_t cmd, uint64_t arg) {
    process_t *p = get_current_process();
    int ifd = (int32_t)fd;
    if (!p || !p->fd_table || ifd < 0 || ifd >= VFS_MAX_FD ||
        !p->fd_table->fds[ifd])
        return (uint64_t)-EBADF;

    vfs_file_t *file = p->fd_table->fds[ifd];

    if (cmd == LINUX_F_DUPFD || cmd == LINUX_F_DUPFD_CLOEXEC) {
        int min_fd = (int)arg;
        if (min_fd < 0 || min_fd >= VFS_MAX_FD) return (uint64_t)-EINVAL;
        uint8_t flags = (cmd == LINUX_F_DUPFD_CLOEXEC) ? VFS_FD_CLOEXEC : 0;
        int newfd = vfs_fd_install(p->fd_table, file, min_fd, flags);
        return newfd < 0 ? (uint64_t)newfd : (uint64_t)newfd;
    }

    if (cmd == LINUX_F_GETFD) {
        return (p->fd_table->fd_flags[ifd] & VFS_FD_CLOEXEC) ?
               LINUX_FD_CLOEXEC : 0;
    }
    if (cmd == LINUX_F_SETFD) {
        p->fd_table->fd_flags[ifd] =
            (arg & LINUX_FD_CLOEXEC) ? VFS_FD_CLOEXEC : 0;
        return 0;
    }
    if (cmd == LINUX_F_GETFL) {
        uint32_t f = file->flags;
        uint64_t linux_fl = (f & VFS_O_RDWR) == VFS_O_RDWR ? 2 :
                            (f & VFS_O_WRITE) ? 1 : 0;
        if (f & VFS_O_APPEND) linux_fl |= 02000;
        if (f & VFS_O_NONBLOCK) linux_fl |= 04000;
        /* O_LARGEFILE is always set on x86-64. */
        return linux_fl | 0100000;
    }
    if (cmd == LINUX_F_SETFL) {
        uint32_t linux_fl = (uint32_t)arg;
        /* F_SETFL may change O_APPEND/O_NONBLOCK only; access mode stays. */
        uint32_t vfs_fl = file->flags & ~(VFS_O_NONBLOCK | VFS_O_APPEND);
        if (linux_fl & 04000) vfs_fl |= VFS_O_NONBLOCK;
        if (linux_fl & 02000) vfs_fl |= VFS_O_APPEND;
        file->flags = vfs_fl;
        return 0;
    }

    /* Unknown commands must not report success for an operation that did
       not happen. Many commands take pointer arguments we do not model yet. */
    return (uint64_t)-EINVAL;
}

/* AT_FDCWD is passed as a 32-bit signed int; userspace may clear the upper
   bits of the register, so compare the sign-extended low 32 bits only. */
static void fill_linux_stat(vfs_node_t *node, struct linux_stat *st) {
    memset(st, 0, sizeof(*st));
    st->st_ino   = node->inode_num ? node->inode_num : 1;
    st->st_nlink = 1;
    st->st_uid   = node->uid;
    st->st_gid   = node->gid;
    st->st_size  = node->size;
    st->st_blksize = 4096;
    st->st_blocks  = (node->size + 511) / 512;
    st->st_rdev    = (node->type == VFS_TYPE_BLOCK_DEV ||
                      node->type == VFS_TYPE_CHAR_DEV) ? node->inode_num : 0;

    uint32_t mode = node->mode;

    mode &= 07777;
    switch (node->type) {
        case VFS_TYPE_DIR:       mode |= 0040000; break;
        case VFS_TYPE_CHAR_DEV:  mode |= 0020000; break;
        case VFS_TYPE_BLOCK_DEV: mode |= 0060000; break;
        case VFS_TYPE_SYMLINK:   mode |= 0120000; break;
        default:                 mode |= 0100000; break;
    }
    st->st_mode = mode;
}

#define LINUX_AT_FDCWD (-100)

/* AT_* flags accepted by the *at() family. */
#define LINUX_AT_SYMLINK_NOFOLLOW 0x100
#define LINUX_AT_REMOVEDIR        0x200
#define LINUX_AT_SYMLINK_FOLLOW   0x400
#define LINUX_AT_NO_AUTOMOUNT     0x800
#define LINUX_AT_EMPTY_PATH       0x1000
#define LINUX_AT_EACCESS          0x200

static vfs_file_t *fdt_file(process_fd_table_t *table, uint64_t fd_arg) {
    int fd = (int32_t)fd_arg;
    if (!table || fd < 0 || fd >= VFS_MAX_FD || !table->fds[fd]) return NULL;
    return table->fds[fd];
}

/* Build the absolute kernel path for a *at() syscall. Absolute names ignore
   the descriptor; relative names are prefixed with the directory the
   descriptor refers to. Returns 0, a negative errno, or the length of the
   prefix written into out (callers append the relative name). */
static int resolve_dirfd_prefix(uint64_t dirfd_arg, const char *path,
                                char *out, size_t cap, size_t *prefix_len) {
    if (!path) return -EFAULT;
    if (path[0] == '/' || path[0] == '\0') {
        *prefix_len = 0;
        return 0;
    }
    if ((int32_t)dirfd_arg == LINUX_AT_FDCWD) {
        *prefix_len = 0; /* the cwd is applied by normalize_path() */
        return 0;
    }

    process_t *p = get_current_process();
    vfs_file_t *file = fdt_file(p ? p->fd_table : NULL, dirfd_arg);
    if (!file) return -EBADF;
    if (!file->node) return -EBADF;
    if (file->node->type != VFS_TYPE_DIR) return -ENOTDIR;
    if (!file->node->name[0]) return -ENOENT;

    const char *base = file->node->name;
    size_t len = strlen(base);
    if (len + 2 >= cap) return -ENAMETOOLONG;
    memcpy(out, base, len);
    if (len == 0 || base[len - 1] != '/') out[len++] = '/';
    out[len] = '\0';
    *prefix_len = len;
    return 0;
}

/* Resolve a *at() pathname into the caller-provided buffer. Absolute names
   and AT_FDCWD keep the original spelling so the VFS applies its logical
   cwd/mountpoint mapping. */
static int resolve_at_path(uint64_t dirfd_arg, uint64_t path_ptr,
                           char *out, size_t cap, size_t *prefix_len) {
    char raw[512];
    int rc = copy_str_from_user(raw, path_ptr, sizeof(raw));
    if (rc) return rc;
    rc = resolve_dirfd_prefix(dirfd_arg, raw, out, cap, prefix_len);
    if (rc) return rc;
    size_t base = *prefix_len;
    size_t raw_len = strlen(raw);
    if (base + raw_len + 1 > cap) return -ENAMETOOLONG;
    memcpy(out + base, raw, raw_len + 1);
    return 0;
}



uint64_t sys_fstat_impl(uint64_t fd, uint64_t statbuf) {
    process_t *p = get_current_process();
    if (!p || fd >= VFS_MAX_FD || !p->fd_table || !p->fd_table->fds[fd])
        return (uint64_t)-EBADF;
    if (!statbuf) return (uint64_t)-EFAULT;

    vfs_file_t *file = p->fd_table->fds[fd];
    if (!file->node) return (uint64_t)-EBADF;

    fill_linux_stat(file->node, (struct linux_stat *)statbuf);
    return 0;
}

uint64_t sys_stat_impl(uint64_t path, uint64_t statbuf) {
    if (!path || !statbuf) return (uint64_t)-EFAULT;
    if (!user_range_ok(statbuf, sizeof(struct linux_stat)))
        return (uint64_t)-EFAULT;
    vfs_node_t *node = vfs_resolve_path((const char *)path);
    if (!node) return (uint64_t)-ENOENT;
    fill_linux_stat(node, (struct linux_stat *)statbuf);
    return 0;
}

uint64_t sys_lstat_impl(uint64_t path, uint64_t statbuf) {
    if (!path || !statbuf) return (uint64_t)-EFAULT;
    if (!user_range_ok(statbuf, sizeof(struct linux_stat)))
        return (uint64_t)-EFAULT;
    vfs_node_t *node = vfs_resolve_path_nofollow((const char *)path);
    if (!node) return (uint64_t)-ENOENT;
    fill_linux_stat(node, (struct linux_stat *)statbuf);
    return 0;
}

uint64_t sys_newfstatat_impl(uint64_t dirfd, uint64_t path,
                             uint64_t statbuf, uint64_t flags) {
    char kpath[512];
    if (!statbuf || !user_range_ok(statbuf, sizeof(struct linux_stat)))
        return (uint64_t)-EFAULT;
    /* Linux accepts AT_NO_AUTOMOUNT (a no-op on modern kernels), and libc
       uses AT_EMPTY_PATH to implement fstat on an existing descriptor. */
    if (flags & ~(uint64_t)(LINUX_AT_SYMLINK_NOFOLLOW | LINUX_AT_NO_AUTOMOUNT |
                            LINUX_AT_EMPTY_PATH)) return (uint64_t)-EINVAL;
    if (!path) return (uint64_t)-EFAULT;

    vfs_node_t *node;
    size_t prefix = 0;
    int rc = resolve_at_path(dirfd, path, kpath, sizeof(kpath), &prefix);
    if (rc) return (uint64_t)rc;
    (void)prefix;

    if (kpath[0] == '\0') {
        if (!(flags & LINUX_AT_EMPTY_PATH)) return (uint64_t)-ENOENT;
        if ((int32_t)dirfd == LINUX_AT_FDCWD) {
            node = vfs_resolve_path(".");
        } else {
            process_t *p = get_current_process();
            vfs_file_t *file = fdt_file(p ? p->fd_table : NULL, dirfd);
            if (!file || !file->node) return (uint64_t)-EBADF;
            node = file->node;
        }
    } else {
        node = flags & LINUX_AT_SYMLINK_NOFOLLOW ?
            vfs_resolve_path_nofollow(kpath) : vfs_resolve_path(kpath);
    }
    if (!node) return (uint64_t)-ENOENT;
    fill_linux_stat(node, (struct linux_stat *)statbuf);
    return 0;
}

uint64_t sys_statx_impl(uint64_t dirfd, uint64_t path, uint64_t flags,
                        uint64_t mask, uint64_t statxbuf) {
    process_t *p = get_current_process();
    if (!p || !p->fd_table) return (uint64_t)-EBADF;
    if (!statxbuf || !user_range_ok(statxbuf, sizeof(struct linux_statx)))
        return (uint64_t)-EFAULT;
    if (flags & ~(uint64_t)(LINUX_AT_SYMLINK_NOFOLLOW | LINUX_AT_NO_AUTOMOUNT |
                            LINUX_AT_EMPTY_PATH | 0x4000 /* AT_STATX_DONT_SYNC */))
        return (uint64_t)-EINVAL;
    (void)mask;

    vfs_node_t *node = NULL;
    char kpath[512];
    size_t prefix = 0;
    int rc = resolve_at_path(dirfd, path, kpath, sizeof(kpath), &prefix);
    if (rc == -ENAMETOOLONG && (flags & LINUX_AT_EMPTY_PATH)) rc = 0;
    if (rc) return (uint64_t)rc;
    (void)prefix;

    if ((flags & LINUX_AT_EMPTY_PATH) && kpath[0] == '\0') {
        vfs_file_t *file = fdt_file(p->fd_table, dirfd);
        if (!file || !file->node) return (uint64_t)-EBADF;
        node = file->node;
    } else {
        node = flags & LINUX_AT_SYMLINK_NOFOLLOW ?
            vfs_resolve_path_nofollow(kpath) : vfs_resolve_path(kpath);
        if (!node) return (uint64_t)-ENOENT;
    }

    struct linux_stat st;
    fill_linux_stat(node, &st);

    struct linux_statx *sx = (struct linux_statx *)statxbuf;
    memset(sx, 0, sizeof(*sx));
    sx->stx_mask = 0x71F;
    sx->stx_blksize = 4096;
    sx->stx_nlink = (uint32_t)st.st_nlink;
    sx->stx_uid = st.st_uid;
    sx->stx_gid = st.st_gid;
    sx->stx_mode = (uint16_t)st.st_mode;
    sx->stx_ino = st.st_ino;
    sx->stx_size = (uint64_t)st.st_size;
    sx->stx_blocks = (uint64_t)st.st_blocks;
    sx->stx_rdev_major = (uint32_t)(st.st_rdev >> 8);
    sx->stx_rdev_minor = (uint32_t)(st.st_rdev & 0xFF);
    return 0;
}

uint64_t sys_read_impl(uint64_t a1, uint64_t a2, uint64_t a3) {
    process_t *p = get_current_process();
    if (!p || !p->fd_table) return (uint64_t)-EBADF;
    if (!a2 || !user_range_ok(a2, a3)) return (uint64_t)-EFAULT;

    int fd = (int)a1;
    if (fd < 0 || fd >= VFS_MAX_FD || !p->fd_table->fds[fd]) {
        console_print_string("[READ ERR] Bad FD: ");
        console_print_num(fd);
        console_print_string("\n");
        return (uint64_t)-EBADF;
    }

    vfs_file_t *file = p->fd_table->fds[fd];
    if (!file->node) {
        console_print_string("[READ ERR] File node is NULL\n");
        return (uint64_t)-EBADF;
    }

    int64_t res = sys_read(p->fd_table, fd, (void *)a2, a3);
    if (res < 0 && res != -11) {
        console_print_string("[READ ERR] sys_read returned ");
        console_print_num(res);
        console_print_string(" node_type=");
        console_print_num(file->node->type);
        console_print_string(" flags=");
        console_print_hex(file->flags);
        console_print_string("\n");
    }
    return (uint64_t)res;
}

uint64_t sys_write_impl(uint64_t a1, uint64_t a2, uint64_t a3) {
    process_t *p = get_current_process();
    if (!p || !p->fd_table) return (uint64_t)-EBADF;
    if (!a2 || !user_range_ok(a2, a3)) return (uint64_t)-EFAULT;
    int64_t result = sys_write(p->fd_table, (int)a1, (const void *)a2, a3);
    /* dbus-run-session passes a pipe write fd to dbus-daemon for its bus
       address. If that write fails, capture the descriptor state before the
       daemon's subsequent fcntl scan overwrites the syscall ring. Do not
       log the buffer or the address (which may contain sensitive data). */
    if (result < 0 && p->trace_desktop && p->name &&
        strcmp(p->name, "dbus-daemon") == 0) {
        vfs_file_t *f = a1 < VFS_MAX_FD ? p->fd_table->fds[a1] : NULL;
        uint64_t detail = ((a1 & 0xffffULL) << 48) |
                          ((uint64_t)(f ? f->flags & 0xffffU : 0) << 32) |
                          ((uint64_t)(f && f->node) << 31) |
                          ((uint32_t)(-result));
        xorgtrace_stage("dbus.write.fail", p->pid, detail);
    }
    return (uint64_t)result;
}

uint64_t sys_writev_impl(uint64_t a1, uint64_t a2, uint64_t a3) {
    process_t *p = get_current_process();
    if (!p || !p->fd_table) return (uint64_t)-EBADF;
    if (a3 == 0) return 0;
    
    if (!a2 || !user_range_ok(a2, a3 * sizeof(struct iovec)))
        return (uint64_t)-EFAULT;

    struct iovec *vecs = (struct iovec *)a2;
    uint64_t total = 0;

    for (uint64_t i = 0; i < a3; i++) {
        if (vecs[i].iov_len == 0) continue;

        if (!vecs[i].iov_base || !user_range_ok((uint64_t)vecs[i].iov_base, vecs[i].iov_len)) {
            if (total > 0) return total;
            return (uint64_t)-EFAULT;
        }

        int res = sys_write(p->fd_table, (int)a1, vecs[i].iov_base, vecs[i].iov_len);
        if (res > 0) {
            total += (uint64_t)res;
        } else if (res < 0) {
            if (total > 0) return total;
            return (uint64_t)res;
        }
    }

    return total;
}

static inline uint32_t translate_linux_open_flags(uint64_t linux_flags) {
    uint32_t vfs_flags = 0;
    uint64_t acc = linux_flags & 3; // O_ACCMODE (0 = O_RDONLY, 1 = O_WRONLY, 2 = O_RDWR)

    if (acc == 1) {
        vfs_flags |= VFS_O_WRITE;
    } else if (acc == 2) {
        vfs_flags |= (VFS_O_READ | VFS_O_WRITE);
    } else {
        vfs_flags |= VFS_O_READ; 
    }

    if (linux_flags & 0100)     vfs_flags |= VFS_O_CREAT;
    if (linux_flags & 0200)     vfs_flags |= VFS_O_EXCL;
    if (linux_flags & 01000)    vfs_flags |= VFS_O_TRUNC;
    if (linux_flags & 02000)    vfs_flags |= VFS_O_APPEND;
    if (linux_flags & 04000)    vfs_flags |= VFS_O_NONBLOCK;
    if (linux_flags & 0200000)  vfs_flags |= VFS_O_DIRECTORY;
    if (linux_flags & 0400000)  vfs_flags |= VFS_O_NOFOLLOW;
    if (linux_flags & 02000000) vfs_flags |= VFS_O_CLOEXEC;

    return vfs_flags;
}

uint64_t sys_openat_impl(uint64_t dirfd, uint64_t path, uint64_t flags) {
    process_t *p = get_current_process();
    if (!p || !p->fd_table) return (uint64_t)-EBADF;
    char kpath[512];
    size_t prefix = 0;
    int rc = resolve_at_path(dirfd, path, kpath, sizeof(kpath), &prefix);
    if (rc) return (uint64_t)rc;
    /* O_NOFOLLOW must not expand a final symlink: plain open(2) reports
       ELOOP instead of silently opening the target (O_PATH is unsupported). */
    if (flags & 0400000 /* O_NOFOLLOW */) {
        vfs_node_t *existing = vfs_resolve_path_nofollow(kpath);
        if (existing && existing->type == VFS_TYPE_SYMLINK)
            return (uint64_t)-ELOOP;
    }
    (void)prefix;
    return (uint64_t)(int64_t)sys_open(p->fd_table, kpath,
                                      translate_linux_open_flags(flags));
}

uint64_t sys_open_impl(uint64_t path, uint64_t flags, uint64_t mode) {
    (void)mode;
    return sys_openat_impl((uint64_t)-100, path, flags);
}

/* truncate(2): path based variant of ftruncate(2). Unlike ftruncate it takes
   no descriptor, so DAC is checked against the resolved inode instead. */
uint64_t sys_truncate_impl(uint64_t path, uint64_t length) {
    if (!path) return (uint64_t)-EFAULT;
    if ((int64_t)length < 0) return (uint64_t)-EINVAL;

    vfs_node_t *node = vfs_resolve_path((const char *)path);
    if (!node) return (uint64_t)-ENOENT;
    if (node->type == VFS_TYPE_DIR) return (uint64_t)-EISDIR;
    if (node->type != VFS_TYPE_FILE) return (uint64_t)-EINVAL;

    process_t *p = get_current_process();
    if (!p || !p->fd_table) return (uint64_t)-EBADF;
    if (vfs_check_access(node, p->fsuid, p->fsgid, VFS_PERM_WRITE) != 0)
        return (uint64_t)-EACCES;

    int res = vfs_truncate_node(node, length);
    return (uint64_t)(int64_t)res;
}

uint64_t sys_close_impl(uint64_t a1) {
    process_t *p = get_current_process();
    if (!p || !p->fd_table) return (uint64_t)-EBADF;
    return (uint64_t)(int64_t)sys_close(p->fd_table, (int)a1);
}

#define LINUX_CLOSE_RANGE_CLOEXEC (1 << 2)
#define LINUX_CLOSE_RANGE_UNSHARE (1 << 1)

uint64_t sys_close_range_impl(uint64_t first_arg, uint64_t last_arg, uint64_t flags) {
    uint32_t first = (uint32_t)first_arg, last = (uint32_t)last_arg;
    if (first > last) return (uint64_t)-EINVAL;
    if (flags & ~(uint64_t)(LINUX_CLOSE_RANGE_CLOEXEC | LINUX_CLOSE_RANGE_UNSHARE))
        return (uint64_t)-EINVAL;
    process_t *p = get_current_process();
    if (!p || !p->fd_table) return (uint64_t)-EBADF;
    if (first >= VFS_MAX_FD) return 0;
    if (last >= VFS_MAX_FD) last = VFS_MAX_FD - 1;
    /* The descriptor table is already private to each process, so the
       UNSHARE bit only changes how failures are reported in Linux; here it
       is a no-op and never changes close/close-on-exec semantics. */
    for (uint32_t fd = first; fd <= last; fd++) {
        if (!p->fd_table->fds[fd]) continue;
        if (flags & LINUX_CLOSE_RANGE_CLOEXEC)
            p->fd_table->fd_flags[fd] |= VFS_FD_CLOEXEC;
        else
            sys_close(p->fd_table, (int)fd);
    }
    return 0;
}

uint64_t sys_mmap_impl(uint64_t addr, uint64_t len, uint64_t prot,
                       uint64_t flags, uint64_t fd, uint64_t offset) {
    process_t *p = get_current_process();
    if (!p || len == 0) return (uint64_t)-EINVAL;
    if (!p->pml4) return (uint64_t)-ENOMEM;

    if (p->mmap_base == 0) {
        p->mmap_base = 0x5000000000ULL;
    }

    uint64_t page_aligned_len = (len + 4095) & ~4095ULL;

    uint64_t virt_addr;
    if ((flags & 0x10 /* MAP_FIXED */) && addr != 0) {
        virt_addr = addr;
    } else {
        virt_addr = p->mmap_base;
        p->mmap_base += page_aligned_len;
    }

    uint64_t start_page = virt_addr & ~4095ULL;
    uint64_t end_page   = (virt_addr + len + 4095) & ~4095ULL;

    if (!(flags & 0x20 /* MAP_ANONYMOUS */) && (int)fd >= 0 && (int)fd < VFS_MAX_FD && p->fd_table && p->fd_table->fds[(int)fd]) {
        vfs_file_t *f = p->fd_table->fds[(int)fd];
        if (f->node && f->node->file_ops && f->node->file_ops->mmap) {
            void* res = f->node->file_ops->mmap(f, (void *)virt_addr, len, (int)prot, (int)flags, (uint64_t)offset);
            if (res == NULL || res == (void *)-1) return (uint64_t)-ENOMEM;
            return (uint64_t)res;
        }
    }

    uint64_t alloc_size = end_page - start_page;
    uint64_t pte_flags  = PTE_PRESENT | PTE_USER | PTE_WRITABLE;

    for (uint64_t page = start_page; page < end_page; page += 4096) {
        if ((flags & 0x10 /* MAP_FIXED */) || !vmm_is_mapped(p->pml4, page)) {
            void *phys = pmm_alloc_page_and_clear();
            if (!phys) return (uint64_t)-ENOMEM;
            vmm_map_page(p->pml4, (uint64_t)phys, page, pte_flags);
        }
    }

    if (!(flags & 0x20 /* MAP_ANONYMOUS */) && (int)fd >= 0 && p->fd_table) {
        if ((int)fd < VFS_MAX_FD && p->fd_table->fds[(int)fd]) {
            vfs_file_t *f = p->fd_table->fds[(int)fd];
            vfs_lseek(f, (int64_t)offset, VFS_SEEK_SET);
            
            int read_bytes = vfs_read(f, (void *)virt_addr, len);
            if (read_bytes >= 0 && (uint64_t)read_bytes < alloc_size) {
                memset((void *)(virt_addr + read_bytes), 0, alloc_size - read_bytes);
            }
        }
    } else {
        memset((void *)virt_addr, 0, len);
    }

    return virt_addr;
}

static bool console_mode_allowed(process_t *p) {
    return p && (p->is_kernel_process ||
                 (p->cap_effective & BOLDON_CAP_SYS_ADMIN));
}

uint64_t sys_ioctl_impl(uint64_t a1, uint64_t a2, uint64_t a3) {
    process_t *p = get_current_process();
    if (!p || !p->fd_table) return (uint64_t)-EBADF;

    int fd = (int)a1;
    if (fd < 0 || fd >= VFS_MAX_FD || !p->fd_table->fds[fd])
        return (uint64_t)-EBADF;

    vfs_file_t *file = p->fd_table->fds[fd];

    if (file && file->node && file->node->type == VFS_TYPE_CHAR_DEV && a2 == 0x4B3A) {
        /* This ioctl actually changes the shared framebuffer owner. */
        if (!console_mode_allowed(p)) return (uint64_t)-EPERM;
        if (a3 == 2) {
            gconsole_acquire_framebuffer();
            return 0;
        }
        if (a3 == 0) {
            gconsole_release_framebuffer();
            return 0;
        }
        return (uint64_t)-EINVAL;
    }

    if (file && file->node && file->node->type == VFS_TYPE_CHAR_DEV && a2 == 0x4B3B) {
        if (!a3 || !user_range_ok(a3, sizeof(int))) return (uint64_t)-EFAULT;
        *(int *)a3 = gconsole_framebuffer_acquired() ? 2 : 0;
        return 0;
    }

    if (file && file->node && file->node->file_ops && file->node->file_ops->ioctl) {
        int res = file->node->file_ops->ioctl(file, a2, (void *)a3);
        if (res != -25 && res != -ENOTTY) return (uint64_t)(int64_t)res;
    }

    switch (a2) {
        case 0x4B3A: /* KDSETMODE is only valid for the handled console char device */
            return (uint64_t)-ENOTTY;
        case 0x4B3B: /* KDGETMODE */
        case 0x4B4B: /* KDGKBMODE */
        case 0x4B45: /* KDSKBMODE */
        case 0x5600: /* VT_OPENQRY */
        case 0x5601: /* VT_GETMODE */
        case 0x5602: /* VT_SETMODE */
        case 0x5603: /* VT_GETSTATE */
        case 0x5606: /* VT_ACTIVATE */
        case 0x5607: /* VT_WAITACTIVE */
        case 0x560B: /* VT_DISALLOCATE */
            return 0;
    }

    return (uint64_t)-25; /* -ENOTTY */
}

uint64_t sys_pselect6_impl(uint64_t nfds, uint64_t readfds_ptr, uint64_t writefds_ptr,
uint64_t exceptfds_ptr, uint64_t timeout_ptr, uint64_t sigmask_ptr) {
    (void)exceptfds_ptr; (void)sigmask_ptr;

    process_t *p = get_current_process();
    if (!p || !p->fd_table) return (uint64_t)-EINVAL;

    linux_fd_set *readfds  = (readfds_ptr  && user_range_ok(readfds_ptr, sizeof(linux_fd_set))) ? (linux_fd_set *)readfds_ptr : NULL;
    linux_fd_set *writefds = (writefds_ptr && user_range_ok(writefds_ptr, sizeof(linux_fd_set))) ? (linux_fd_set *)writefds_ptr : NULL;
    struct linux_timespec *ts = (timeout_ptr && user_range_ok(timeout_ptr, sizeof(struct linux_timespec))) ? (struct linux_timespec *)timeout_ptr : NULL;

    int64_t timeout_ms = -1;
    if (ts) {
        if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000LL)
            return (uint64_t)-EINVAL;
        timeout_ms = ts->tv_sec * 1000 + ts->tv_nsec / 1000000;
        if (timeout_ms == 0 && ts->tv_nsec > 0) timeout_ms = 1;
    }

    uint64_t start = get_monotonic_ms();

    while (1) {
        int ready = 0;
        linux_fd_set res_readfds;
        linux_fd_set res_writefds;
        memset(&res_readfds, 0, sizeof(res_readfds));
        memset(&res_writefds, 0, sizeof(res_writefds));

        for (uint64_t fd = 0; fd < nfds && fd < VFS_MAX_FD; fd++) {
            if (!p->fd_table->fds[fd]) continue;
            vfs_file_t *file = p->fd_table->fds[fd];

            if (readfds && (readfds->fds_bits[fd / 64] & (1ULL << (fd % 64)))) {
                short rev = 0;

                /* === ИСПРАВЛЕНИЕ: сначала спрашиваем poll самого устройства! === */
                if (file->node && file->node->file_ops && file->node->file_ops->poll) {
                    rev = file->node->file_ops->poll(file, 1 /* POLLIN */);
                } else {
                    rev = pty_poll_events(file, 1 /* POLLIN */);
                    if (!rev && file->node && file->node->type != VFS_TYPE_CHAR_DEV) {
                        if (file->offset < file->node->size) {
                            rev = 1;
                        }
                    }
                }

                if (rev & 1) {
                    res_readfds.fds_bits[fd / 64] |= (1ULL << (fd % 64));
                    ready++;
                }
            }

            if (writefds && (writefds->fds_bits[fd / 64] & (1ULL << (fd % 64)))) {
                res_writefds.fds_bits[fd / 64] |= (1ULL << (fd % 64));
                ready++;
            }
        }

        if (ready > 0) {
            if (readfds)  memcpy(readfds, &res_readfds, sizeof(linux_fd_set));
            if (writefds) memcpy(writefds, &res_writefds, sizeof(linux_fd_set));
            return (uint64_t)ready;
        }

        if (timeout_ms == 0) {
            if (readfds)  memset(readfds, 0, sizeof(linux_fd_set));
            if (writefds) memset(writefds, 0, sizeof(linux_fd_set));
            return 0;
        }

        if (timeout_ms > 0 && get_monotonic_ms() - start >= (uint64_t)timeout_ms) {
            if (readfds)  memset(readfds, 0, sizeof(linux_fd_set));
            if (writefds) memset(writefds, 0, sizeof(linux_fd_set));
            return 0;
        }

        schedule_yield();
    }
}

uint64_t sys_dup_impl(uint64_t oldfd) {
    return sys_fcntl_impl(oldfd, LINUX_F_DUPFD, 0);
}

uint64_t sys_dup2_impl(uint64_t oldfd, uint64_t newfd) {
    process_t *p = get_current_process();
    if (!p || !p->fd_table) return (uint64_t)-EBADF;

    int iold = (int32_t)oldfd;
    int inew = (int32_t)newfd;
    if (iold < 0 || iold >= VFS_MAX_FD || !p->fd_table->fds[iold])
        return (uint64_t)-EBADF;
    if (inew < 0 || inew >= VFS_MAX_FD)
        return (uint64_t)-EBADF;
    if (iold == inew) return newfd;

    /* dup2 always copies to a descriptor without close-on-exec. */
    vfs_fd_install_at(p->fd_table, inew, p->fd_table->fds[iold], 0);
    return newfd;
}

/* dup3 adds flags and rejects newfd == oldfd (Linux returns EINVAL there). */
uint64_t sys_dup3_impl(uint64_t oldfd, uint64_t newfd, uint64_t flags) {
    process_t *p = get_current_process();
    if (!p || !p->fd_table) return (uint64_t)-EBADF;
    if (flags & ~(uint64_t)02000000 /* O_CLOEXEC */) return (uint64_t)-EINVAL;

    int iold = (int32_t)oldfd;
    int inew = (int32_t)newfd;
    if (iold < 0 || iold >= VFS_MAX_FD || !p->fd_table->fds[iold])
        return (uint64_t)-EBADF;
    if (inew < 0 || inew >= VFS_MAX_FD)
        return (uint64_t)-EBADF;
    if (iold == inew) return (uint64_t)-EINVAL;

    vfs_fd_install_at(p->fd_table, inew, p->fd_table->fds[iold],
                      (flags & 02000000) ? VFS_FD_CLOEXEC : 0);
    return newfd;
}

uint64_t sys_readlink_impl(uint64_t path, uint64_t buf, uint64_t bufsiz) {
    if (!path || !buf || bufsiz == 0) return (uint64_t)-EFAULT;
    if (!user_range_ok(path, 1) || !user_range_ok(buf, bufsiz)) return (uint64_t)-EFAULT;

    /* readlink() must not follow the final symlink; vfs_readlink resolves
       the parent only and reads the link body without expanding it. */
    int res = vfs_readlink((const char *)path, (char *)buf, bufsiz);
    if (res < 0) return (uint64_t)res;
    return (uint64_t)res;
}

uint64_t sys_readlinkat_impl(uint64_t dirfd, uint64_t path, uint64_t buf,
                             uint64_t bufsiz) {
    if (!path || !buf || bufsiz == 0) return (uint64_t)-EFAULT;
    if (!user_range_ok(buf, bufsiz)) return (uint64_t)-EFAULT;
    char kpath[512];
    size_t prefix = 0;
    int rc = resolve_at_path(dirfd, path, kpath, sizeof(kpath), &prefix);
    if (rc) return (uint64_t)rc;
    (void)prefix;
    int res = vfs_readlink(kpath, (char *)buf, bufsiz);
    return res < 0 ? (uint64_t)res : (uint64_t)res;
}

/* Linux d_type values. */
#define LINUX_DT_UNKNOWN  0
#define LINUX_DT_FIFO     1
#define LINUX_DT_CHR      2
#define LINUX_DT_DIR      4
#define LINUX_DT_BLK      6
#define LINUX_DT_REG      8
#define LINUX_DT_LNK      10
#define LINUX_DT_SOCK     12

static uint8_t linux_dtype(vfs_node_type_t type) {
    switch (type) {
        case VFS_TYPE_FILE:      return LINUX_DT_REG;
        case VFS_TYPE_DIR:       return LINUX_DT_DIR;
        case VFS_TYPE_SYMLINK:   return LINUX_DT_LNK;
        case VFS_TYPE_BLOCK_DEV: return LINUX_DT_BLK;
        case VFS_TYPE_CHAR_DEV:  return LINUX_DT_CHR;
        case VFS_TYPE_FIFO:      return LINUX_DT_FIFO;
        case VFS_TYPE_SOCKET:    return LINUX_DT_SOCK;
        default:                 return LINUX_DT_UNKNOWN;
    }
}

uint64_t sys_getdents64_impl(uint64_t fd_raw, uint64_t dirp_raw, uint64_t count_raw) {
    int fd = (int)fd_raw;
    uint8_t *buf = (uint8_t *)dirp_raw;
    size_t count = (size_t)count_raw;

    if (!buf || count == 0) return (uint64_t)-EINVAL;

    process_t *curr = get_current_process();
    if (!curr || !curr->fd_table || fd < 0 || fd >= VFS_MAX_FD ||
        !curr->fd_table->fds[fd]) return (uint64_t)-EBADF;
    if (!user_range_ok(dirp_raw, count)) return (uint64_t)-EFAULT;
    vfs_file_t *file = curr->fd_table->fds[fd];
    if (!file->node || file->node->type != VFS_TYPE_DIR) return (uint64_t)-ENOTDIR;

    size_t total_bytes = 0;

    for (;;) {
        vfs_dirent_t dent;
        memset(&dent, 0, sizeof(dent));

        /* readdir advances file->offset; a short buffer must simply stop
           writing while leaving the offset on the next unconsumed entry. */
        int res = vfs_readdir(file, &dent);
        if (res < 0) break;

        size_t name_len = strlen(dent.name);
        if (name_len == 0) continue;

        size_t reclen = (19 + name_len + 1 + 7) & ~(size_t)7;
        if (total_bytes + reclen > count) {
            /* Push the record back: the backend iterator already moved, so
               the next getdents64() must return this same entry instead of
               silently skipping it. */
            vfs_readdir_pushback(file, &dent);
            /* Nothing fit at all: report EINVAL like Linux filldir() does
               instead of returning 0 forever for an undersized buffer. */
            if (total_bytes == 0) return (uint64_t)-EINVAL;
            break;
        }

        struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + total_bytes);
        d->d_ino = dent.inode_num;
        d->d_off = (int64_t)(file->offset);
        d->d_reclen = (uint16_t)reclen;
        d->d_type = linux_dtype(dent.type);

        memset(d->d_name, 0, reclen - 19);
        memcpy(d->d_name, dent.name, name_len);
        d->d_name[name_len] = '\0';

        total_bytes += reclen;
    }

    return (uint64_t)total_bytes;
}


uint64_t sys_readv_impl(uint64_t a1, uint64_t a2, uint64_t a3) {
    process_t *p = get_current_process();
    if (!p || !p->fd_table) return (uint64_t)-EBADF;
    if (a3 == 0) return 0;

    if (!a2 || !user_range_ok(a2, a3 * sizeof(struct iovec)))
        return (uint64_t)-EFAULT;

    struct iovec *vecs = (struct iovec *)a2;
    uint64_t total = 0;

    for (uint64_t i = 0; i < a3; i++) {
        if (vecs[i].iov_len == 0) continue;

        if (!vecs[i].iov_base || !user_range_ok((uint64_t)vecs[i].iov_base, vecs[i].iov_len)) {
            if (total > 0) return total;
            return (uint64_t)-EFAULT;
        }

        int res = sys_read(p->fd_table, (int)a1, vecs[i].iov_base, vecs[i].iov_len);
        if (res > 0) {
            total += (uint64_t)res;
        } else if (res < 0) {
            if (total > 0) return total;
            return (uint64_t)res;
        } else {
            break; // EOF
        }
    }

    return total;
}

static int pipe_read(vfs_file_t *f, void *buf, uint64_t count) {
    if (!f || !f->node || !f->node->fs_data) return -1;
    pipe_ring_t *p = (pipe_ring_t *)f->node->fs_data;
    if (count == 0) return 0;

    while (p->head == p->tail) {
        if (f->flags & VFS_O_NONBLOCK) return -EAGAIN;
        schedule_yield();
    }

    uint8_t *out = (uint8_t *)buf;
    uint64_t got = 0;
    while (got < count && p->tail != p->head) {
        out[got++] = p->buf[p->tail];
        p->tail = (p->tail + 1) % 4096;
    }
    return (int)got;
}

static int pipe_write(vfs_file_t *f, const void *buf, uint64_t count) {
    if (!f || !f->node || !f->node->fs_data) return -1;
    pipe_ring_t *p = (pipe_ring_t *)f->node->fs_data;
    const uint8_t *in = (const uint8_t *)buf;
    uint64_t wrote = 0;

    while (wrote < count) {
        uint32_t next = (p->head + 1) % 4096;
        if (next == p->tail) {
            if (f->flags & VFS_O_NONBLOCK) break;
            schedule_yield();
            continue;
        }
        p->buf[p->head] = in[wrote++];
        p->head = next;
    }
    return (int)wrote;
}

static vfs_file_ops_t pipe_file_ops = {
    .read  = pipe_read,
    .write = pipe_write
};

uint64_t sys_pipe2_impl(uint64_t pipefd_ptr, uint64_t flags) {
    if (!pipefd_ptr || !user_range_ok(pipefd_ptr, 8)) return (uint64_t)-EFAULT;

    process_t *proc = get_current_process();
    if (!proc || !proc->fd_table) return (uint64_t)-EINVAL;

    pipe_ring_t *ring = malloc_alloc(sizeof(pipe_ring_t));
    if (!ring) return (uint64_t)-ENOMEM;
    memset(ring, 0, sizeof(pipe_ring_t));

    vfs_node_t *node = malloc_alloc(sizeof(vfs_node_t));
    if (!node) { malloc_free(ring); return (uint64_t)-ENOMEM; }
    memset(node, 0, sizeof(vfs_node_t));
    node->type = VFS_TYPE_CHAR_DEV;
    node->file_ops = &pipe_file_ops;
    node->fs_data = ring;

    vfs_file_t *rf = malloc_alloc(sizeof(vfs_file_t));
    vfs_file_t *wf = malloc_alloc(sizeof(vfs_file_t));
    if (!rf || !wf) return (uint64_t)-ENOMEM;
    memset(rf, 0, sizeof(vfs_file_t));
    memset(wf, 0, sizeof(vfs_file_t));

    uint32_t vfs_flags = (flags & 04000 /* O_NONBLOCK */) ? VFS_O_NONBLOCK : 0;
    rf->node = node; rf->flags = VFS_O_READ | vfs_flags; rf->refcount = 1;
    wf->node = node; wf->flags = VFS_O_WRITE | vfs_flags; wf->refcount = 1;

    int rfd = -1, wfd = -1;
    for (int i = 0; i < VFS_MAX_FD; i++) {
        if (!proc->fd_table->fds[i]) {
            if (rfd < 0) { rfd = i; proc->fd_table->fds[i] = rf; }
            else if (wfd < 0) { wfd = i; proc->fd_table->fds[i] = wf; break; }
        }
    }

    if (rfd < 0 || wfd < 0) return (uint64_t)-EMFILE;

    int32_t *fds = (int32_t *)pipefd_ptr;
    fds[0] = rfd;
    fds[1] = wfd;
    return 0;
}

/* Linux splice(2): exactly one endpoint must be a pipe. Copy through a
   bounded kernel buffer for now; never report success without moving bytes. */
uint64_t sys_splice_impl(uint64_t fd_in, uint64_t off_in, uint64_t fd_out,
                         uint64_t off_out, uint64_t len, uint64_t flags) {
    process_t *proc = get_current_process();
    if (!proc || !proc->fd_table || fd_in >= VFS_MAX_FD || fd_out >= VFS_MAX_FD ||
        !proc->fd_table->fds[fd_in] || !proc->fd_table->fds[fd_out])
        return (uint64_t)-EBADF;
    vfs_file_t *src = proc->fd_table->fds[fd_in];
    vfs_file_t *dst = proc->fd_table->fds[fd_out];
    bool src_pipe = src->node && src->node->file_ops == &pipe_file_ops;
    bool dst_pipe = dst->node && dst->node->file_ops == &pipe_file_ops;
    if (!src_pipe && !dst_pipe) return (uint64_t)-EINVAL;
    if (src->node == dst->node) return (uint64_t)-EINVAL;
    if ((off_in && src_pipe) || (off_out && dst_pipe)) return (uint64_t)-ESPIPE;
    if (flags & ~0xFu) return (uint64_t)-EINVAL;
    if ((off_in && !user_range_ok(off_in, sizeof(uint64_t))) ||
        (off_out && !user_range_ok(off_out, sizeof(uint64_t))))
        return (uint64_t)-EFAULT;
    if ((off_in && (int64_t)*(uint64_t *)off_in < 0) ||
        (off_out && (int64_t)*(uint64_t *)off_out < 0))
        return (uint64_t)-EINVAL;
    if (!(src->flags & VFS_O_READ) || !(dst->flags & VFS_O_WRITE))
        return (uint64_t)-EBADF;
    if (len == 0) return 0;
    if (len > 0x7FFFFFFFFFFFFFFFULL) len = 0x7FFFFFFFFFFFFFFFULL;

    uint8_t *buf = malloc_alloc(4096);
    if (!buf) return (uint64_t)-ENOMEM;
    uint64_t done = 0;
    int error = 0;
    while (done < len) {
        uint64_t want = len - done;
        if (want > 4096) want = 4096;
        if (dst_pipe) {
            pipe_ring_t *ring = dst->node->fs_data;
            if (!ring) { error = -EBADF; break; }
            uint32_t used = (ring->head + 4096 - ring->tail) % 4096;
            uint32_t room = 4095 - used;
            if (!room) {
                if ((flags & 2) || (dst->flags & VFS_O_NONBLOCK)) {
                    error = -EAGAIN; break;
                }
                schedule_yield();
                continue;
            }
            if (want > room) want = room;
        }
        if (src_pipe && ((flags & 2) || (src->flags & VFS_O_NONBLOCK))) {
            pipe_ring_t *ring = src->node->fs_data;
            if (!ring) { error = -EBADF; break; }
            uint32_t available = (ring->head + 4096 - ring->tail) % 4096;
            if (!available) { error = -EAGAIN; break; }
            if (want > available) want = available;
        }
        uint64_t old_in = src->offset, old_out = dst->offset;
        if (off_in) src->offset = *(uint64_t *)off_in;
        if (off_out) dst->offset = *(uint64_t *)off_out;
        int nr = vfs_read(src, buf, want);
        if (nr > 0) {
            int nw = vfs_write(dst, buf, (uint64_t)nr);
            if (nw > 0) done += (uint64_t)nw;
            if (nw < nr) {
                /* A short write must not be mistaken for a complete splice. */
                error = nw < 0 ? nw : -EAGAIN;
            }
        } else if (nr < 0) error = nr;
        if (off_in) { *(uint64_t *)off_in = src->offset; src->offset = old_in; }
        if (off_out) { *(uint64_t *)off_out = dst->offset; dst->offset = old_out; }
        if (nr <= 0 || error) break;
    }
    malloc_free(buf);
    return done ? done : (uint64_t)error;
}

/* Apply the same filesystem credentials to mutations as sys_open uses.
   Direct in-kernel VFS operations are deliberately outside this user check. */
static int mutation_parent_access(const char *path) {
    process_t *p = get_current_process();
    if (!p || !p->fd_table) return -EBADF;
    char name[256] = {0};
    vfs_node_t *parent = vfs_resolve_path_ext(path, true, name);
    if (!parent) return -ENOENT;
    if (parent->type != VFS_TYPE_DIR) return -ENOTDIR;
    return vfs_check_access(parent, p->fsuid, p->fsgid,
                            VFS_PERM_WRITE | VFS_PERM_EXEC) ? -EACCES : 0;
}

static int mutation_owner_access(vfs_node_t *node) {
    process_t *p = get_current_process();
    if (!node) return -ENOENT;
    if (!p) return -EBADF;
    return p->fsuid == node->uid ||
           (p->cap_effective & BOLDON_CAP_FOWNER) ? 0 : -EPERM;
}

/* New entries honour the process umask, like Linux does for open/mkdir. */
static uint32_t apply_umask(uint32_t mode) {
    process_t *p = get_current_process();
    uint32_t mask = (p && p->umask) ? p->umask : 0;
    return mode & ~mask & 07777;
}

uint64_t sys_mkdirat_impl(uint64_t dirfd, uint64_t path_ptr, uint64_t mode) {
    char kpath[512];
    size_t prefix = 0;
    int rc = resolve_at_path(dirfd, path_ptr, kpath, sizeof(kpath), &prefix);
    if (rc) return (uint64_t)rc;
    (void)prefix;
    int access = mutation_parent_access(kpath);
    if (access) return (uint64_t)(int64_t)access;
    int res = vfs_mkdir(kpath, apply_umask((uint32_t)mode));
    if (res < 0) {
        return (uint64_t)res;
    }

    return 0;
}

uint64_t sys_mkdir_impl(uint64_t path_ptr, uint64_t mode) {
    return sys_mkdirat_impl((uint64_t)(int64_t)LINUX_AT_FDCWD, path_ptr, mode);
}

uint64_t sys_renameat_impl(uint64_t oldfd, uint64_t old_ptr,
                           uint64_t newfd, uint64_t new_ptr) {
    char oldpath[512], newpath[512];
    size_t prefix = 0;
    int rc = resolve_at_path(oldfd, old_ptr, oldpath, sizeof(oldpath), &prefix);
    if (rc) return (uint64_t)rc;
    rc = resolve_at_path(newfd, new_ptr, newpath, sizeof(newpath), &prefix);
    if (rc) return (uint64_t)rc;
    int access = mutation_parent_access(oldpath);
    if (!access) access = mutation_parent_access(newpath);
    if (access) return (uint64_t)(int64_t)access;
    return (uint64_t)(int64_t)vfs_rename(oldpath, newpath);
}

uint64_t sys_unlinkat_impl(uint64_t dirfd, uint64_t path_ptr, uint64_t flags) {
    char kpath[512];
    size_t prefix = 0;
    int rc = resolve_at_path(dirfd, path_ptr, kpath, sizeof(kpath), &prefix);
    if (rc) return (uint64_t)rc;
    (void)prefix;
    if (flags & ~0x200ULL) return (uint64_t)-EINVAL;
    int access = mutation_parent_access(kpath);
    if (access) return (uint64_t)(int64_t)access;
    int res;
    if (flags & 0x200 /* AT_REMOVEDIR */) {
        res = vfs_rmdir(kpath);
    } else {
        res = vfs_unlink(kpath);
    }

    if (res < 0) return (uint64_t)res;
    return 0;
}

uint64_t sys_unlink_impl(uint64_t path_ptr) {
    return sys_unlinkat_impl((uint64_t)(int64_t)LINUX_AT_FDCWD, path_ptr, 0);
}

uint64_t sys_rmdir_impl(uint64_t path_ptr) {
    return sys_unlinkat_impl((uint64_t)(int64_t)LINUX_AT_FDCWD, path_ptr,
                             0x200 /* AT_REMOVEDIR */);
}

uint64_t sys_fchmodat_impl(uint64_t dirfd, uint64_t path_ptr, uint64_t mode, uint64_t flags) {
    char kpath[512];
    size_t prefix = 0;
    int rc = resolve_at_path(dirfd, path_ptr, kpath, sizeof(kpath), &prefix);
    if (rc) return (uint64_t)rc;
    (void)prefix;
    /* AT_SYMLINK_NOFOLLOW is the only documented flag here; chmod(2) on the
       link itself is not supported by this VFS, so reject it explicitly. */
    if (flags & ~0x100ULL) return (uint64_t)-EINVAL;
    if (flags & 0x100ULL) return (uint64_t)-ENOTSUP;
    int access = mutation_owner_access(vfs_resolve_path(kpath));
    if (access) return (uint64_t)(int64_t)access;
    int res = vfs_chmod(kpath, (uint32_t)mode);
    if (res < 0) return (uint64_t)-ENOENT;
    
    return 0;
}

uint64_t sys_chmod_impl(uint64_t path_ptr, uint64_t mode) {
    return sys_fchmodat_impl((uint64_t)(int64_t)LINUX_AT_FDCWD, path_ptr, mode, 0);
}

uint64_t sys_linkat_impl(uint64_t olddirfd, uint64_t oldpath_ptr,
                         uint64_t newdirfd, uint64_t newpath_ptr,
                         uint64_t flags) {
    char kold[512], knew[512];
    size_t prefix = 0;
    /* AT_SYMLINK_FOLLOW may be ignored: the VFS resolves symlinks here. */
    if (flags & ~0x400ULL) return (uint64_t)-EINVAL;
    int rc = resolve_at_path(olddirfd, oldpath_ptr, kold, sizeof(kold), &prefix);
    if (rc) return (uint64_t)rc;
    rc = resolve_at_path(newdirfd, newpath_ptr, knew, sizeof(knew), &prefix);
    if (rc) return (uint64_t)rc;
    int access = mutation_parent_access(knew);
    if (access) return (uint64_t)(int64_t)access;
    int res = vfs_link(kold, knew);
    if (res < 0) return (uint64_t)res;
    return 0;
}

uint64_t sys_link_impl(uint64_t oldpath_ptr, uint64_t newpath_ptr) {
    return sys_linkat_impl((uint64_t)(int64_t)LINUX_AT_FDCWD, oldpath_ptr,
                           (uint64_t)(int64_t)LINUX_AT_FDCWD, newpath_ptr, 0);
}

/* Shared body for symlink(2) and symlinkat(2). The target string is stored
   verbatim, the link name is resolved against the descriptor. */
static uint64_t symlink_common(uint64_t target_ptr, uint64_t dirfd,
                               uint64_t linkpath_ptr) {
    char target[512], linkpath[512];
    size_t prefix = 0;
    int rc = copy_str_from_user(target, target_ptr, sizeof(target));
    if (rc) return (uint64_t)rc;
    /* Linux reports ENOENT for an empty target: a link to "" is useless. */
    if (target[0] == '\0') return (uint64_t)-ENOENT;
    rc = resolve_at_path(dirfd, linkpath_ptr, linkpath, sizeof(linkpath), &prefix);
    if (rc) return (uint64_t)rc;
    (void)prefix;
    int access = mutation_parent_access(linkpath);
    if (access) return (uint64_t)(int64_t)access;
    return (uint64_t)(int64_t)vfs_symlink(target, linkpath);
}

uint64_t sys_symlink_impl(uint64_t target_ptr, uint64_t linkpath_ptr) {
    return symlink_common(target_ptr, (uint64_t)(int64_t)LINUX_AT_FDCWD,
                          linkpath_ptr);
}

uint64_t sys_symlinkat_impl(uint64_t target_ptr, uint64_t newdirfd,
                            uint64_t linkpath_ptr) {
    return symlink_common(target_ptr, newdirfd, linkpath_ptr);
}


uint64_t sys_fchmod_impl(uint64_t fd, uint64_t mode) {
    process_t *p = get_current_process();
    if (!p || !p->fd_table || fd >= VFS_MAX_FD || !p->fd_table->fds[fd])
        return (uint64_t)-EBADF;

    vfs_file_t *file = p->fd_table->fds[fd];
    int access = mutation_owner_access(file->node);
    if (access) return (uint64_t)(int64_t)access;
    int res = vfs_fchmod(file, (uint32_t)mode);
    if (res < 0) return (uint64_t)res;

    return 0;
}

uint64_t sys_faccessat2_impl(uint64_t dfd, uint64_t filename, uint64_t mode, uint64_t flags) {
    char path[512];
    size_t prefix = 0;
    if (mode & ~7ULL) return (uint64_t)-EINVAL;
    if (flags & ~(uint64_t)(LINUX_AT_EACCESS | LINUX_AT_SYMLINK_NOFOLLOW))
        return (uint64_t)-EINVAL;
    int rc = resolve_at_path(dfd, filename, path, sizeof(path), &prefix);
    if (rc) return (uint64_t)rc;
    (void)prefix;

    vfs_node_t *node = (flags & LINUX_AT_SYMLINK_NOFOLLOW) ?
        vfs_resolve_path_nofollow(path) : vfs_resolve_path(path);
    if (!node) return (uint64_t)-ENOENT;
    if (mode == 0) return 0;

    process_t *p = get_current_process();
    if (!p) return (uint64_t)-ESRCH;
    /* access(2) defaults to the real IDs; AT_EACCESS switches to the
       effective ones, which is what faccessat2() callers expect. */
    uint32_t uid = (flags & LINUX_AT_EACCESS) ? p->euid : p->fsuid;
    uint32_t gid = (flags & LINUX_AT_EACCESS) ? p->egid : p->fsgid;
    int res = vfs_check_access(node, uid, gid, (int)mode);
    return res != 0 ? (uint64_t)-EACCES : 0;
}

uint64_t sys_fstatfs_impl(uint64_t fd, uint64_t buf) {
    (void)fd;
    if (!buf || !user_range_ok(buf, 120)) return (uint64_t)-EFAULT;
    memset((void *)buf, 0, 120);
    *(uint64_t *)(buf + 0)  = 0xEF53;  // f_type = EXT4_SUPER_MAGIC
    *(uint64_t *)(buf + 8)  = 4096;    // f_bsize
    *(uint64_t *)(buf + 16) = 262144;  // f_blocks
    *(uint64_t *)(buf + 24) = 200000;  // f_bfree
    *(uint64_t *)(buf + 32) = 200000;  // f_bavail
    return 0;
}

uint64_t sys_reboot_impl(uint64_t magic, uint64_t magic2, uint64_t cmd, uint64_t arg) {
    (void)magic; (void)magic2; (void)cmd; (void)arg;
    process_t *p = get_current_process();
    if (!p || (!p->is_kernel_process && !(p->cap_effective & BOLDON_CAP_SYS_BOOT)))
        return (uint64_t)-EPERM;
    outb(0x64, 0xFE);
    return 0;
}
uint64_t sys_mount_impl(uint64_t source, uint64_t target, uint64_t fstype,
                        uint64_t flags, uint64_t data) {
    process_t *p = get_current_process();
    if (!p || (!p->is_kernel_process && !(p->cap_effective & BOLDON_CAP_SYS_ADMIN)))
        return (uint64_t)-EPERM;
    char ksource[256], ktarget[256], kfstype[64];
    memset(ksource, 0, sizeof(ksource));
    memset(ktarget, 0, sizeof(ktarget));
    memset(kfstype, 0, sizeof(kfstype));

    if (!source || !target) return (uint64_t)-EFAULT;
    if (copy_str_from_user(ksource, source, sizeof(ksource)) != 0) return (uint64_t)-EFAULT;
    if (copy_str_from_user(ktarget, target, sizeof(ktarget)) != 0) return (uint64_t)-EFAULT;
    if (fstype && copy_str_from_user(kfstype, fstype, sizeof(kfstype)) != 0)
        return (uint64_t)-EFAULT;

    int res = vfs_mount(ksource, ktarget, kfstype, (uint32_t)flags, (const void *)data);
    return (uint64_t)(int64_t)res;
}

uint64_t sys_umount2_impl(uint64_t target, uint64_t flags) {
    process_t *p = get_current_process();
    if (!p || (!p->is_kernel_process && !(p->cap_effective & BOLDON_CAP_SYS_ADMIN)))
        return (uint64_t)-EPERM;
    char ktarget[256];
    memset(ktarget, 0, sizeof(ktarget));
    if (!target) return (uint64_t)-EFAULT;
    if (copy_str_from_user(ktarget, target, sizeof(ktarget)) != 0) return (uint64_t)-EFAULT;
    int res = vfs_umount(ktarget, (uint32_t)flags);
    return (uint64_t)(int64_t)res;
}

uint64_t sys_fsync_impl(uint64_t fd) {
    process_t *p = get_current_process();
    if (!p || !p->fd_table || fd >= VFS_MAX_FD || !p->fd_table->fds[fd])
        return (uint64_t)-EBADF;
    for (int i = 0; i < MAX_BLOCK_DEVICES; i++) {
        if (block_devices[i].sector_count > 0 && block_devices[i].name[0] &&
            bcache_flush(block_devices[i].name) != 0)
            return (uint64_t)-EIO;
    }
    return 0;
}

uint64_t sys_pread64_impl(uint64_t fd, uint64_t buf, uint64_t count, uint64_t offset) {
    process_t *p = get_current_process();
    if (!p || !p->fd_table || fd >= VFS_MAX_FD || !p->fd_table->fds[fd])
        return (uint64_t)-EBADF;
    vfs_file_t *file = p->fd_table->fds[fd];
    uint64_t saved = file->offset;
    file->offset = offset;
    int res = vfs_read(file, (void *)buf, count);
    file->offset = saved;
    return (uint64_t)(int64_t)res;
}

uint64_t sys_pwrite64_impl(uint64_t fd, uint64_t buf, uint64_t count, uint64_t offset) {
    process_t *p = get_current_process();
    if (!p || !p->fd_table || fd >= VFS_MAX_FD || !p->fd_table->fds[fd])
        return (uint64_t)-EBADF;
    if (!user_range_ok(buf, count)) return (uint64_t)-EFAULT;
    vfs_file_t *file = p->fd_table->fds[fd];
    uint64_t saved = file->offset;
    file->offset = offset;
    int res = vfs_write(file, (const void *)buf, count);
    file->offset = saved;
    return (uint64_t)(int64_t)res;
}

/* Linux xattr syscalls 188..199.  Only security.capability is exposed:
   unsupported namespaces must not be advertised as successful writes. */
uint64_t sys_xattr_impl(uint64_t nr, uint64_t a1, uint64_t a2,
                        uint64_t a3, uint64_t a4, uint64_t a5) {
    if (nr < 188 || nr > 199) return (uint64_t)-ENOSYS;
    unsigned op = (unsigned)(nr - 188) / 3;
    unsigned variant = (unsigned)(nr - 188) % 3; /* path, lpath, fd */
    process_t *p = get_current_process();
    if (!p || !p->fd_table) return (uint64_t)-EBADF;
    vfs_node_t *node;
    if (variant == 2) {
        if (a1 >= VFS_MAX_FD || !p->fd_table->fds[a1]) return (uint64_t)-EBADF;
        node = p->fd_table->fds[a1]->node;
    } else {
        char path[512];
        if (copy_str_from_user(path, a1, sizeof(path))) return (uint64_t)-EFAULT;
        node = variant == 1 ? vfs_resolve_path_nofollow(path) : vfs_resolve_path(path);
        if (!node) return (uint64_t)-ENOENT;
    }
    if (!node || !node->ops || !node->ops->getxattr) return (uint64_t)-EOPNOTSUPP;
    if (vfs_check_access(node, p->fsuid, p->fsgid, VFS_PERM_READ))
        return (uint64_t)-EACCES;
    const char *cap_name = "security.capability";
    char name[64];
    if (op != 2) {
        if (copy_str_from_user(name, a2, sizeof(name))) return (uint64_t)-EFAULT;
        if (strcmp(name, cap_name)) return (uint64_t)-EOPNOTSUPP;
    }
    if (op == 0) {
        if (vfs_check_access(node, p->fsuid, p->fsgid, VFS_PERM_WRITE))
            return (uint64_t)-EACCES;
        if (a4 > 24) return (uint64_t)-E2BIG;
        if (a5) return (uint64_t)-EOPNOTSUPP; /* CREATE/REPLACE not atomic here */
        if (node->type != VFS_TYPE_FILE || !(p->cap_effective & BOLDON_CAP_SETFCAP))
            return (uint64_t)-EPERM;
        if (!a4 || !user_range_ok(a3, a4)) return (uint64_t)-EFAULT;
        uint8_t value[24];
        memcpy(value, (const void *)a3, (size_t)a4);
        boldon_file_caps_t decoded;
        int rc = capability_parse_file(value, (int)a4, &decoded);
        if (rc) return (uint64_t)(int64_t)rc;
        return (uint64_t)(int64_t)vfs_setxattr(node, cap_name, value, a4);
    }
    if (op == 3) {
        if (vfs_check_access(node, p->fsuid, p->fsgid, VFS_PERM_WRITE))
            return (uint64_t)-EACCES;
        if (!(p->cap_effective & BOLDON_CAP_SETFCAP)) return (uint64_t)-EPERM;
        return (uint64_t)(int64_t)vfs_removexattr(node, cap_name);
    }
    uint8_t value[24];
    int len = node->ops->getxattr(node, cap_name, value, sizeof(value));
    if (op == 2) {
        if (len == -ENODATA) return 0;
        if (len < 0) return (uint64_t)(int64_t)len;
        size_t required = strlen(cap_name) + 1;
        if (!a3) return required;
        if (a3 < required) return (uint64_t)-ERANGE;
        if (!user_range_ok(a2, required)) return (uint64_t)-EFAULT;
        memcpy((void *)a2, cap_name, required);
        return required;
    }
    if (len < 0) return (uint64_t)(int64_t)len;
    if (!a4) return (uint64_t)len;
    if (a4 < (uint64_t)len) return (uint64_t)-ERANGE;
    if (!user_range_ok(a3, (uint64_t)len)) return (uint64_t)-EFAULT;
    memcpy((void *)a3, value, (size_t)len);
    return (uint64_t)len;
}
