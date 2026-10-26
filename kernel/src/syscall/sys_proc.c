#include "gconsole.h"
#include "kernel.h"
#include "mm/pmm.h"
#include "mm/malloc.h"
#include "mm/vmm.h"
#include <desc/gdt.h>
#include <stdint.h>
#include <sys/elf.h>
#include <scheduler/scheduler.h>
#include <io.h>
#include <memory.h>
#include <errno.h>
#include <cpu/time.h>

#define IA32_FS_BASE 0xC0000100

#define CLONE_VM              0x00000100
#define CLONE_FS              0x00000200
#define CLONE_FILES           0x00000400
#define CLONE_SIGHAND         0x00000800
#define CLONE_THREAD          0x00010000
#define CLONE_SETTLS          0x00080000
#define CLONE_PARENT_SETTID   0x00100000
#define CLONE_CHILD_CLEARTID  0x00200000
#define CLONE_CHILD_SETTID    0x01000000

#define FUTEX_WAIT           0
#define FUTEX_WAKE           1
#define FUTEX_PRIVATE_FLAG   128
#define FUTEX_CLOCK_REALTIME 256

#define KERNEL_STACK_SIZE 16384
static uint64_t global_tid_counter = 1000;

struct linux_rlimit {
    uint64_t rlim_cur;
    uint64_t rlim_max;
};

struct new_utsname {
    char sysname[65], nodename[65], release[65];
    char version[65], machine[65], domainname[65];
};

struct k_sigaction {
    void (*handler)(int);
    uint64_t flags;
    void (*restorer)(void);
    uint64_t mask;
};

typedef struct futex_waiter {
    uint64_t addr;
    uint64_t tid;
    process_t *proc;
    struct futex_waiter *next;
} futex_waiter_t;

struct timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

struct linux_sysinfo {
    long uptime;
    unsigned long loads[3];
    unsigned long totalram;
    unsigned long freeram;
    unsigned long sharedram;
    unsigned long bufferram;
    unsigned long totalswap;
    unsigned long freeswap;
    unsigned short procs;
    unsigned short pad;
    unsigned long totalhigh;
    unsigned long freehigh;
    unsigned int mem_unit;
    char _f[20-2*sizeof(long)-sizeof(int)];
};

static futex_waiter_t *futex_queue = NULL;

uint64_t sys_getuid_impl(void)  { process_t *p = get_current_process(); return p ? p->uid  : 0; }
uint64_t sys_geteuid_impl(void) { process_t *p = get_current_process(); return p ? p->euid : 0; }
uint64_t sys_getgid_impl(void)  { process_t *p = get_current_process(); return p ? p->gid  : 0; }
uint64_t sys_getegid_impl(void) { process_t *p = get_current_process(); return p ? p->egid : 0; }

uint64_t sys_getrandom_impl(uint64_t buf, uint64_t count, uint64_t flags) {
    (void)flags;
    if (!buf) return (uint64_t)-EFAULT;
    uint8_t *p = (uint8_t *)buf;
    for (uint64_t i = 0; i < count; i++) p[i] = (uint8_t)(i * 31 + 17);
    return count;
}

uint64_t sys_rt_sigaction_impl(uint64_t sig, uint64_t act,
                               uint64_t oldact, uint64_t sigsetsize) {
    (void)sigsetsize;
    if (sig >= 64) return (uint64_t)-EINVAL;
    process_t *p = get_current_process();
    if (!p) return (uint64_t)-ESRCH;

    if (oldact && p->sigactions[sig])
        memcpy((void *)oldact, p->sigactions[sig], sizeof(struct k_sigaction));

    if (act) {
        if (!p->sigactions[sig])
            p->sigactions[sig] = malloc_alloc(sizeof(struct k_sigaction));
        if (!p->sigactions[sig]) return (uint64_t)-ENOMEM;
        memcpy(p->sigactions[sig], (const void *)act, sizeof(struct k_sigaction));
    }
    return 0;
}

uint64_t sys_rt_sigprocmask_impl(uint64_t how, uint64_t set,
                                 uint64_t oldset, uint64_t sigsetsize) {
    (void)sigsetsize;
    process_t *p = get_current_process();
    if (!p) return (uint64_t)-ESRCH;

    if (oldset) *(uint64_t *)oldset = p->sigmask;
    if (set) {
        uint64_t nm = *(uint64_t *)set;
        if (how == 0) p->sigmask |= nm;
        else if (how == 1) p->sigmask &= ~nm;
        else if (how == 2) p->sigmask = nm;
    }
    return 0;
}

