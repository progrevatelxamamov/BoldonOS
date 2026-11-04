#include <sys/random.h>
#include <cpu/apic.h>
#include <cpu/time.h>
#include <scheduler/spinlock.h>
#include <io.h>
#include <stdbool.h>
#include <stdint.h>

static spinlock_t random_lock = SPINLOCK_INIT;
static uint64_t state;
static bool initialized;
static bool has_rdrand;

static uint64_t mix(uint64_t x) {
    x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
    return x ^ (x >> 31);
}

static bool hardware_word(uint64_t *out) {
    if (!has_rdrand) return false;
    for (int i = 0; i < 10; ++i) {
        unsigned char ok;
        uint64_t value;
        __asm__ volatile("rdrand %0; setc %1" : "=r"(value), "=qm"(ok));
        if (ok) { *out = value; return true; }
    }
    return false;
}

void random_fill(void *buf, uint64_t count) {
    uint8_t *bytes = buf;
    uint64_t flags;
    spin_lock_irqsave(&random_lock, &flags);
    if (!initialized) {
        uint32_t a, b, c, d;
        cpuid(1, &a, &b, &c, &d);
        has_rdrand = (c & (1U << 30)) != 0;
        /* Time and TSC avoid the same stream on successive boots even on
         * machines without RDRAND. This fallback is NOT cryptographic. */
        state = mix(__rdtsc() ^ get_unix_time_ms() ^
                    (uint64_t)(uintptr_t)&state);
        initialized = true;
    }
    for (uint64_t offset = 0; offset < count;) {
        uint64_t hw = 0;
        (void)hardware_word(&hw);
        state += UINT64_C(0x9e3779b97f4a7c15);
        uint64_t word = mix(state ^ __rdtsc() ^ hw);
        for (int i = 0; i < 8 && offset < count; i++, offset++) {
            bytes[offset] = (uint8_t)word;
            word >>= 8;
        }
    }
    spin_unlock_irqrestore(&random_lock, flags);
}
