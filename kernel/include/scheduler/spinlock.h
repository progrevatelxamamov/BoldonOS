#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdbool.h>
#include <stdint.h>

typedef struct spinlock {
    volatile int locked;
} spinlock_t;

#define SPINLOCK_INIT { 0 }

static inline void spin_lock(spinlock_t *spin) {
    while (__atomic_test_and_set(&spin->locked, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&spin->locked, __ATOMIC_RELAXED)) {
            __asm__ volatile ("pause");
        }
    }
}

static inline void spin_unlock(spinlock_t *spin) {
    __atomic_clear(&spin->locked, __ATOMIC_RELEASE);
}

static inline bool spin_trylock(spinlock_t *spin) {
    return !__atomic_test_and_set(&spin->locked, __ATOMIC_ACQUIRE);
}

static inline void spin_lock_irqsave(spinlock_t *spin, uint64_t *flags) {
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(*flags) :: "memory");
    spin_lock(spin);
}

static inline void spin_unlock_irqrestore(spinlock_t *spin, uint64_t flags) {
    spin_unlock(spin);
    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
}

#endif