uint64_t sys_kill_impl(uint64_t pid, uint64_t sig) {
    process_t *p = get_current_process();
    if (!p) return (uint64_t)-ESRCH;
    if (sig == 0) return 0;

    if (pid == p->pid || pid == 0) {
        if (sig == 9 || sig == 15) {
            for (int i = 0; i < 256; i++) {
                if (p->threads[i]) {
                    p->threads[i]->state = TASK_DEAD;
                }
            }
            p->state = TASK_DEAD;
            schedule_yield();
        }
    }
    return 0;
}

uint64_t sys_getpid_impl(void)   { process_t *p = get_current_process(); return p ? p->pid : 1; }
uint64_t sys_getppid_impl(void)  { process_t *p = get_current_process(); return (p && p->parent) ? p->parent->pid : 1; }
uint64_t sys_getpgid_impl(uint64_t pid) { (void)pid; process_t *p = get_current_process(); return p ? p->pgid : 0; }
uint64_t sys_setpgid_impl(uint64_t pid, uint64_t pgid) {
    (void)pid;
    process_t *p = get_current_process();
    if (!p) return (uint64_t)-ESRCH;
    p->pgid = (pgid == 0) ? p->pid : pgid;
    return 0;
}
uint64_t sys_getsid_impl(uint64_t pid) { (void)pid; process_t *p = get_current_process(); return p ? p->sid : 1; }
uint64_t sys_setsid_impl(void) {
    process_t *p = get_current_process();
    if (!p) return (uint64_t)-ESRCH;
    p->sid = p->pid;
    p->pgid = p->pid;
    return p->sid;
}

uint64_t sys_getcwd_impl(uint64_t buf, uint64_t size) {
    if (!buf || size == 0) return (uint64_t)-EINVAL;
    process_t *p = get_current_process();
    const char *cwd = (p && p->cwd[0]) ? p->cwd : "/";
    size_t len = 0; while (cwd[len]) len++;
    if (len + 1 > size) return (uint64_t)-ERANGE;
    memcpy((void *)buf, cwd, len + 1);
    return buf;
}

uint64_t sys_chdir_impl(uint64_t path) {
    if (!path) return (uint64_t)-EFAULT;
    vfs_node_t *node = vfs_resolve_path((const char *)path);
    if (!node) return (uint64_t)-ENOENT;
    if (node->type != VFS_TYPE_DIR) return (uint64_t)-ENOTDIR;

    process_t *p = get_current_process();
    if (p) {
        const char *s = (const char *)path;
        size_t len = 0;
        while (s[len] && len < 255) { p->cwd[len] = s[len]; len++; }
        p->cwd[len] = 0;
    }
    return 0;
}

uint64_t sys_execve_impl(uint64_t a1, uint64_t a2, uint64_t a3) {
    if (!a1) return (uint64_t)-EFAULT;
    return (uint64_t)(int64_t)sys_execve((const char *)a1,
                                         (char *const *)a2,
                                         (char *const *)a3);
}

uint64_t sys_brk_impl(uint64_t brk) {
    process_t *p = get_current_process();
    if (!p) return (uint64_t)-ENOMEM;
    if (!p->pml4) return (uint64_t)-ENOMEM;

    if (brk == 0 || brk < p->brk_start) return p->brk_current;

    uint64_t old_page = (p->brk_current + 4095) & ~4095ULL;
    uint64_t new_page = (brk + 4095) & ~4095ULL;

    for (uint64_t addr = old_page; addr < new_page; addr += 4096) {
        if (!vmm_is_mapped(p->pml4, addr)) {
            void *phys = pmm_alloc_page_and_clear();
            if (!phys) return p->brk_current;
            vmm_map_page(p->pml4, (uint64_t)phys, addr,
                         PTE_PRESENT | PTE_WRITABLE | PTE_USER);
        }
    }

    p->brk_current = brk;
    return p->brk_current;
}

static inline bool user_range_ok(uint64_t ptr, uint64_t len) {
    if (ptr == 0) return len == 0;
    if (ptr < 0x10000) return false;
    uint64_t end;
    if (__builtin_add_overflow(ptr, len, &end)) return false;
    return end <= 0x00007FFFFFFFFFFFULL;
}

#define IA32_FS_BASE 0xC0000100

uint64_t sys_arch_prctl_impl(uint64_t code, uint64_t addr) {
    process_t *p = get_current_process();
    thread_t *thr = get_current_thread();
    if (!p || !thr) return (uint64_t)-ESRCH;

    if (code == 0x1002) { /* ARCH_SET_FS */
        uint64_t base = addr & ~0xFFFULL;
        for (int i = -2; i <= 4; i++) {
            int64_t page = (int64_t)base + (int64_t)i * 4096;
            if (page < 0x10000) continue;
            if ((uint64_t)page >= 0x00007FFFFFFFFFFFULL) continue;
            if (!vmm_is_mapped(p->pml4, (uint64_t)page)) {
                void *phys = pmm_alloc_page_and_clear();
                if (phys)
                    vmm_map_page(p->pml4, (uint64_t)phys, (uint64_t)page,
                                 PTE_PRESENT | PTE_WRITABLE | PTE_USER);
            }
        }
        thr->fs_base = addr;
        wrmsr(IA32_FS_BASE, addr);
        return 0;
    }
    if (code == 0x1003) { /* ARCH_GET_FS */
        if (!addr || !user_range_ok(addr, 8)) return (uint64_t)-EFAULT;
        *(uint64_t *)addr = thr->fs_base;
        return 0;
    }
    return (uint64_t)-EINVAL;
}

