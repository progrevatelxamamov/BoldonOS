#include "lwip/sys.h"
#include "lwip/opt.h"
#include "lwip/err.h"
#include "arch/sys_arch.h"

#include <mm/malloc.h>
#include <scheduler/scheduler.h>
#include <memory.h>

// Таймер ядра (если есть внешняя переменная миллисекунд - объяви, иначе юзаем свой счетчик)
extern uint64_t g_ticks_ms; // Если в твоей ОС миллисекунды называются иначе, измени тут

static inline bool interrupts_enabled(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0" : "=r"(flags));
    return (flags & (1ULL << 9)) != 0;
}

static inline void schedule_yield_safe(void) {
    if (interrupts_enabled()) schedule_yield();
}

// Возвращает текущее время системы в миллисекундах
u32_t sys_now(void) {
    // Если g_ticks_ms нет, можно использовать паузный счетчик или RDTSC
    return (u32_t)g_ticks_ms; 
}

void sys_init(void) {
    // Инициализация слоя ОС (если требуется)
}

// ============================================================================
// CЕМАФОРЫ (Semaphores)
// ============================================================================

err_t sys_sem_new(sys_sem_t *sem, u8_t count) {
    struct sys_sem_s *s = (struct sys_sem_s *)malloc_alloc(sizeof(struct sys_sem_s));
    if (!s) return ERR_MEM;

    s->counter = count;
    s->valid = 1;
    *sem = s;
    return ERR_OK;
}

void sys_sem_free(sys_sem_t *sem) {
    if (sem && *sem) {
        (*sem)->valid = 0;
        malloc_free(*sem);
        *sem = NULL;
    }
}

void sys_sem_signal(sys_sem_t *sem) {
    if (sem && *sem) {
        __atomic_fetch_add(&(*sem)->counter, 1, __ATOMIC_SEQ_CST);
    }
}

