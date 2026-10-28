#include "gconsole.h"
#include <syscall/syscall.h>
#include <stdint.h>
#include <io.h>
#include <desc/gdt.h>
#include <fs/vfs.h>
#include <scheduler/scheduler.h>
#include <errno.h>

#define IA32_STAR_MSR        0xC0000081
#define IA32_LSTAR_MSR       0xC0000082
#define IA32_FMASK_MSR       0xC0000084
#define IA32_EFER_MSR        0xC0000080
#define IA32_GS_BASE         0xC0000101
#define IA32_KERNEL_GS_BASE  0xC0000102
#define EFER_SCE             (1ULL << 0)
#define EFER_NXE (1ULL << 11)

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

extern void syscall_entry_stub(void);

extern uint64_t sys_read_impl(uint64_t, uint64_t, uint64_t);
extern uint64_t sys_write_impl(uint64_t, uint64_t, uint64_t);
extern uint64_t sys_open_impl(uint64_t, uint64_t, uint64_t);
extern uint64_t sys_close_impl(uint64_t);
extern uint64_t sys_ioctl_impl(uint64_t, uint64_t, uint64_t);
extern uint64_t sys_mount_impl(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
extern uint64_t sys_umount2_impl(uint64_t, uint64_t);
extern uint64_t sys_fsync_impl(uint64_t);
extern uint64_t sys_pread64_impl(uint64_t, uint64_t, uint64_t, uint64_t);
extern uint64_t sys_pwrite64_impl(uint64_t, uint64_t, uint64_t, uint64_t);
extern uint64_t sys_mmap_impl(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
extern uint64_t sys_writev_impl(uint64_t, uint64_t, uint64_t);
extern uint64_t sys_stat_impl(uint64_t, uint64_t);
extern uint64_t sys_fstat_impl(uint64_t, uint64_t);
extern uint64_t sys_newfstatat_impl(uint64_t, uint64_t, uint64_t, uint64_t);
extern uint64_t sys_statx_impl(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
extern uint64_t sys_poll_impl(uint64_t, uint64_t, uint64_t);
extern uint64_t sys_fcntl_impl(uint64_t, uint64_t, uint64_t);
extern uint64_t sys_lseek_impl(uint64_t, uint64_t, uint64_t);
extern uint64_t sys_access_impl(uint64_t, uint64_t);
extern uint64_t sys_mprotect_impl(uint64_t, uint64_t, uint64_t);
extern uint64_t sys_memfd_create_w(uint64_t, uint64_t);
extern uint64_t sys_ftruncate_w(uint64_t, uint64_t);
extern uint64_t sys_timerfd_create_w(uint64_t, uint64_t);
extern uint64_t sys_timerfd_settime_w(uint64_t, uint64_t, uint64_t, uint64_t);
extern uint64_t sys_timerfd_gettime_w(uint64_t, uint64_t);
extern uint64_t sys_signalfd4_w(uint64_t, uint64_t, uint64_t, uint64_t);
extern uint64_t sys_eventfd2_w(uint64_t, uint64_t);

extern uint64_t sys_execve_impl(uint64_t, uint64_t, uint64_t);
extern uint64_t sys_brk_impl(uint64_t);
extern uint64_t sys_arch_prctl_impl(uint64_t, uint64_t);
extern uint64_t sys_exit_impl(uint64_t);
extern uint64_t sys_getuid_impl(void);
extern uint64_t sys_geteuid_impl(void);
extern uint64_t sys_getgid_impl(void);
extern uint64_t sys_getegid_impl(void);
extern uint64_t sys_getpid_impl(void);
extern uint64_t sys_getppid_impl(void);
extern uint64_t sys_getpgid_impl(uint64_t);
extern uint64_t sys_setpgid_impl(uint64_t, uint64_t);
extern uint64_t sys_getsid_impl(uint64_t);
extern uint64_t sys_setsid_impl(void);
extern uint64_t sys_kill_impl(uint64_t, uint64_t);
extern uint64_t sys_rt_sigaction_impl(uint64_t, uint64_t, uint64_t, uint64_t);
extern uint64_t sys_rt_sigprocmask_impl(uint64_t, uint64_t, uint64_t, uint64_t);
extern uint64_t sys_getcwd_impl(uint64_t, uint64_t);
extern uint64_t sys_chdir_impl(uint64_t);
extern uint64_t sys_uname_impl(uint64_t);
extern uint64_t sys_getrandom_impl(uint64_t, uint64_t, uint64_t);
extern uint64_t sys_clone_impl(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
extern uint64_t sys_futex_impl(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

extern uint64_t sys_socket_impl(uint64_t, uint64_t, uint64_t);
extern uint64_t sys_connect_impl(uint64_t, uint64_t, uint64_t);
extern uint64_t sys_accept_impl(uint64_t, uint64_t, uint64_t);
extern uint64_t sys_send_impl(uint64_t, uint64_t, uint64_t, uint64_t);
extern uint64_t sys_recv_impl(uint64_t, uint64_t, uint64_t, uint64_t);
extern uint64_t sys_bind_impl(uint64_t, uint64_t, uint64_t);
extern uint64_t sys_listen_impl(uint64_t, uint64_t);
extern uint64_t sys_epoll_wait_impl(uint64_t, uint64_t, uint64_t, uint64_t);
extern uint64_t sys_epoll_ctl_impl(uint64_t, uint64_t, uint64_t, uint64_t);
extern uint64_t sys_epoll_create1_impl(uint64_t);
extern uint64_t sys_set_tid_address_impl(uint64_t tidptr);
extern uint64_t sys_munmap_impl(uint64_t, uint64_t);
extern uint64_t sys_prlimit64_impl(uint64_t pid, uint64_t resource, uint64_t new_limit, uint64_t old_limit);
extern uint64_t sys_gettimeofday_impl(uint64_t, uint64_t);
extern uint64_t sys_getresuid_impl(uint64_t, uint64_t, uint64_t);
extern uint64_t sys_getresgid_impl(uint64_t, uint64_t, uint64_t);
extern uint64_t sys_pselect6_impl(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

extern uint64_t sys_dup2_impl(uint64_t, uint64_t);
extern uint64_t sys_readlink_impl(uint64_t, uint64_t, uint64_t);
extern uint64_t sys_getdents64_impl(uint64_t, uint64_t, uint64_t);
extern uint64_t sys_wait4_impl(uint64_t, uint64_t, uint64_t, uint64_t);
extern uint64_t sys_time_impl(uint64_t);
extern uint64_t sys_rseq_impl(uint64_t, uint64_t, uint64_t, uint64_t);
extern uint64_t sys_clock_gettime_impl(uint64_t, uint64_t);
extern uint64_t sys_readv_impl(uint64_t, uint64_t, uint64_t);
extern uint64_t sys_access_impl(uint64_t, uint64_t);
extern uint64_t sys_setuid_impl(uint64_t);
extern uint64_t sys_setgid_impl(uint64_t);
extern uint64_t sys_pipe2_impl(uint64_t, uint64_t);
extern uint64_t sys_umask_impl(uint64_t);

extern uint64_t sys_mkdir_impl(uint64_t, uint64_t);
extern uint64_t sys_mkdirat_impl(uint64_t, uint64_t, uint64_t);
extern uint64_t sys_unlink_impl(uint64_t);
extern uint64_t sys_unlinkat_impl(uint64_t, uint64_t, uint64_t);
extern uint64_t sys_chmod_impl(uint64_t, uint64_t);
extern uint64_t sys_fchmodat_impl(uint64_t, uint64_t, uint64_t, uint64_t);
extern uint64_t sys_sysinfo_impl(uint64_t);
extern uint64_t sys_getrusage_impl(uint64_t, uint64_t);
extern uint64_t sys_rmdir_impl(uint64_t);
extern uint64_t sys_rt_sigsuspend_impl(uint64_t);
extern uint64_t sys_nanosleep_impl(uint64_t, uint64_t);
extern uint64_t sys_alarm_impl(uint64_t);
extern uint64_t sys_getitimer_impl(uint64_t, uint64_t);
extern uint64_t sys_setitimer_impl(uint64_t, uint64_t, uint64_t);
extern uint64_t sys_link_impl(uint64_t, uint64_t);
extern uint64_t sys_fchmod_impl(uint64_t, uint64_t);

extern uint64_t sys_socket_impl(uint64_t, uint64_t, uint64_t);
extern uint64_t sys_bind_impl(uint64_t, uint64_t, uint64_t);
extern uint64_t sys_listen_impl(uint64_t, uint64_t);
extern uint64_t sys_accept_impl(uint64_t, uint64_t, uint64_t);
extern uint64_t sys_getsockname_impl(uint64_t, uint64_t, uint64_t);
extern uint64_t sys_getsockopt_impl(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
extern uint64_t sys_setsockopt_impl(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

extern uint64_t sys_clock_getres_impl(uint64_t, uint64_t);
extern uint64_t sys_getresuid(uint32_t*, uint32_t*, uint32_t*);
extern uint64_t sys_getresgid(uint32_t*, uint32_t*, uint32_t*);
extern uint64_t sys_prctl_impl(uint64_t option, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);

uint64_t sys_faccessat2_impl(uint64_t dfd, uint64_t filename, uint64_t mode, uint64_t flags);
uint64_t sys_fstatfs_impl(uint64_t fd, uint64_t buf);
uint64_t sys_reboot_impl(uint64_t magic, uint64_t magic2, uint64_t cmd, uint64_t arg);
uint64_t sys_clock_nanosleep_impl(uint64_t clock_id, uint64_t flags, uint64_t req_ptr, uint64_t rem_ptr);

uint64_t sys_getresuid2_impl(uint64_t ruid, uint64_t euid, uint64_t suid) {
    return sys_getresuid((uint32_t *)ruid, (uint32_t *)euid, (uint32_t *)suid);
}

uint64_t sys_getresgid2_impl(uint64_t rgid, uint64_t egid, uint64_t sgid) {
    return sys_getresgid((uint32_t *)rgid, (uint32_t *)egid, (uint32_t *)sgid);
}

extern uint64_t sys_setfsuid_impl(uint64_t);
extern uint64_t sys_setfsgid_impl(uint64_t);

#define SIGCHLD 17

uint64_t sys_fork_impl(void) {
    return sys_clone_impl(SIGCHLD, 0, 0, 0, 0);
}

void syscall_save_frame(struct interrupt_frame *frame) {
    thread_t *thr = get_current_thread();
    if (thr && thr->regs && frame) {
        *thr->regs = *frame;
    }
}

void syscall_init(void) {
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 &= ~(1ULL << 21); // Сбрасываем 21-й бит
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));

    uint64_t efer = rdmsr(IA32_EFER_MSR);
    wrmsr(IA32_EFER_MSR, efer | EFER_SCE | EFER_NXE);
    
    wrmsr(IA32_EFER_MSR, efer | EFER_SCE);

    uint64_t star = ((uint64_t)0x10 << 48) | ((uint64_t)KERNEL_CS << 32);
    wrmsr(IA32_STAR_MSR, star);

    wrmsr(IA32_LSTAR_MSR, (uint64_t)syscall_entry_stub);
    wrmsr(IA32_FMASK_MSR, 0x200 | 0x400);

    wrmsr(IA32_GS_BASE, (uint64_t)get_current_local_data());
    wrmsr(IA32_KERNEL_GS_BASE, 0);
}

uint64_t osdev_syscall_handler(uint64_t num,
                         uint64_t a1, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    // console_print_string("[SYSCALL] num=");
    // console_print_num(num);
    // console_print_string("\n");

    switch (num) {
    case SYS_READ:        return sys_read_impl(a1, a2, a3);
    case SYS_WRITE:       return sys_write_impl(a1, a2, a3);
    case SYS_OPEN:        return sys_open_impl(a1, a2, a3);
    case SYS_CLOSE:       return sys_close_impl(a1);
    case SYS_STAT:        return sys_stat_impl(a1, a2);
    case SYS_FSTAT:       return sys_fstat_impl(a1, a2);
    case SYS_LSTAT:       return sys_stat_impl(a1, a2);
    case SYS_POLL:        return sys_poll_impl(a1, a2, a3);
    case SYS_LSEEK:       return sys_lseek_impl(a1, a2, a3);
    case SYS_MMAP:        return sys_mmap_impl(a1, a2, a3, a4, a5, a6);
    case SYS_MPROTECT:    return sys_mprotect_impl(a1, a2, a3);
    case SYS_IOCTL:       return sys_ioctl_impl(a1, a2, a3);
    case SYS_MOUNT:       return sys_mount_impl(a1, a2, a3, a4, a5);
    case SYS_UMOUNT2:     return sys_umount2_impl(a1, a2);
    case SYS_FSYNC:       return sys_fsync_impl(a1);
    case SYS_FDATASYNC:   return sys_fsync_impl(a1);
    case SYS_PREAD64:     return sys_pread64_impl(a1, a2, a3, a4);
    case SYS_PWRITE64:    return sys_pwrite64_impl(a1, a2, a3, a4);
    case SYS_WRITEV:      return sys_writev_impl(a1, a2, a3);
    case SYS_ACCESS:      return sys_access_impl(a1, a2);
    case SYS_FCNTL:       return sys_fcntl_impl(a1, a2, a3);
    case SYS_FTRUNCATE:   return sys_ftruncate_w(a1, a2);
    case SYS_MEMFD_CREATE: return sys_memfd_create_w(a1, a2);
    case SYS_OPENAT: {
        process_t *p = get_current_process();
        if (!p || !a2) return (uint64_t)-EFAULT;
        uint32_t flags = translate_linux_open_flags((uint32_t)a3);
        return sys_open(p->fd_table, (const char *)a2, (uint32_t)flags);
    }
    case SYS_NEWFSTATAT:  return sys_newfstatat_impl(a1, a2, a3, a4);
    case SYS_STATX:       return sys_statx_impl(a1, a2, a3, a4, a5);
    case SYS_FACCESSAT:   return sys_access_impl(a2, a3);
    case SYS_GETCWD:      return sys_getcwd_impl(a1, a2);
    case SYS_CHDIR:       return sys_chdir_impl(a1);

    case SYS_BRK:         return sys_brk_impl(a1);
    case SYS_EXECVE:      return sys_execve_impl(a1, a2, a3);
    case SYS_EXIT:        return sys_exit_impl(a1);
    case SYS_EXIT_GROUP:  return sys_exit_impl(a1);
    case SYS_CLONE:       return sys_clone_impl(a1, a2, a3, a4, a5);
    case SYS_FUTEX:       return sys_futex_impl(a1, a2, a3, a4, a5, a6);
    case SYS_KILL:        return sys_kill_impl(a1, a2);
    case SYS_TKILL:       return sys_kill_impl(a1, a2);
    case SYS_TGKILL:      return sys_kill_impl(a1, a2);
    case SYS_RT_SIGACTION:   return sys_rt_sigaction_impl(a1, a2, a3, a4);
    case SYS_RT_SIGPROCMASK: return sys_rt_sigprocmask_impl(a1, a2, a3, a4);

    case SYS_GETPID:      return sys_getpid_impl();
    case SYS_GETPPID:     return sys_getppid_impl();
    case SYS_GETPGID:     return sys_getpgid_impl(a1);
    case SYS_SETPGID:     return sys_setpgid_impl(a1, a2);
    case SYS_GETSID:      return sys_getsid_impl(a1);
    case SYS_SETSID:      return sys_setsid_impl();
    case SYS_GETTID: {
        thread_t *thr = get_current_thread();
        return (thr && thr->tid != 0) ? thr->tid : sys_getpid_impl();
    }

    case SYS_GETUID:      return sys_getuid_impl();
    case SYS_GETEUID:     return sys_geteuid_impl();
    case SYS_GETGID:      return sys_getgid_impl();
    case SYS_GETEGID:     return sys_getegid_impl();

    case SYS_ARCH_PRCTL:      return sys_arch_prctl_impl(a1, a2);
    case SYS_UNAME:           return sys_uname_impl(a1);
    case SYS_SET_TID_ADDRESS: return sys_set_tid_address_impl(a1);

    case SYS_TIMERFD_CREATE:  return sys_timerfd_create_w(a1, a2);
    case SYS_TIMERFD_SETTIME: return sys_timerfd_settime_w(a1, a2, a3, a4);
    case SYS_TIMERFD_GETTIME: return sys_timerfd_gettime_w(a1, a2);
    case SYS_SIGNALFD:        return sys_signalfd4_w(a1, a2, a3, a4);
    case SYS_SIGNALFD4:       return sys_signalfd4_w(a1, a2, a3, a4);
    case SYS_EVENTFD:         return sys_eventfd2_w(a1, a2);
    case SYS_EVENTFD2:        return sys_eventfd2_w(a1, a2);

    case SYS_EPOLL_WAIT:  return sys_epoll_wait_impl(a1, a2, a3, a4);
    case SYS_EPOLL_CTL:   return sys_epoll_ctl_impl(a1, a2, a3, a4);
    case SYS_EPOLL_CREATE1: return sys_epoll_create1_impl(a1);
    case SYS_GETTIMEOFDAY:  return sys_gettimeofday_impl(a1, a2);
    case 118: return sys_getresuid_impl(a1, a2, a3);
    case 120: return sys_getresgid_impl(a1, a2, a3);
    case SYS_PIPE: return sys_pipe2_impl(a1, 0);
    case SYS_PIPE2: return sys_pipe2_impl(a1, a2);

    case SYS_SET_ROBUST_LIST: return 0;
    case 318:                 return sys_getrandom_impl(a1, a2, a3);
    case 435: {
        if (!a1 || a2 < 64) return (uint64_t)-EINVAL;
        uint64_t *ca = (uint64_t *)a1;
        uint64_t flags      = ca[0];
        uint64_t ptid       = ca[3];
        uint64_t ctid       = ca[2];
        uint64_t stack      = ca[5];
        uint64_t stack_size = ca[6];
        uint64_t tls        = ca[7];
        uint64_t child_stack = stack ? (stack + stack_size) : 0;
        return sys_clone_impl(flags, child_stack, ptid, ctid, tls);
    }
    case SYS_MUNMAP: return sys_munmap_impl(a1, a2);
    case SYS_PRLIMIT64:       return sys_prlimit64_impl(a1, a2, a3, a4);
    case SYS_GETPGRP: return sys_getpgid_impl(0);
    case SYS_PSELECT6: return sys_pselect6_impl(a1, a2, a3, a4, a5, a6);
    case SYS_DUP:  return sys_dup2_impl(a1, a2);
    case SYS_WAIT4:  return sys_wait4_impl(a1, a2, a3, a4);
    case SYS_READLINK:  return sys_readlink_impl(a1, a2, a3);
    case SYS_TIME: return sys_time_impl(a1);
    case SYS_GETDENTS64: return sys_getdents64_impl(a1, a2, a3);
    case 334: return sys_rseq_impl(a1, a2, a3, a4);
    case SYS_FORK: return sys_fork_impl();
    case SYS_CLOCK_GETTIME: return sys_clock_gettime_impl(a1, a2);
    case SYS_READV: return sys_readv_impl(a1, a2, a3);
    case SYS_DUP2: return sys_dup2_impl(a1, a2);
    case SYS_SETUID: return sys_setuid_impl(a1);
    case SYS_SETGID: return sys_setgid_impl(a1);
    case SYS_UMASK: return sys_umask_impl(a1);
    case 83:  return sys_mkdir_impl(a1, a2);
    case 87:  return sys_unlink_impl(a1);
    case 90:  return sys_chmod_impl(a1, a2);
    case 98:  return sys_getrusage_impl(a1, a2);
    case 99:  return sys_sysinfo_impl(a1);
    case 258: return sys_mkdirat_impl(a1, a2, a3);
    case 263: return sys_unlinkat_impl(a1, a2, a3);
    case 268: return sys_fchmodat_impl(a1, a2, a3, a4);
    case SYS_RMDIR: return sys_rmdir_impl(a1);
    case 130: return sys_rt_sigsuspend_impl(a1);
    case SYS_NANOSLEEP: return sys_nanosleep_impl(a1, a2);
    case SYS_GETITIMER: return sys_getitimer_impl(a1, a2);
    case SYS_ALARM: return sys_alarm_impl(a1);
    case SYS_SETITIMER: return sys_setitimer_impl(a1, a2, a3);
    case 86: return sys_link_impl(a1, a2);   /* SYS_LINK */
    case 91: return sys_fchmod_impl(a1, a2); /* SYS_FCHMOD */
    case 41: return sys_socket_impl(a1, a2, a3);      /* SYS_SOCKET */
    case 43: return sys_accept_impl(a1, a2, a3);      /* SYS_ACCEPT */
    case 49: return sys_bind_impl(a1, a2, a3);        /* SYS_BIND */
    case 50: return sys_listen_impl(a1, a2);          /* SYS_LISTEN */
    case 51: return sys_getsockname_impl(a1, a2, a3);  /* SYS_GETSOCKNAME */
    case 55: return sys_getsockopt_impl(a1, a2, a3, a4, a5); /* SYS_GETSOCKOPT */
    case 54: return sys_setsockopt_impl(a1, a2, a3, a4, a5); /* SYS_SETSOCKOPT */
    case 42: return sys_connect_impl(a1, a2, a3); /* SYS_CONNECT */
    case 229: return sys_clock_getres_impl(a1, a2); /* SYS_CLOCK_GETRES */
    case 122: return sys_setfsuid_impl(a1);
    case 123: return sys_setfsgid_impl(a1);
    case 157: return sys_prctl_impl(a1, a2, a3, a4, a5);
    case 230: return sys_clock_nanosleep_impl(a1, a2, a3, a4);
    case 439: return sys_faccessat2_impl(a1, a2, a3, a4);
    case 221: return sys_fstatfs_impl(a1, a2);
    case 138: return sys_reboot_impl(a1, a2, a3, a4);
    case 92:  return 0; // sys_truncate

    default:
        console_print_string("[SYSCALL] ENOSYS num=");
        console_print_num(num);
        console_print_string("\n");
        // while (1) {
        //     __asm__ volatile ("sti; hlt");
        // }
        return (uint64_t)-ENOSYS;
    }
}

uint64_t syscall_handler(uint64_t num,
                         uint64_t a1, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    uint64_t ret = osdev_syscall_handler(num, a1, a2, a3, a4, a5, a6);
    // if (dbg_cnt < 100) {
    //     console_print_string("[S] ");
    //     console_print_num(num);
    //     console_print_string(" a1="); console_print_hex(a1);
    //     console_print_string(" a2="); console_print_hex(a2);
    //     console_print_string(" a3="); console_print_hex(a3);
    //     console_print_string(" ret="); console_print_hex(ret);
    //     console_print_string("\n");
    //     dbg_cnt++;
    // }
    return ret;
}