uint64_t sys_set_tid_address_impl(uint64_t tidptr) {
    process_t *p = get_current_process();
    thread_t *thr = get_current_thread();
    if (thr) thr->ctid = tidptr;
    uint64_t tid = (thr && thr->tid != 0) ? thr->tid : (p && p->pid != 0 ? p->pid : 1);
    return tid;
}

uint64_t sys_prlimit64_impl(uint64_t pid, uint64_t resource,
                            uint64_t new_limit, uint64_t old_limit) {
    (void)pid; (void)resource; (void)new_limit;
    if (old_limit) {
        if (!user_range_ok(old_limit, sizeof(struct linux_rlimit))) return (uint64_t)-EFAULT;
        struct linux_rlimit *rl = (struct linux_rlimit *)old_limit;
        rl->rlim_cur = 8 * 1024 * 1024; /* 8MB stack limit */
        rl->rlim_max = 8 * 1024 * 1024;
    }
    return 0;
}

extern process_t *tasks[];

uint64_t sys_exit_impl(uint64_t status) {
    process_t *p = get_current_process();
    thread_t *thr = get_current_thread();

    if (thr) {
        thr->state = TASK_DEAD;
    }

    if (p) {
        p->exit_code = (int)status;
        p->state = TASK_ZOMBIE;

        for (int i = 1; i < 512; i++) {
            if (tasks[i] && tasks[i]->parent == p) {
                tasks[i]->parent = NULL;
            }
        }
    }

    schedule_yield();
    for (;;) schedule_yield();
}

uint64_t sys_uname_impl(uint64_t a1) {
    if (!a1) return (uint64_t)-EFAULT;
    struct new_utsname *u = (struct new_utsname *)a1;
    memset(u, 0, sizeof(*u));
    memcpy(u->sysname,  "Linux", 6);
    memcpy(u->nodename, "boldon", 7);
    memcpy(u->release,  "6.1.0", 6);
    memcpy(u->version,  "#1 BoldonOS", 12);
    memcpy(u->machine,  "x86_64", 7);
    return 0;
}

#define CLONE_VM     0x00000100
#define CLONE_THREAD 0x00010000
#define CLONE_SETTLS 0x00080000

extern page_table_t *vmm_clone_pml4(page_table_t *src_pml4);

static char *kstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = 0; while (s[n]) n++; n++;
    char *out = malloc_alloc(n);
    if (!out) return NULL;
    memcpy(out, s, n);
    return out;
}