u32_t sys_arch_sem_wait(sys_sem_t *sem, u32_t timeout) {
    if (!sem || !*sem) return SYS_ARCH_TIMEOUT;

    struct sys_sem_s *s = *sem;
    u32_t start = sys_now();

    while (1) {
        int val = s->counter;
        if (val > 0) {
            if (__atomic_compare_exchange_n(&s->counter, &val, val - 1, 1, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {
                return (sys_now() - start); // Успешно захватили
            }
        }

        if (timeout != 0 && (sys_now() - start) >= timeout) {
            return SYS_ARCH_TIMEOUT; // Вышел таймаут
        }

        schedule_yield_safe();
        __asm__ volatile("pause");
    }
}

int sys_sem_valid(sys_sem_t *sem) {
    return (sem && *sem && (*sem)->valid);
}

void sys_sem_set_invalid(sys_sem_t *sem) {
    if (sem && *sem) (*sem)->valid = 0;
}

// ============================================================================
// МЬЮТЕКСЫ (Mutexes)
// ============================================================================

err_t sys_mutex_new(sys_mutex_t *mutex) {
    struct sys_mutex_s *m = (struct sys_mutex_s *)malloc_alloc(sizeof(struct sys_mutex_s));
    if (!m) return ERR_MEM;

    m->lock = 0;
    m->valid = 1;
    *mutex = m;
    return ERR_OK;
}

void sys_mutex_free(sys_mutex_t *mutex) {
    if (mutex && *mutex) {
        (*mutex)->valid = 0;
        malloc_free(*mutex);
        *mutex = NULL;
    }
}

void sys_mutex_lock(sys_mutex_t *mutex) {
    if (!mutex || !*mutex) return;
    struct sys_mutex_s *m = *mutex;

    while (__sync_lock_test_and_set(&m->lock, 1)) {
        schedule_yield_safe();
        __asm__ volatile("pause");
    }
}

void sys_mutex_unlock(sys_mutex_t *mutex) {
    if (!mutex || !*mutex) return;
    struct sys_mutex_s *m = *mutex;
    __sync_lock_release(&m->lock);
}

int sys_mutex_valid(sys_mutex_t *mutex) {
    return (mutex && *mutex && (*mutex)->valid);
}

void sys_mutex_set_invalid(sys_mutex_t *mutex) {
    if (mutex && *mutex) (*mutex)->valid = 0;
}

// ============================================================================
// ПОЧТОВЫЕ ЯЩИКИ / ОЧЕРЕДИ (Mailboxes)
// ============================================================================

err_t sys_mbox_new(sys_mbox_t *mbox, int size) {
    if (size <= 0) size = 32;

    struct sys_mbox_s *m = (struct sys_mbox_s *)malloc_alloc(sizeof(struct sys_mbox_s));
    if (!m) return ERR_MEM;

    m->buf = (void **)malloc_alloc(sizeof(void *) * size);
    if (!m->buf) {
        malloc_free(m);
        return ERR_MEM;
    }

    m->size = size;
    m->head = 0;
    m->tail = 0;
    m->count = 0;
    m->valid = 1;

    *mbox = m;
    return ERR_OK;
}

void sys_mbox_free(sys_mbox_t *mbox) {
    if (mbox && *mbox) {
        struct sys_mbox_s *m = *mbox;
        m->valid = 0;
        if (m->buf) malloc_free(m->buf);
        malloc_free(m);
        *mbox = NULL;
    }
}

void sys_mbox_post(sys_mbox_t *mbox, void *msg) {
    if (!mbox || !*mbox) return;
    struct sys_mbox_s *m = *mbox;

    while (1) {
        sys_prot_t flags = sys_arch_protect();
        if (m->count < m->size) {
            m->buf[m->head] = msg;
            m->head = (m->head + 1) % m->size;
            m->count++;
            sys_arch_unprotect(flags);
            return;
        }
        sys_arch_unprotect(flags);
        schedule_yield_safe();
        __asm__ volatile("pause");
    }
}

err_t sys_mbox_trypost(sys_mbox_t *mbox, void *msg) {
    if (!mbox || !*mbox) return ERR_MEM;
    struct sys_mbox_s *m = *mbox;

    sys_prot_t flags = sys_arch_protect();
    if (m->count >= m->size) {
        sys_arch_unprotect(flags);
        return ERR_MEM;
    }

    m->buf[m->head] = msg;
    m->head = (m->head + 1) % m->size;
    m->count++;
    sys_arch_unprotect(flags);

    return ERR_OK;
}

u32_t sys_arch_mbox_fetch(sys_mbox_t *mbox, void **msg, u32_t timeout) {
    if (!mbox || !*mbox) return SYS_ARCH_TIMEOUT;
    struct sys_mbox_s *m = *mbox;
    u32_t start = sys_now();

    while (1) {
        sys_prot_t flags = sys_arch_protect();
        if (m->count > 0) {
            void *p = m->buf[m->tail];
            m->tail = (m->tail + 1) % m->size;
            m->count--;
            sys_arch_unprotect(flags);

            if (msg) *msg = p;
            return (sys_now() - start);
        }
        sys_arch_unprotect(flags);

        if (timeout != 0 && (sys_now() - start) >= timeout) {
            return SYS_ARCH_TIMEOUT;
        }

        schedule_yield_safe();
        __asm__ volatile("pause");
    }
}

u32_t sys_arch_mbox_tryfetch(sys_mbox_t *mbox, void **msg) {
    if (!mbox || !*mbox) return SYS_MBOX_EMPTY;
    struct sys_mbox_s *m = *mbox;

    sys_prot_t flags = sys_arch_protect();
    if (m->count == 0) {
        sys_arch_unprotect(flags);
        return SYS_MBOX_EMPTY;
    }

    void *p = m->buf[m->tail];
    m->tail = (m->tail + 1) % m->size;
    m->count--;
    sys_arch_unprotect(flags);

    if (msg) *msg = p;
    return 0;
}

int sys_mbox_valid(sys_mbox_t *mbox) {
    return (mbox && *mbox && (*mbox)->valid);
}

void sys_mbox_set_invalid(sys_mbox_t *mbox) {
    if (mbox && *mbox) (*mbox)->valid = 0;
}

// ============================================================================
// ПОТОКИ / ТАСКИ (Threads)
// ============================================================================

sys_thread_t sys_thread_new(const char *name, lwip_thread_fn thread, void *arg, int stacksize, int prio) {
    (void)stacksize;
    (void)prio;

    // Создаём новый поток ядра планировщиком
    create_task_kernel_arg((void *)thread, (uint64_t)arg, name);
    
    return (sys_thread_t)1; // Возвращаем ненулевой хэндл
}