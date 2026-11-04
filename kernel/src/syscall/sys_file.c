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
    if (!p || fd >= VFS_MAX_FD || !p->fd_table || !p->fd_table->fds[fd])
        return (uint64_t)-EBADF;
    return (uint64_t)vfs_lseek(p->fd_table->fds[fd], (int64_t)offset, (int)whence);
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

uint64_t sys_fcntl_impl(uint64_t fd, uint64_t cmd, uint64_t arg) {
    process_t *p = get_current_process();
    if (!p || !p->fd_table || fd >= VFS_MAX_FD || !p->fd_table->fds[fd])
        return (uint64_t)-EBADF;

    if (cmd == 0 || cmd == 1030) {
        int min_fd = (int)arg;
        if (min_fd < 0) min_fd = 0;
        for (int n = min_fd; n < VFS_MAX_FD; n++) {
            if (!p->fd_table->fds[n]) {
                p->fd_table->fds[n] = p->fd_table->fds[fd];
                p->fd_table->fds[n]->refcount++;
                return (uint64_t)n;
            }
        }
        return (uint64_t)-EMFILE;
    }

    if (cmd == 1 /* F_GETFD */) return 0;
    if (cmd == 2 /* F_SETFD */) return 0;
    if (cmd == 3 /* F_GETFL */) return p->fd_table->fds[fd]->flags;
    if (cmd == 4 /* F_SETFL */) {
        uint32_t linux_fl = (uint32_t)arg;
        uint32_t vfs_fl = p->fd_table->fds[fd]->flags & ~(VFS_O_NONBLOCK | VFS_O_APPEND);
        if (linux_fl & 04000) vfs_fl |= VFS_O_NONBLOCK;
        if (linux_fl & 02000) vfs_fl |= VFS_O_APPEND;
        p->fd_table->fds[fd]->flags = vfs_fl;
        return 0;
    }

    return 0;
}

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
    vfs_node_t *node = vfs_resolve_path((const char *)path);
    if (!node) return (uint64_t)-ENOENT;
    fill_linux_stat(node, (struct linux_stat *)statbuf);
    return 0;
}