uint64_t sys_clone_impl(uint64_t flags, uint64_t child_stack,
                        uint64_t ptid, uint64_t ctid, uint64_t tls) {
    process_t *p = get_current_process();
    thread_t *curr_thr = get_current_thread();
    if (!p || !curr_thr) return (uint64_t)-EINVAL;

    if (flags & CLONE_THREAD) {
        int slot = -1;
        for (int i = 1; i < 256; i++) {
            if (p->threads[i] == NULL) { slot = i; break; }
        }
        if (slot == -1) return (uint64_t)-EAGAIN;

        thread_t *t = malloc_alloc(sizeof(thread_t));
        struct interrupt_frame *regs = malloc_alloc(sizeof(struct interrupt_frame));
        struct interrupt_frame *kframe = malloc_alloc(sizeof(struct interrupt_frame));
        void *kstack = malloc_alloc(KERNEL_STACK_SIZE);
        if (!t || !regs || !kframe || !kstack) {
            if (t) malloc_free(t);
            if (regs) malloc_free(regs);
            if (kframe) malloc_free(kframe);
            if (kstack) malloc_free(kstack);
            return (uint64_t)-ENOMEM;
        }

        memset(t, 0, sizeof(thread_t));
        memset(kframe, 0, sizeof(struct interrupt_frame));
        *regs = *curr_thr->regs;
        regs->rax = 0;
        if (child_stack != 0) regs->rsp = child_stack;

        t->regs = regs;
        t->syscall_frame = kframe;
        t->kernel_stack = kstack;
        t->kernel_stack_top = (uint64_t)kstack + KERNEL_STACK_SIZE;
        *(uint64_t *)kstack = 0xDEAD10CCDEAD10CCULL;
        t->state = TASK_WAITING;
        t->is_kernel = false;
        t->fs_base = (flags & CLONE_SETTLS) ? tls : curr_thr->fs_base;
        t->tid = __atomic_fetch_add(&global_tid_counter, 1, __ATOMIC_RELAXED);
        t->ctid = ctid;

        if ((flags & 0x01000000) && ctid && user_range_ok(ctid, 4)) *(uint32_t *)ctid = (uint32_t)t->tid;
        if ((flags & 0x00100000) && ptid && user_range_ok(ptid, 4)) *(uint32_t *)ptid = (uint32_t)t->tid;

        void *src_fpu = thread_get_fpu_state(curr_thr);
        void *dst_fpu = thread_get_fpu_state(t);
        if (src_fpu && dst_fpu) {
            memcpy(dst_fpu, src_fpu, 512);
        }
        p->threads[slot] = t;
        return t->tid;
    }

    process_t *child = malloc_alloc(sizeof(process_t));
    if (!child) return (uint64_t)-ENOMEM;
    memset(child, 0, sizeof(process_t));

    thread_t *t = malloc_alloc(sizeof(thread_t));
    struct interrupt_frame *regs = malloc_alloc(sizeof(struct interrupt_frame));
    struct interrupt_frame *kframe = malloc_alloc(sizeof(struct interrupt_frame));
    void *kstack = malloc_alloc(KERNEL_STACK_SIZE);
    if (!t || !regs || !kframe || !kstack) {
        if (child) malloc_free(child);
        if (t) malloc_free(t);
        if (regs) malloc_free(regs);
        if (kframe) malloc_free(kframe);
        if (kstack) malloc_free(kstack);
        return (uint64_t)-ENOMEM;
    }

    memset(t, 0, sizeof(thread_t));
    memset(kframe, 0, sizeof(struct interrupt_frame));
    *regs = *curr_thr->regs;
    regs->rax = 0;
    if (child_stack != 0) regs->rsp = child_stack;

    t->regs = regs;
    t->syscall_frame = kframe;
    t->kernel_stack = kstack;
    t->kernel_stack_top = (uint64_t)kstack + KERNEL_STACK_SIZE;
    *(uint64_t *)kstack = 0xDEAD10CCDEAD10CCULL;
    t->state = TASK_WAITING;
    t->is_kernel = false;
    t->fs_base = curr_thr->fs_base;
    t->tid = __atomic_fetch_add(&global_tid_counter, 1, __ATOMIC_RELAXED);
    void *src_fpu = thread_get_fpu_state(curr_thr);
    void *dst_fpu = thread_get_fpu_state(t);
    if (src_fpu && dst_fpu) {
        memcpy(dst_fpu, src_fpu, 512);
    }

    if (flags & CLONE_VM) {
        child->pml4 = p->pml4;
    } else {
        child->pml4 = vmm_clone_pml4(p->pml4);
        if (!child->pml4) {
            malloc_free(kstack);
            malloc_free(regs);
            malloc_free(kframe);
            malloc_free(t);
            malloc_free(child);
            return (uint64_t)-ENOMEM;
        }
    }

    child->threads[0] = t;
    child->pid = t->tid;
    child->pgid = p->pgid;
    child->sid = p->sid;
    child->parent = p;
    child->state = TASK_WAITING;
    child->is_kernel_process = false;
    child->brk_start = p->brk_start;
    child->brk_current = p->brk_current;
    child->mmap_base = p->mmap_base;
    child->name = p->name ? kstrdup(p->name) : kstrdup("child");

    child->uid  = p->uid;
    child->euid = p->euid;
    child->suid = p->suid;
    child->gid  = p->gid;
    child->egid = p->egid;
    child->sgid = p->sgid;

    memcpy(child->cwd, p->cwd, sizeof(p->cwd));

    if (p->fd_table) {
        child->fd_table = malloc_alloc(sizeof(process_fd_table_t));
        if (child->fd_table) {
            memset(child->fd_table, 0, sizeof(process_fd_table_t));
            child->fd_table->uid = p->fd_table->uid;
            child->fd_table->gid = p->fd_table->gid;
            for (int fd = 0; fd < VFS_MAX_FD; fd++) {
                if (p->fd_table->fds[fd]) {
                    child->fd_table->fds[fd] = p->fd_table->fds[fd];
                    child->fd_table->fds[fd]->refcount++;
                }
            }
        }
    }

    if (!tasks_insert(child)) {
        task_free(child);
        return (uint64_t)-EAGAIN;
    }

    return child->pid;
}

