#ifndef LWIP_ARCH_SYS_ARCH_H
#define LWIP_ARCH_SYS_ARCH_H

#include <stdint.h>
#include <stdbool.h>

typedef uint64_t sys_prot_t;

static inline sys_prot_t sys_arch_protect(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void sys_arch_unprotect(sys_prot_t pval) {
    if (pval & (1ULL << 9)) {
        __asm__ volatile("sti" ::: "memory");
    }
}

// Структуры объектов синхронизации
struct sys_sem_s {
    volatile int counter;
    volatile int valid;
};
typedef struct sys_sem_s *sys_sem_t;

struct sys_mutex_s {
    volatile uint32_t lock;
    volatile int valid;
};
typedef struct sys_mutex_s *sys_mutex_t;

struct sys_mbox_s {
    void **buf;
    int size;
    volatile int head;
    volatile int tail;
    volatile int count;
    volatile int valid;
};
typedef struct sys_mbox_s *sys_mbox_t;

typedef void *sys_thread_t;

#define SYS_SEM_NULL   NULL
#define SYS_MBOX_NULL  NULL
#define SYS_MUTEX_NULL NULL

#endif /* LWIP_ARCH_SYS_ARCH_H */