uint64_t sys_newfstatat_impl(uint64_t dirfd, uint64_t path,
                             uint64_t statbuf, uint64_t flags) {
    char kpath[512];
    if (!statbuf || !user_range_ok(statbuf, sizeof(struct linux_stat)) ||
        copy_str_from_user(kpath, path, sizeof(kpath))) return (uint64_t)-EFAULT;
    if ((int64_t)dirfd != -100 && kpath[0] != '/') return (uint64_t)-ENOSYS;
    if (flags & ~0x100ULL) return (uint64_t)-ENOSYS;
    vfs_node_t *node = flags & 0x100 /* AT_SYMLINK_NOFOLLOW */ ?
        vfs_resolve_path_nofollow(kpath) : vfs_resolve_path(kpath);
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
    if (flags & ~0x7900ULL) return (uint64_t)-EINVAL;
    (void)mask;

    vfs_node_t *node = NULL;
    const char *pathstr = (const char *)path;
    if ((flags & 0x1000) && (!pathstr || pathstr[0] == 0)) {
        int fd = (int)dirfd;
        if (fd < 0 || fd >= VFS_MAX_FD || !p->fd_table->fds[fd])
            return (uint64_t)-EBADF;
        node = p->fd_table->fds[fd]->node;
        if (!node) return (uint64_t)-EBADF;
    } else {
        if (!path) return (uint64_t)-EFAULT;
        node = vfs_resolve_path(pathstr);
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
    if (linux_flags & 01000)    vfs_flags |= VFS_O_TRUNC;
    if (linux_flags & 02000)    vfs_flags |= VFS_O_APPEND;
    if (linux_flags & 04000)    vfs_flags |= VFS_O_NONBLOCK;
    if (linux_flags & 02000000) vfs_flags |= VFS_O_CLOEXEC;

    return vfs_flags;
}

uint64_t sys_openat_impl(uint64_t dirfd, uint64_t path, uint64_t flags) {
    process_t *p = get_current_process();
    if (!p || !p->fd_table) return (uint64_t)-EBADF;
    char kpath[512];
    if (copy_str_from_user(kpath, path, sizeof(kpath))) return (uint64_t)-EFAULT;
    /* Absolute paths ignore dirfd; relative dirfd resolution is not yet
       implemented and must never silently use the process cwd instead. */
    if ((int64_t)dirfd != -100 && kpath[0] != '/') return (uint64_t)-ENOSYS;
    return (uint64_t)(int64_t)sys_open(p->fd_table, kpath,
                                      translate_linux_open_flags(flags));
}

uint64_t sys_open_impl(uint64_t path, uint64_t flags, uint64_t mode) {
    (void)mode;
    return sys_openat_impl((uint64_t)-100, path, flags);
}

uint64_t sys_close_impl(uint64_t a1) {
    process_t *p = get_current_process();
    if (!p || !p->fd_table) return (uint64_t)-EBADF;
    return (uint64_t)(int64_t)sys_close(p->fd_table, (int)a1);
}

uint64_t sys_close_range_impl(uint64_t first_arg, uint64_t last_arg, uint64_t flags) {
    uint32_t first = (uint32_t)first_arg, last = (uint32_t)last_arg;
    if (first > last) return (uint64_t)-EINVAL;
    /* CLOSE_RANGE_UNSHARE and CLOEXEC require per-descriptor state; do not
       silently advertise those semantics with a process-wide file flag. */
    if (flags != 0) return (uint64_t)-ENOSYS;
    process_t *p = get_current_process();
    if (!p || !p->fd_table) return (uint64_t)-EBADF;
    if (first >= VFS_MAX_FD) return 0;
    if (last >= VFS_MAX_FD) last = VFS_MAX_FD - 1;
    for (uint32_t fd = first; fd <= last; fd++)
        if (p->fd_table->fds[fd]) sys_close(p->fd_table, (int)fd);
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

uint64_t sys_dup2_impl(uint64_t oldfd, uint64_t newfd) {
    process_t *p = get_current_process();
    if (!p || !p->fd_table) return (uint64_t)-EBADF;

    if (oldfd >= VFS_MAX_FD || !p->fd_table->fds[oldfd])
        return (uint64_t)-EBADF;

    if (newfd >= VFS_MAX_FD)
        return (uint64_t)-EBADF;

    if (oldfd == newfd) return newfd;

    if (p->fd_table->fds[newfd]) {
        sys_close(p->fd_table, (int)newfd);
    }

    p->fd_table->fds[newfd] = p->fd_table->fds[oldfd];
    p->fd_table->fds[newfd]->refcount++;

    return newfd;
}

/* dup(2) allocates the lowest available fd; it is not dup2(oldfd, 0).
   Overwriting fd 0 here breaks shells that save stdout before redirection. */
uint64_t sys_dup_impl(uint64_t oldfd) {
    process_t *p = get_current_process();
    if (!p || !p->fd_table || oldfd >= VFS_MAX_FD ||
        !p->fd_table->fds[oldfd]) return (uint64_t)-EBADF;
    for (int fd = 0; fd < VFS_MAX_FD; fd++) {
        if (!p->fd_table->fds[fd])
            return sys_dup2_impl(oldfd, (uint64_t)fd);
    }
    return (uint64_t)-EMFILE;
}

uint64_t sys_readlink_impl(uint64_t path, uint64_t buf, uint64_t bufsiz) {
    if (!path || !buf || bufsiz == 0) return (uint64_t)-EFAULT;
    if (!user_range_ok(path, 1) || !user_range_ok(buf, bufsiz)) return (uint64_t)-EFAULT;

    /* Resolve the parent, not the final component: ordinary VFS resolution
       follows symlinks, which made readlink() unable to see the link itself. */
    char name[256];
    vfs_node_t *parent = vfs_resolve_path_ext((const char *)path, true, name);
    if (!parent || !parent->ops || !parent->ops->lookup) return (uint64_t)-ENOENT;
    vfs_node_t *node = parent->ops->lookup(parent, name);
    if (!node) return (uint64_t)-ENOENT;
    if (node->type != VFS_TYPE_SYMLINK) return (uint64_t)-EINVAL;

    if (node->ops && node->ops->readlink) {
        int res = node->ops->readlink(node, (char *)buf, bufsiz);
        if (res < 0) return (uint64_t)-EINVAL;
        return (uint64_t)res;
    }

    return (uint64_t)-EINVAL;
}

uint64_t sys_getdents64_impl(uint64_t fd_raw, uint64_t dirp_raw, uint64_t count_raw) {
    int fd = (int)fd_raw;
    uint8_t *buf = (uint8_t *)dirp_raw;
    size_t count = (size_t)count_raw;

    if (!buf || count < sizeof(struct linux_dirent64)) return (uint64_t)-EINVAL;

    process_t *curr = get_current_process();
    vfs_file_t *file = curr->fd_table->fds[fd];
    if (!file || !file->node || file->node->type != VFS_TYPE_DIR) return (uint64_t)-EBADF;

    size_t total_bytes = 0;

    while (total_bytes < count) {
        vfs_dirent_t dent;
        memset(&dent, 0, sizeof(dent));

        int res = vfs_readdir(file, &dent);
        
        if (res < 0) break; 

        size_t name_len = strlen(dent.name);
        if (name_len == 0) continue;

        size_t reclen = (19 + name_len + 1 + 7) & ~7;

        if (total_bytes + reclen > count) {
            break;
        }

        struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + total_bytes);
        d->d_ino = dent.inode_num;
        d->d_off = (int64_t)file->offset;
        d->d_reclen = (uint16_t)reclen;

        uint8_t dt = DT_UNKNOWN;
        if (dent.type == VFS_TYPE_DIR) dt = DT_DIR;
        else if (dent.type == VFS_TYPE_FILE) dt = DT_REG;
        else if (dent.type == VFS_TYPE_CHAR_DEV) dt = DT_CHR;

        d->d_type = dt;
        
        memset(d->d_name, 0, reclen - 19); 
        strncpy(d->d_name, dent.name, name_len);
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

uint64_t sys_mkdirat_impl(uint64_t dirfd, uint64_t path_ptr, uint64_t mode) {
    char kpath[256];
    if (copy_str_from_user(kpath, path_ptr, sizeof(kpath)) != 0) {
        return (uint64_t)-EFAULT;
    }

    if ((int64_t)dirfd != -100 && kpath[0] != '/') return (uint64_t)-ENOSYS;
    int access = mutation_parent_access(kpath);
    if (access) return (uint64_t)(int64_t)access;
    int res = vfs_mkdir(kpath, (uint32_t)mode);
    if (res < 0) {
        return (uint64_t)res;
    }

    return 0;
}

uint64_t sys_mkdir_impl(uint64_t path_ptr, uint64_t mode) {
    return sys_mkdirat_impl(-100 /* AT_FDCWD */, path_ptr, mode);
}

uint64_t sys_renameat_impl(uint64_t oldfd, uint64_t old_ptr,
                           uint64_t newfd, uint64_t new_ptr) {
    char oldpath[512], newpath[512];
    if (copy_str_from_user(oldpath, old_ptr, sizeof(oldpath)) != 0 ||
        copy_str_from_user(newpath, new_ptr, sizeof(newpath)) != 0)
        return (uint64_t)-EFAULT;
    if (((int64_t)oldfd != -100 && oldpath[0] != '/') ||
        ((int64_t)newfd != -100 && newpath[0] != '/'))
        return (uint64_t)-ENOSYS;
    int access = mutation_parent_access(oldpath);
    if (!access) access = mutation_parent_access(newpath);
    if (access) return (uint64_t)(int64_t)access;
    return (uint64_t)(int64_t)vfs_rename(oldpath, newpath);
}

uint64_t sys_unlinkat_impl(uint64_t dirfd, uint64_t path_ptr, uint64_t flags) {
    char kpath[256];
    if (copy_str_from_user(kpath, path_ptr, sizeof(kpath)) != 0) {
        return (uint64_t)-EFAULT;
    }

    if ((int64_t)dirfd != -100 && kpath[0] != '/') return (uint64_t)-ENOSYS;
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
    return sys_unlinkat_impl(-100 /* AT_FDCWD */, path_ptr, 0);
}

uint64_t sys_rmdir_impl(uint64_t path_ptr) {
    return sys_unlinkat_impl(-100 /* AT_FDCWD */, path_ptr, 0x200 /* AT_REMOVEDIR */);
}

uint64_t sys_fchmodat_impl(uint64_t dirfd, uint64_t path_ptr, uint64_t mode, uint64_t flags) {
    char kpath[256];
    if (copy_str_from_user(kpath, path_ptr, sizeof(kpath)) != 0) {
        return (uint64_t)-EFAULT;
    }

    if ((int64_t)dirfd != -100 && kpath[0] != '/') return (uint64_t)-ENOSYS;
    if (flags) return (uint64_t)-ENOSYS;
    int access = mutation_owner_access(vfs_resolve_path(kpath));
    if (access) return (uint64_t)(int64_t)access;
    int res = vfs_chmod(kpath, (uint32_t)mode);
    if (res < 0) return (uint64_t)-ENOENT;
    
    return 0;
}

uint64_t sys_chmod_impl(uint64_t path_ptr, uint64_t mode) {
    return sys_fchmodat_impl(-100, path_ptr, mode, 0);
}

uint64_t sys_link_impl(uint64_t oldpath_ptr, uint64_t newpath_ptr) {
    char kold[256], knew[256];

    if (copy_str_from_user(kold, oldpath_ptr, sizeof(kold)) != 0)
        return (uint64_t)-EFAULT;

    if (copy_str_from_user(knew, newpath_ptr, sizeof(knew)) != 0)
        return (uint64_t)-EFAULT;

    int access = mutation_parent_access(knew);
    if (access) return (uint64_t)(int64_t)access;
    int res = vfs_link(kold, knew);
    if (res < 0) return (uint64_t)res;

    return 0;
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
    if (copy_str_from_user(path, filename, sizeof(path))) return (uint64_t)-EFAULT;
    if ((int64_t)dfd != -100 && path[0] != '/') return (uint64_t)-ENOSYS;
    /* access(2) uses real credentials; AT_EACCESS and nofollow need distinct
       handling, not a silent alias to the existing access approximation. */
    if (flags) return (uint64_t)-ENOSYS;
    return sys_access_impl((uint64_t)path, mode);
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