#define FUTEX_WAIT           0
#define FUTEX_WAKE           1
#define FUTEX_FD             2
#define FUTEX_REQUEUE        3
#define FUTEX_CMP_REQUEUE     4
#define FUTEX_WAKE_OP        5
#define FUTEX_LOCK_PI        6
#define FUTEX_UNLOCK_PI      7
#define FUTEX_TRYLOCK_PI     8
#define FUTEX_WAIT_BITSET    9
#define FUTEX_WAKE_BITSET   10

#define FUTEX_PRIVATE_FLAG   128
#define FUTEX_CLOCK_REALTIME 256

#define FUTEX_WAIT           0
#define FUTEX_WAKE           1
#define FUTEX_PRIVATE_FLAG   128
#define FUTEX_CLOCK_REALTIME 256

uint64_t sys_futex_impl(uint64_t uaddr, uint64_t op, uint64_t val,
                        uint64_t timeout, uint64_t uaddr2, uint64_t val3) {
    (void)timeout; (void)uaddr2; (void)val3;
    if (!uaddr || !user_range_ok(uaddr, 4)) return (uint64_t)-EFAULT;

    int cmd = (int)(op & ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME));

    if (cmd == FUTEX_WAIT || cmd == 9 /* FUTEX_WAIT_BITSET */) {
        uint32_t cur_val = *(volatile uint32_t *)uaddr;
        if (cur_val != (uint32_t)val) {
            return (uint64_t)-EAGAIN; // -11 (EAGAIN)
        }

        thread_t *thr = get_current_thread();
        if (thr) {
            task_sleep_thread(thr);
            schedule_yield();
        }
        return 0;
    }

    if (cmd == FUTEX_WAKE || cmd == 10 /* FUTEX_WAKE_BITSET */) {
        process_t *p = get_current_process();
        if (!p) return 0;

        uint64_t woken = 0;
        for (int i = 0; i < 256; i++) {
            thread_t *t = p->threads[i];
            if (t && t != get_current_thread() && t->state == TASK_SLEEPING) {
                task_wake_thread(t);
                woken++;
                if (woken >= val) break;
            }
        }
        return woken;
    }

    return 0;
}

uint64_t sys_gettimeofday_impl(uint64_t tv_ptr, uint64_t tz_ptr) {
    if (tv_ptr) {
        if (!user_range_ok(tv_ptr, sizeof(struct timeval))) return (uint64_t)-EFAULT;
        struct timeval *tv = (struct timeval *)tv_ptr;
        uint64_t unix_ms = get_unix_time_ms();
        tv->tv_sec = (int64_t)(unix_ms / 1000ULL);
        tv->tv_usec = (int64_t)((unix_ms % 1000ULL) * 1000ULL);
    }
    if (tz_ptr) {
        if (!user_range_ok(tz_ptr, sizeof(struct timezone))) return (uint64_t)-EFAULT;
        struct timezone *tz = (struct timezone *)tz_ptr;
        tz->tz_minuteswest = 0;
        tz->tz_dsttime = 0;
    }
    return 0;
}

uint64_t sys_getresuid_impl(uint64_t ruid, uint64_t euid, uint64_t suid) {
    if (ruid) *(uint32_t *)ruid = 0; // Root (UID 0)
    if (euid) *(uint32_t *)euid = 0;
    if (suid) *(uint32_t *)suid = 0;
    return 0;
}

uint64_t sys_getresgid_impl(uint64_t rgid, uint64_t egid, uint64_t sgid) {
    if (rgid) *(uint32_t *)rgid = 0; // Root (GID 0)
    if (egid) *(uint32_t *)egid = 0;
    if (sgid) *(uint32_t *)sgid = 0;
    return 0;
}

uint64_t sys_wait4_impl(uint64_t pid, uint64_t wstatus, uint64_t options, uint64_t rusage) {
    (void)rusage;
    process_t *curr = get_current_process();
    if (!curr) return (uint64_t)-ECHILD;

    int64_t target_pid = (int64_t)pid;

    while (1) {
        bool has_children = false;
        process_t *zombie_child = NULL;
        int zombie_slot = -1;

        for (int i = 1; i < 512 /* MAX_TASKS */; i++) {
            process_t *p = tasks[i];
            if (!p || p->parent != curr) continue;

            if (target_pid > 0 && (int64_t)p->pid != target_pid) continue;

            has_children = true;

            if (p->state == TASK_ZOMBIE) {
                zombie_child = p;
                zombie_slot = i;
                break;
            }
        }

        if (zombie_child) {
            uint64_t child_pid = zombie_child->pid;
            
            if (wstatus && user_range_ok(wstatus, 4)) {
                *(uint32_t *)wstatus = (uint32_t)((zombie_child->exit_code & 0xFF) << 8);
            }

            if (zombie_slot >= 0) {
                tasks[zombie_slot] = NULL;
            }
            
            task_free(zombie_child);
            return child_pid;
        }

        if (!has_children) {
            return (uint64_t)-ECHILD;
        }

        if (options & 1) {
            return 0;
        }

        schedule_yield();
    }
}

