#include "gconsole.h"
#include "kernel.h"
#include <scheduler/scheduler.h>
#include <scheduler/spinlock.h>
#include <mm/vmm.h>
#include <mm/malloc.h>
#include <mm/pmm.h>
#include <desc/idt.h>
#include <desc/gdt.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <memory.h>
#include <io.h>
#include <cpu/smp.h>

#define MAX_TASKS   512
#define MAX_THREADS 256
#define MAX_CPUS    256
#define KERNEL_STACK_SIZE 65536
#define IDLE_STACK_SIZE KERNEL_STACK_SIZE
#define STACK_CANARY 0xDEAD10CCDEAD10CCULL

int g_stack_guard_broken = 0;

bool thread_stack_guard_ok(thread_t *t) {
    if (!t || !t->kernel_stack) return true;
    uint64_t *canary = (uint64_t *)t->kernel_stack;
    if (*canary != STACK_CANARY) {
        g_stack_guard_broken = 1;
        return false;
    }
    return true;
}
#define IA32_FS_BASE 0xC0000100
#define IA32_GS_BASE 0xC0000101
#define IA32_KERNEL_GS_BASE 0xC0000102

struct cpu_local_data cpu_locals[MAX_CPUS];

uint64_t   current_tasks[MAX_CPUS]   = {0};
uint64_t   current_threads[MAX_CPUS] = {0};
process_t *tasks[MAX_TASKS];

static uint64_t next_pid = 1;
static uint64_t next_tid = 1;

static thread_t *prev_held_threads[MAX_CPUS] = {NULL};
static bool cpu_has_current[MAX_CPUS] = {false};
static page_table_t *last_pml4[MAX_CPUS] = {NULL};
static spinlock_t sched_lock = SPINLOCK_INIT;
static bool scheduler_ready = false;

struct cpu_local_data* get_current_local_data(void) {
    int cpu = get_cpu_id();
    if (cpu < 0 || cpu >= MAX_CPUS) cpu = 0;
    return &cpu_locals[cpu];
}

bool scheduler_is_ready(void) {
    return __atomic_load_n(&scheduler_ready, __ATOMIC_ACQUIRE);
}

static char *kstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = 0;
    while (s[n]) n++;
    n++;
    char *out = malloc_alloc(n);
    if (!out) return NULL;
    memcpy(out, s, n);
    return out;
}

static void thread_free(thread_t *t) {
    if (!t) return;
    if (t->regs) malloc_free(t->regs);
    if (t->syscall_frame) malloc_free(t->syscall_frame);
    if (t->stack_base && !t->is_kernel) malloc_free(t->stack_base);
    if (t->kernel_stack) malloc_free(t->kernel_stack);
    malloc_free(t);
}

void idle_task(void) {
    while (1) {
        __asm__ volatile ("sti; hlt");
    }
}

static thread_t *build_thread_arg(void *addr, void *stack, uint64_t stack_size,
                                  uint64_t cs, uint64_t ss, uint64_t arg)
{
    thread_t *t = malloc_alloc(sizeof(thread_t));
    if (!t) return NULL;
    memset(t, 0, sizeof(thread_t));

    struct interrupt_frame *regs = malloc_alloc(sizeof(struct interrupt_frame));
    if (!regs) { malloc_free(t); return NULL; }
    memset(regs, 0, sizeof(struct interrupt_frame));

    struct interrupt_frame *kframe = malloc_alloc(sizeof(struct interrupt_frame));
    if (!kframe) { malloc_free(regs); malloc_free(t); return NULL; }
    memset(kframe, 0, sizeof(struct interrupt_frame));

    void *kstack = malloc_alloc(KERNEL_STACK_SIZE);
    if (!kstack) {
        malloc_free(regs);
        malloc_free(t);
        return NULL;
    }

    uint64_t rsp_top = ((uint64_t)stack + stack_size) & ~0xFULL;

    *(uint64_t *)kstack = STACK_CANARY;
    if (stack) *(uint64_t *)stack = STACK_CANARY;

    regs->rsp    = rsp_top;
    regs->rip    = (uint64_t)addr;
    regs->rflags = 0x202;
    regs->cs     = cs;
    regs->ss     = ss;
    regs->rdi    = arg;

    t->regs         = regs;
    t->syscall_frame = kframe;
    t->stack_base   = stack;
    t->stack_size   = stack_size;
    t->kernel_stack = kstack;
    t->kernel_stack_top = (uint64_t)kstack + KERNEL_STACK_SIZE;
    t->fs_base      = 0;
    t->state        = TASK_WAITING;
    t->tid          = 0;
    t->ctid         = 0;
    t->is_kernel    = (cs == KERNEL_CS);

    void *fpu_ptr = thread_get_fpu_state(t);
    memset(fpu_ptr, 0, 512);
    *(uint16_t *)((uint8_t *)fpu_ptr + 0)  = 0x037F;
    *(uint32_t *)((uint8_t *)fpu_ptr + 24) = 0x1F80;

    return t;
}

