#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <mm/vmm.h>
#include <desc/idt.h>
#include <fs/vfs.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    TASK_WAITING,
    TASK_RUNNING,
    TASK_SLEEPING,
    TASK_ZOMBIE,
    TASK_DEAD
} process_state_t;

typedef struct thread {
    struct interrupt_frame *regs;
    void *stack_base;
    uint64_t stack_size;
    void *kernel_stack;
    uint64_t kernel_stack_top;
    uint64_t fs_base;
    int state;
    uint64_t tid;
    uint64_t ctid;
    bool is_kernel;

    uint8_t fpu_area[512 + 16];
    struct interrupt_frame *syscall_frame; 
} thread_t;

static inline void *thread_get_fpu_state(thread_t *t) {
    if (!t) return NULL;
    uint64_t addr = (uint64_t)t->fpu_area;
    return (void *)((addr + 15) & ~15ULL);
}

typedef struct process {
    process_state_t state;
    bool is_kernel_process;
    page_table_t *pml4;
    struct thread *threads[256];
    char *name;
    struct process *parent;
    process_fd_table_t *fd_table;
    uint64_t brk_start;
    uint64_t brk_current;
    uint64_t mmap_base;

    uint64_t pid;
    uint64_t pgid;
    uint64_t sid;

    uint64_t sigmask;
    uint64_t sigpending;
    char cwd[256];
    void *sigactions[64];
    int exit_code;

    uint32_t uid;
    uint32_t euid;
    uint32_t suid;

    uint32_t gid;
    uint32_t egid;
    uint32_t sgid;

    uint32_t umask;
} process_t;

struct cpu_local_data {
    struct cpu_local_data *self; // Обязательно на offset 0x00!
    int cpu_id;                  // Логический ID ядра (offset 0x08)

    // Поля для syscall / контекста
    uint64_t kernel_rsp;
    uint64_t user_rsp;

    process_t *current_proc;     // Текущий процесс
    thread_t  *current_thread;   // Текущий поток

    uint64_t user_rip;
    uint64_t user_rflags;
    uint64_t user_rdx;
    uint64_t user_rbx;
    uint64_t user_rbp;
    uint64_t user_rsi;
    uint64_t user_rdi;
    uint64_t user_r8;
    uint64_t user_r9;
    uint64_t user_r10;
    uint64_t user_r12;
    uint64_t user_r13;
    uint64_t user_r14;
    uint64_t user_r15;
};

_Static_assert(offsetof(struct cpu_local_data, self) == 0x00, "cpu_local_data: self must stay at 0x00");
_Static_assert(offsetof(struct cpu_local_data, cpu_id) == 0x08, "cpu_local_data: cpu_id must stay at 0x08");
_Static_assert(offsetof(struct cpu_local_data, kernel_rsp) == 0x10, "cpu_local_data: kernel_rsp must stay at 0x10");
_Static_assert(offsetof(struct cpu_local_data, user_rsp) == 0x18, "cpu_local_data: user_rsp must stay at 0x18");

struct cpu_local_data* get_current_local_data(void);

bool thread_stack_guard_ok(thread_t *t);
extern int g_stack_guard_broken;

static inline struct cpu_local_data* get_cpu_local_data(void) {
    return get_current_local_data();
}

static inline int get_cpu_local_data_id(void) {
    return get_current_local_data()->cpu_id;
}
extern uint64_t current_task;
extern uint64_t current_thread;

void schedule(struct interrupt_frame *frame);
void scheduler_init(void);
void scheduler_update_pml4(page_table_t *new_pml4);

process_t* create_task_kernel(void *addr, const char *name);
process_t* create_task_kernel_arg(void *addr, uint64_t arg, const char *name);
process_t* create_task_user(void *addr, page_table_t *pml4, void *stack, uint64_t stack_size, const char *name);
void task_free(process_t *proc);
void schedule_yield(void);
void task_sleep(process_t *proc);
void task_wake(process_t *proc);
void task_sleep_thread(thread_t *t);
void task_wake_thread(thread_t *t);
process_t *get_current_process(void);
thread_t  *get_current_thread(void);
bool tasks_insert(process_t *proc);

#endif