uint64_t sys_time_impl(uint64_t tloc) {
    uint64_t t = get_unix_time();
    if (tloc && user_range_ok(tloc, sizeof(uint64_t))) {
        *(uint64_t *)tloc = t;
    }
    return t;
}

uint64_t sys_rseq_impl(uint64_t rseq, uint64_t len, uint64_t flags, uint64_t sig) {
    (void)rseq; (void)len; (void)flags; (void)sig;
    return (uint64_t)-ENOSYS;
}

struct linux_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

uint64_t sys_clock_gettime_impl(uint64_t clock_id, uint64_t tp_ptr) {
    if (!tp_ptr || !user_range_ok(tp_ptr, sizeof(struct linux_timespec)))
        return (uint64_t)-EFAULT;

    uint64_t now_ms;
    if (clock_id == 0 || clock_id == 5) {
        now_ms = get_unix_time_ms();
    } else if (clock_id == 1 || clock_id == 2 || clock_id == 3 || clock_id == 4 || clock_id == 6 || clock_id == 7) {
        now_ms = get_monotonic_ms();
    } else {
        return (uint64_t)-EINVAL;
    }

    struct linux_timespec *ts = (struct linux_timespec *)tp_ptr;
    ts->tv_sec = (int64_t)(now_ms / 1000ULL);
    ts->tv_nsec = (int64_t)((now_ms % 1000ULL) * 1000000ULL);
    return 0;
}

uint64_t sys_setuid_impl(uint64_t uid) {
    process_t *p = get_current_process();
    if (!p) return (uint64_t)-ESRCH;

    uint32_t new_uid = (uint32_t)uid;

    if (p->euid == 0) {
        p->uid  = new_uid;
        p->euid = new_uid;
        p->suid = new_uid;
    }
    else if (new_uid == p->uid || new_uid == p->suid) {
        p->euid = new_uid;
    } else {
        return (uint64_t)-EPERM;
    }

    if (p->fd_table) p->fd_table->uid = p->euid;

    return 0;
}

uint64_t sys_setgid_impl(uint64_t gid) {
    process_t *p = get_current_process();
    if (!p) return (uint64_t)-ESRCH;

    uint32_t new_gid = (uint32_t)gid;

    if (p->euid == 0) {
        p->gid  = new_gid;
        p->egid = new_gid;
        p->sgid = new_gid;
    } else if (new_gid == p->gid || new_gid == p->sgid) {
        p->egid = new_gid;
    } else {
        return (uint64_t)-EPERM;
    }

    if (p->fd_table) p->fd_table->gid = p->egid;

    return 0;
}

uint64_t sys_umask_impl(uint64_t mask) {
    process_t *p = get_current_process();
    
    uint32_t old_mask = 022;
    if (p) {
        if (p->umask == 0) p->umask = 022;
        old_mask = p->umask;
        p->umask = (uint32_t)mask & 0777;
    }

    return (uint64_t)old_mask;
}

uint64_t sys_sysinfo_impl(uint64_t info_ptr) {
    if (!info_ptr || !user_range_ok(info_ptr, sizeof(struct linux_sysinfo))) 
        return (uint64_t)-EFAULT;

    struct linux_sysinfo *si = (struct linux_sysinfo *)info_ptr;
    memset(si, 0, sizeof(struct linux_sysinfo));

    si->uptime = get_uptime_time();
    si->totalram = get_total_pages() * 4096;
    si->freeram  = get_free_pages() * 4096;
    si->procs    = 67;
    si->mem_unit = 1;

    return 0;
}

uint64_t sys_getrusage_impl(uint64_t who, uint64_t usage_ptr) {
    (void)who;
    if (!usage_ptr || !user_range_ok(usage_ptr, 144)) /* sizeof(struct rusage) == 144 */
        return (uint64_t)-EFAULT;

    memset((void *)usage_ptr, 0, 144);
    return 0;
}

uint64_t sys_rt_sigsuspend_impl(uint64_t mask_ptr) {
    (void)mask_ptr;
    schedule_yield();
    return (uint64_t)-EINTR;
}

struct linux_itimerval {
    struct timeval it_interval;
    struct timeval it_value;
};

static void alarm_refresh(process_t *p, uint64_t now_ms) {
    if (!p || p->alarm_deadline_ms == 0 || p->alarm_deadline_ms > now_ms) return;

    p->sigpending |= (1ULL << 14);
    if (p->alarm_interval_ms == 0) {
        p->alarm_deadline_ms = 0;
        return;
    }

    uint64_t periods = (now_ms - p->alarm_deadline_ms) / p->alarm_interval_ms + 1;
    p->alarm_deadline_ms += periods * p->alarm_interval_ms;
}