static thread_t *build_thread(void *addr, void *stack, uint64_t stack_size,
                              uint64_t cs, uint64_t ss)
{
    return build_thread_arg(addr, stack, stack_size, cs, ss, 0);
}

bool tasks_insert(process_t *proc) {
    uint64_t flags;
    spin_lock_irqsave(&sched_lock, &flags);
    for (uint64_t i = 1; i < MAX_TASKS; i++) {
        if (tasks[i] == NULL) {
            tasks[i] = proc;
            spin_unlock_irqrestore(&sched_lock, flags);
            return true;
        }
    }
    spin_unlock_irqrestore(&sched_lock, flags);
    return false;
}

process_t* create_task_kernel_arg(void *addr, uint64_t arg, const char *name) {
    process_t *proc = malloc_alloc(sizeof(process_t));
    if (!proc) return NULL;
    memset(proc, 0, sizeof(process_t));

    void *stack = malloc_alloc(KERNEL_STACK_SIZE);
    if (!stack) { malloc_free(proc); return NULL; }

    thread_t *t = build_thread_arg(addr, stack, KERNEL_STACK_SIZE, KERNEL_CS, KERNEL_DS, arg);
    if (!t) {
        malloc_free(stack);
        malloc_free(proc);
        return NULL;
    }

    proc->threads[0]        = t;
    proc->name              = kstrdup(name);
    proc->pml4              = get_kernel_pml4();
    proc->is_kernel_process = true;
    proc->state             = TASK_WAITING;
    t->is_kernel            = true;

    if (!tasks_insert(proc)) {
        thread_free(t);
        if (proc->name) malloc_free(proc->name);
        malloc_free(proc);
        return NULL;
    }
    return proc;
}

process_t* create_task_kernel(void *addr, const char *name) {
    return create_task_kernel_arg(addr, 0, name);
}

void scheduler_init(void) {
    process_t *idle = malloc_alloc(sizeof(process_t));
    memset(idle, 0, sizeof(process_t));

    idle->name              = kstrdup("idle");
    idle->pml4              = get_kernel_pml4();
    idle->is_kernel_process = true;
    idle->state             = TASK_RUNNING;

    // Создаем Idle-поток для каждого ядра, чтобы никому не было скучно
    uint32_t cpu_count = smp_cpu_count();
    if (cpu_count == 0) cpu_count = 1;
    if (cpu_count > MAX_CPUS) cpu_count = MAX_CPUS;
    for (uint32_t i = 0; i < cpu_count; i++) {
        void *stack = malloc_alloc(IDLE_STACK_SIZE);
        if (!stack) break;
        thread_t *t = build_thread((void*)idle_task, stack, IDLE_STACK_SIZE, KERNEL_CS, KERNEL_DS);
        if (!t) { malloc_free(stack); break; }
        t->is_kernel = true;
        t->state = TASK_RUNNING; // Держим RUNNING, чтобы нормальный шедулер их не брал
        idle->threads[i] = t;
    }

    for (int i = 0; i < MAX_CPUS; i++) {
        current_tasks[i] = 0;
        current_threads[i] = (uint64_t)i;
        cpu_has_current[i] = false;
        prev_held_threads[i] = NULL;
    }

    tasks[0] = idle;
    __atomic_store_n(&scheduler_ready, true, __ATOMIC_RELEASE);
}

process_t* create_task_user(void *addr, page_table_t *pml4,
                            void *stack, uint64_t stack_size, const char *name)
{
    process_t *proc = malloc_alloc(sizeof(process_t));
    if (!proc) return NULL;
    memset(proc, 0, sizeof(process_t));

    thread_t *t = build_thread(addr, stack, stack_size, USER_CODE | 3, USER_DATA | 3);
    if (!t) { malloc_free(proc); return NULL; }
    t->is_kernel = false;

    proc->threads[0]        = t;
    proc->name              = kstrdup(name);
    proc->pml4              = pml4;
    proc->is_kernel_process = false;
    proc->state             = TASK_WAITING;

    uint64_t flags;
    spin_lock_irqsave(&sched_lock, &flags);
    proc->pid = next_pid++;
    proc->pgid = proc->pid;
    proc->sid = proc->pid;
    spin_unlock_irqrestore(&sched_lock, flags);

    proc->cwd[0] = '/'; proc->cwd[1] = '\0';

    if (!tasks_insert(proc)) {
        task_free(proc);
        return NULL;
    }

    return proc;
}

