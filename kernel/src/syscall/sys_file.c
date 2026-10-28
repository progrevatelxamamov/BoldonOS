#include "gconsole.h"
#include "io.h"
#include "mm/malloc.h"
#include <cpu/time.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <fs/vfs.h>
#include <fs/mount.h>
#include <disk/block.h>
#include <disk/bcache.h>
#include <scheduler/scheduler.h>
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

    size_t i = 0;
    for (; i < max_len - 1; i++) {
        if (!user_range_ok(src_user_ptr + i, 1)) return -EFAULT;
        char c = *(const char *)(src_user_ptr + i);
        dst[i] = c;
        if (c == '\0') return 0;
    }
    dst[i] = '\0';
    return 0;
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
    if (nfds == 0) return 0;
    if (!user_range_ok(fds_ptr, nfds * sizeof(struct pollfd)))
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
            if (fd < 0 || fd >= VFS_MAX_FD || !p->fd_table->fds[fd]) continue;

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
        if (timeout == 0) return 0;
        if (timeout != (uint64_t)-1 && get_monotonic_ms() - start >= timeout)
            return 0;
        schedule_yield();
    }
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
    (void)dirfd; (void)flags;
    if (!path || !statbuf) return (uint64_t)-EFAULT;
    vfs_node_t *node = vfs_resolve_path((const char *)path);
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
    return (uint64_t)(int64_t)sys_write(p->fd_table, (int)a1, (const void *)a2, a3);
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

uint64_t sys_open_impl(uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)a3;
    process_t *p = get_current_process();
    if (!p || !p->fd_table) return (uint64_t)-EBADF;
    if (!a1) return (uint64_t)-EFAULT;
    return (uint64_t)(int64_t)sys_open(p->fd_table, (const char *)a1, translate_linux_open_flags((uint32_t)a2));
}

uint64_t sys_close_impl(uint64_t a1) {
    process_t *p = get_current_process();
    if (!p || !p->fd_table) return (uint64_t)-EBADF;
    return (uint64_t)(int64_t)sys_close(p->fd_table, (int)a1);
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

uint64_t sys_ioctl_impl(uint64_t a1, uint64_t a2, uint64_t a3) {
    process_t *p = get_current_process();
    if (!p || !p->fd_table) return (uint64_t)-EBADF;

    int fd = (int)a1;
    if (fd < 0 || fd >= VFS_MAX_FD || !p->fd_table->fds[fd])
        return (uint64_t)-EBADF;

    vfs_file_t *file = p->fd_table->fds[fd];

    if (file && file->node && file->node->type == VFS_TYPE_CHAR_DEV && a2 == 0x4B3A) {
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
        case 0x4B3A: /* KDSETMODE */
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

uint64_t sys_readlink_impl(uint64_t path, uint64_t buf, uint64_t bufsiz) {
    if (!path || !buf || bufsiz == 0) return (uint64_t)-EFAULT;
    if (!user_range_ok(path, 1) || !user_range_ok(buf, bufsiz)) return (uint64_t)-EFAULT;

    vfs_node_t *node = vfs_resolve_path((const char *)path);
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

uint64_t sys_mkdirat_impl(uint64_t dirfd, uint64_t path_ptr, uint64_t mode) {
    (void)dirfd;

    char kpath[256];
    if (copy_str_from_user(kpath, path_ptr, sizeof(kpath)) != 0) {
        return (uint64_t)-EFAULT;
    }

    int res = vfs_mkdir(kpath, (uint32_t)mode);
    if (res < 0) {
        return (uint64_t)res;
    }

    return 0;
}

uint64_t sys_mkdir_impl(uint64_t path_ptr, uint64_t mode) {
    return sys_mkdirat_impl(-100 /* AT_FDCWD */, path_ptr, mode);
}

uint64_t sys_unlinkat_impl(uint64_t dirfd, uint64_t path_ptr, uint64_t flags) {
    (void)dirfd;

    char kpath[256];
    if (copy_str_from_user(kpath, path_ptr, sizeof(kpath)) != 0) {
        return (uint64_t)-EFAULT;
    }

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
    (void)dirfd; (void)flags;
    
    char kpath[256];
    if (copy_str_from_user(kpath, path_ptr, sizeof(kpath)) != 0) {
        return (uint64_t)-EFAULT;
    }

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

    int res = vfs_link(kold, knew);
    if (res < 0) return (uint64_t)res;

    return 0;
}

uint64_t sys_fchmod_impl(uint64_t fd, uint64_t mode) {
    process_t *p = get_current_process();
    if (!p || !p->fd_table || fd >= VFS_MAX_FD || !p->fd_table->fds[fd])
        return (uint64_t)-EBADF;

    vfs_file_t *file = p->fd_table->fds[fd];
    int res = vfs_fchmod(file, (uint32_t)mode);
    if (res < 0) return (uint64_t)res;

    return 0;
}

uint64_t sys_faccessat2_impl(uint64_t dfd, uint64_t filename, uint64_t mode, uint64_t flags) {
    (void)dfd; (void)flags;
    return sys_access_impl(filename, mode);
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
    outb(0x64, 0xFE);
    return 0;
}
uint64_t sys_mount_impl(uint64_t source, uint64_t target, uint64_t fstype,
                        uint64_t flags, uint64_t data) {
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
        if (block_devices[i].sector_count > 0 && block_devices[i].name[0])
            bcache_flush(block_devices[i].name);
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
    vfs_file_t *file = p->fd_table->fds[fd];
    uint64_t saved = file->offset;
    file->offset = offset;
    int res = vfs_write(file, (const void *)buf, count);
    file->offset = saved;
    return (uint64_t)(int64_t)res;
}