static uint64_t alarm_remaining_ms(process_t *p, uint64_t now_ms) {
    alarm_refresh(p, now_ms);
    if (!p || p->alarm_deadline_ms == 0 || p->alarm_deadline_ms <= now_ms) return 0;
    return p->alarm_deadline_ms - now_ms;
}

static void ms_to_timeval(uint64_t ms, struct timeval *tv) {
    tv->tv_sec = (int64_t)(ms / 1000ULL);
    tv->tv_usec = (int64_t)((ms % 1000ULL) * 1000ULL);
}

static uint64_t timeval_to_ms(struct timeval *tv) {
    if (tv->tv_sec < 0 || tv->tv_usec < 0 || tv->tv_usec >= 1000000) return (uint64_t)-1;
    return (uint64_t)tv->tv_sec * 1000ULL + (uint64_t)tv->tv_usec / 1000ULL;
}

uint64_t sys_alarm_impl(uint64_t seconds) {
    process_t *p = get_current_process();
    if (!p) return 0;

    uint64_t now_ms = get_monotonic_ms();
    uint64_t previous_ms = alarm_remaining_ms(p, now_ms);
    uint64_t previous_sec = (previous_ms + 999ULL) / 1000ULL;

    p->alarm_interval_ms = 0;
    if (seconds == 0) {
        p->alarm_deadline_ms = 0;
    } else {
        p->alarm_deadline_ms = now_ms + seconds * 1000ULL;
    }

    return previous_sec;
}

uint64_t sys_getitimer_impl(uint64_t which, uint64_t value_ptr) {
    if (!value_ptr || !user_range_ok(value_ptr, sizeof(struct linux_itimerval)))
        return (uint64_t)-EFAULT;
    if (which > 2) return (uint64_t)-EINVAL;

    process_t *p = get_current_process();
    if (!p) return (uint64_t)-ESRCH;

    struct linux_itimerval *value = (struct linux_itimerval *)value_ptr;
    uint64_t remaining_ms = 0;
    uint64_t interval_ms = 0;
    if (which == 0) {
        uint64_t now_ms = get_monotonic_ms();
        remaining_ms = alarm_remaining_ms(p, now_ms);
        interval_ms = p->alarm_interval_ms;
    }

    ms_to_timeval(interval_ms, &value->it_interval);
    ms_to_timeval(remaining_ms, &value->it_value);
    return 0;
}

uint64_t sys_setitimer_impl(uint64_t which, uint64_t value_ptr, uint64_t old_ptr) {
    if (!value_ptr || !user_range_ok(value_ptr, sizeof(struct linux_itimerval)))
        return (uint64_t)-EFAULT;
    if (old_ptr && !user_range_ok(old_ptr, sizeof(struct linux_itimerval)))
        return (uint64_t)-EFAULT;
    if (which > 2) return (uint64_t)-EINVAL;

    process_t *p = get_current_process();
    if (!p) return (uint64_t)-ESRCH;

    if (old_ptr) {
        uint64_t old_res = sys_getitimer_impl(which, old_ptr);
        if (old_res != 0) return old_res;
    }

    if (which != 0) return 0;

    struct linux_itimerval *value = (struct linux_itimerval *)value_ptr;
    uint64_t value_ms = timeval_to_ms(&value->it_value);
    uint64_t interval_ms = timeval_to_ms(&value->it_interval);
    if (value_ms == (uint64_t)-1 || interval_ms == (uint64_t)-1)
        return (uint64_t)-EINVAL;

    uint64_t now_ms = get_monotonic_ms();
    alarm_refresh(p, now_ms);
    p->alarm_interval_ms = interval_ms;
    if (value_ms == 0) {
        p->alarm_deadline_ms = 0;
    } else {
        p->alarm_deadline_ms = now_ms + value_ms;
    }

    return 0;
}

uint64_t sys_nanosleep_impl(uint64_t req_ptr, uint64_t rem_ptr) {
    if (!req_ptr || !user_range_ok(req_ptr, sizeof(struct linux_timespec)))
        return (uint64_t)-EFAULT;

    struct linux_timespec *req = (struct linux_timespec *)req_ptr;

    if (req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000LL)
        return (uint64_t)-EINVAL;

    uint64_t req_ms = (uint64_t)req->tv_sec * 1000 + (uint64_t)req->tv_nsec / 1000000;
    if (req_ms == 0 && req->tv_nsec > 0) {
        req_ms = 1;
    }

    uint64_t start_time = get_monotonic_ms();

    while (1) {
        uint64_t elapsed_ms = get_monotonic_ms() - start_time;
        if (elapsed_ms >= req_ms) {
            break;
        }
        schedule_yield();
    }

    if (rem_ptr && user_range_ok(rem_ptr, sizeof(struct linux_timespec))) {
        struct linux_timespec *rem = (struct linux_timespec *)rem_ptr;
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }

    return 0;
}