void scheduler_update_pml4(page_table_t *new_pml4) {
    uint64_t flags;
    spin_lock_irqsave(&sched_lock, &flags);
    last_pml4[get_cpu_id()] = new_pml4;
    spin_unlock_irqrestore(&sched_lock, flags);
}

process_t *find_process_name(const char *name) {
    if (!name) return NULL;
    for (uint64_t i = 0; i < MAX_TASKS; i++) {
        process_t *p = tasks[i];
        if (!p || !p->name) continue;
        const char *a = p->name;
        const char *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a == 0 && *b == 0) return p;
    }
    return NULL;
}

void task_free(process_t *proc) {
    if (!proc) return;

    for (uint64_t i = 0; i < MAX_THREADS; i++) {
        if (proc->threads[i]) {
            thread_free(proc->threads[i]);
            proc->threads[i] = NULL;
        }
    }

    if (proc->name) malloc_free(proc->name);

    if (proc->fd_table) {
        for (int fd = 0; fd < VFS_MAX_FD; fd++) {
            if (proc->fd_table->fds[fd]) {
                sys_close(proc->fd_table, fd);
            }
        }
        malloc_free(proc->fd_table);
        proc->fd_table = NULL;
    }

    if (proc->pml4 && proc->pml4 != get_kernel_pml4()) {
        for(int i = 0; i < MAX_CPUS; i++) {
            if (proc->pml4 == last_pml4[i]) last_pml4[i] = NULL;
        }
        pmm_free_page((void*)((uint64_t)proc->pml4 - get_bootinfo().hhdm->offset));
    }

    malloc_free(proc);
}

void schedule_yield(void) {
    __asm__ volatile("int $0xFD"); // Вызываем Reschedule IPI
}

void task_sleep(process_t *proc) {
    if (!proc) return;
    uint64_t flags;
    spin_lock_irqsave(&sched_lock, &flags);
    proc->state = TASK_SLEEPING;
    for (int i = 0; i < MAX_THREADS; i++)
        if (proc->threads[i]) proc->threads[i]->state = TASK_SLEEPING;
    spin_unlock_irqrestore(&sched_lock, flags);
}

void task_wake(process_t *proc) {
    if (!proc) return;
    uint64_t flags;
    spin_lock_irqsave(&sched_lock, &flags);
    if (proc->state == TASK_SLEEPING) proc->state = TASK_WAITING;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (proc->threads[i] && proc->threads[i]->state == TASK_SLEEPING)
            proc->threads[i]->state = TASK_WAITING;
    }
    spin_unlock_irqrestore(&sched_lock, flags);
}

void task_sleep_thread(thread_t *t) {
    if (t) t->state = TASK_SLEEPING;
}

void task_wake_thread(thread_t *t) {
    if (t && t->state == TASK_SLEEPING) t->state = TASK_WAITING;
}

process_t *get_current_process(void) {
    if (!scheduler_ready) return NULL;
    int cpu = get_cpu_id();
    if (cpu < 0 || cpu >= MAX_CPUS || current_tasks[cpu] >= MAX_TASKS) return NULL;
    return tasks[current_tasks[cpu]];
}

thread_t *get_current_thread(void) {
    process_t *p = get_current_process();
    if (!p) return NULL;
    int cpu = get_cpu_id();
    if (current_threads[cpu] >= MAX_THREADS) return NULL;
    return p->threads[current_threads[cpu]];
}

static void release_prev_held(int cpu, thread_t *except) {
    thread_t *prev = prev_held_threads[cpu];
    if (!prev || prev == except) return;
    prev_held_threads[cpu] = NULL;
    prev->switch_held = false;
    if (prev->state == TASK_RUNNING) prev->state = TASK_WAITING;
}

void schedule(struct interrupt_frame *frame) {
    if (!frame || !scheduler_ready) return;

    struct cpu_local_data *local = get_cpu_local_data();
    int cpu = local->cpu_id;

    uint64_t flags;
    spin_lock_irqsave(&sched_lock, &flags);

    process_t *curr_proc = tasks[current_tasks[cpu]];
    thread_t  *curr_thr  = NULL;

    if (cpu_has_current[cpu] && curr_proc && current_threads[cpu] < MAX_THREADS) {
        curr_thr = curr_proc->threads[current_threads[cpu]];
    }

    release_prev_held(cpu, curr_thr);

    if (curr_thr) {
        if (curr_thr && curr_thr->regs) {
            bool user_saved = (curr_thr->regs->cs & 3) == 3;
            bool frame_kernel = (frame->cs & 3) == 0;

            if (user_saved && frame_kernel) {
                struct interrupt_frame keep = *curr_thr->regs;
                uint64_t ret_rax = frame->rax;
                if (curr_thr->syscall_frame) *curr_thr->syscall_frame = *frame;
                *curr_thr->regs = keep;
                curr_thr->regs->rax = ret_rax;
            } else {
                *curr_thr->regs = *frame;
                if (curr_thr->syscall_frame) curr_thr->syscall_frame->rip = 0;
            }
            
            void *fpu_ptr = thread_get_fpu_state(curr_thr);
            if (fpu_ptr) {
                __asm__ volatile("fxsave64 (%0)" :: "r"(fpu_ptr) : "memory");
            }

            curr_thr->fs_base = rdmsr(IA32_FS_BASE);
            curr_thr->switch_held = true;
        }
    }

    // 2. Очистка мертвых задач (только если их никто не исполняет)
    for (uint64_t i = 1; i < MAX_TASKS; i++) {
        process_t *p = tasks[i];
        if (p && p->state == TASK_DEAD) {
            bool can_free = true;
            for(int j = 0; j < MAX_THREADS; j++) {
                if(p->threads[j] && (p->threads[j]->state == TASK_RUNNING || p->threads[j]->switch_held)) {
                    can_free = false;
                    break;
                }
            }
            if(can_free && i != current_tasks[cpu]) {
                tasks[i] = NULL;
                task_free(p);
            }
        }
    }

    // 3. Ищем следующий поток
    uint64_t start_task = current_tasks[cpu];
    uint64_t start_thr  = current_threads[cpu];
    bool     found      = false;

    for (uint64_t dt = 1; dt <= MAX_TASKS && !found; dt++) {
        uint64_t idx = (start_task + dt) % MAX_TASKS;
        if (idx == 0) continue;
        process_t *p = tasks[idx];
        if (!p || p->state == TASK_DEAD) continue;

        uint64_t first = (idx == start_task) ? (start_thr + 1) : 0;
        for (uint64_t t = first; t < MAX_THREADS; t++) {
            thread_t *thr = p->threads[t];
            if (!thr) continue;
            
            if (thr->state == TASK_WAITING && !thr->switch_held) {
                current_tasks[cpu]   = idx;
                current_threads[cpu] = t;
                thr->state = TASK_RUNNING;
                found = true;
                break;
            }
        }
    }

    if (!found && curr_thr && curr_thr->regs && curr_thr->state == TASK_RUNNING &&
        curr_proc && curr_proc->state != TASK_DEAD) {
        current_tasks[cpu]   = start_task;
        current_threads[cpu] = start_thr;
        found = true;
    }

    if (!found) {
        current_tasks[cpu]   = 0;
        current_threads[cpu] = cpu;
    }

    process_t *proc = tasks[current_tasks[cpu]];
    if (!proc) {
        if (curr_thr) curr_thr->switch_held = false;
        spin_unlock_irqrestore(&sched_lock, flags);
        return;
    }

    thread_t *thr = proc->threads[current_threads[cpu]];
    if (!thr || !thr->regs) {
        if (curr_thr) curr_thr->switch_held = false;
        spin_unlock_irqrestore(&sched_lock, flags);
        return;
    }

    if (curr_thr && curr_thr != thr) {
        prev_held_threads[cpu] = curr_thr;
    } else if (curr_thr) {
        curr_thr->switch_held = false;
        prev_held_threads[cpu] = NULL;
    }
    cpu_has_current[cpu] = true;

    if (!proc->fd_table && !proc->is_kernel_process) {
        proc->fd_table = malloc_alloc(sizeof(process_fd_table_t));
        if (proc->fd_table) memset(proc->fd_table, 0, sizeof(process_fd_table_t));
    }

    if (!thread_stack_guard_ok(curr_thr)) {
        console_print_string("\n[STACK] kernel stack guard broken\n");
    }

    cpu_locals[cpu].kernel_rsp = thr->kernel_stack_top;

    local->current_proc = proc;
    local->current_thread = thr;
    local->kernel_rsp = thr->kernel_stack_top;
    wrmsr(IA32_FS_BASE, thr->fs_base);

    struct interrupt_frame *restore = thr->regs;
    if (thr->syscall_frame && thr->syscall_frame->rip != 0) {
        restore = thr->syscall_frame;
        thr->syscall_frame->rip = 0;
    }

    *frame = *restore;
    void *fpu_ptr = thread_get_fpu_state(thr);
    if (fpu_ptr) {
        __asm__ volatile("fxrstor64 (%0)" :: "r"(fpu_ptr) : "memory");
    }

    thr->state = TASK_RUNNING;
    if (!proc->is_kernel_process) proc->state = TASK_RUNNING;

    tss_set_stack(thr->kernel_stack_top);

    if (proc->pml4 && proc->pml4 != last_pml4[cpu]) {
        last_pml4[cpu] = proc->pml4;
        vmm_switch_address_space((uint64_t)proc->pml4 - get_bootinfo().hhdm->offset);
    }

    spin_unlock_irqrestore(&sched_lock, flags);
}