uint64_t sys_clock_getres_impl(uint64_t clock_id, uint64_t res_ptr) {
    (void)clock_id;
    if (res_ptr) {
        if (!user_range_ok(res_ptr, sizeof(struct linux_timespec)))
            return (uint64_t)-EFAULT;

        struct linux_timespec *res = (struct linux_timespec *)res_ptr;
        res->tv_sec = 0;
        res->tv_nsec = 1000000;
    }
    return 0;
}

uint64_t sys_getresuid(uint32_t *ruid, uint32_t *euid, uint32_t *suid) {
    process_t *proc = get_current_process();
    uint32_t uid = proc ? proc->uid : 0;

    if (ruid && user_range_ok((uint64_t)ruid, sizeof(uint32_t))) *ruid = uid;
    if (euid && user_range_ok((uint64_t)euid, sizeof(uint32_t))) *euid = uid;
    if (suid && user_range_ok((uint64_t)suid, sizeof(uint32_t))) *suid = uid;

    return 0;
}

uint64_t sys_getresgid(uint32_t *rgid, uint32_t *egid, uint32_t *sgid) {
    process_t *proc = get_current_process();
    uint32_t gid = proc ? proc->gid : 0;

    if (rgid && user_range_ok((uint64_t)rgid, sizeof(uint32_t))) *rgid = gid;
    if (egid && user_range_ok((uint64_t)egid, sizeof(uint32_t))) *egid = gid;
    if (sgid && user_range_ok((uint64_t)sgid, sizeof(uint32_t))) *sgid = gid;

    return 0;
}

#define PR_SET_PDEATHSIG  1
#define PR_GET_PDEATHSIG  2
#define PR_GET_DUMPABLE   3
#define PR_SET_DUMPABLE   4
#define PR_SET_NAME       15
#define PR_GET_NAME       16
#define PR_CAPBSET_READ   23
#define PR_CAPBSET_DROP   24

uint64_t sys_prctl_impl(uint64_t option, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    (void)arg3; (void)arg4; (void)arg5;
    process_t *p = get_current_process();
    if (!p) return (uint64_t)-ESRCH;

    switch (option) {
        case PR_SET_NAME: {
            if (!arg2 || !user_range_ok(arg2, 16)) return (uint64_t)-EFAULT;
            char *name_ptr = (char *)arg2;
            int i = 0;
            if (!p->name) p->name = malloc_alloc(256);
            if (p->name) {
                while (i < 15 && name_ptr[i] != '\0') {
                    p->name[i] = name_ptr[i];
                    i++;
                }
                p->name[i] = '\0';
            }
            return 0;
        }
        case PR_GET_NAME: {
            if (!arg2 || !user_range_ok(arg2, 16)) return (uint64_t)-EFAULT;
            char *name_ptr = (char *)arg2;
            if (p->name) {
                int i = 0;
                while (i < 15 && p->name[i] != '\0') {
                    name_ptr[i] = p->name[i];
                    i++;
                }
                name_ptr[i] = '\0';
            } else {
                name_ptr[0] = '\0';
            }
            return 0;
        }
        case PR_GET_DUMPABLE:
            return 1;
        case PR_SET_DUMPABLE:
            return 0;
        case PR_CAPBSET_READ:
            return 1;
        default:
            return (uint64_t)-EINVAL;
    }
}

uint64_t sys_clock_nanosleep_impl(uint64_t clock_id, uint64_t flags, uint64_t req_ptr, uint64_t rem_ptr) {
    (void)clock_id; (void)flags;
    return sys_nanosleep_impl(req_ptr, rem_ptr);
}

uint64_t sys_setfsuid_impl(uint64_t fsuid_raw) {
    process_t *p = get_current_process();
    if (!p) return 0;

    uint32_t old_fsuid = p->euid;
    uint32_t new_fsuid = (uint32_t)fsuid_raw;

    if (new_fsuid == p->uid || new_fsuid == p->euid || p->euid == 0) {
        p->euid = new_fsuid;
    }

    return (uint64_t)old_fsuid;
}

uint64_t sys_setfsgid_impl(uint64_t fsgid_raw) {
    process_t *p = get_current_process();
    if (!p) return 0;

    uint32_t old_fsgid = p->egid;
    uint32_t new_fsgid = (uint32_t)fsgid_raw;

    if (new_fsgid == p->gid || new_fsgid == p->egid || p->euid == 0) {
        p->egid = new_fsgid;
    }

    return (uint64_t)old_fsgid;
}