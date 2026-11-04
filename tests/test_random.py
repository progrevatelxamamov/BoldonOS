"""Regression: repeated getrandom/urandom reads must not replay the same bytes."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = '\n'.join(line for line in (root / 'kernel/src/sys/random.c').read_text().splitlines()
                   if not line.startswith('#include'))
proc = (root / 'kernel/src/syscall/sys_proc.c').read_text()
dev = (root / 'kernel/src/fs/base_devs.c').read_text()
assert 'random_fill((void *)buf, count)' in proc
assert 'random_fill(buf, count)' in dev
assert 'i * 31 + 17' not in proc
assert 'rng_state = (uint32_t)get_unix_time()' not in dev
code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
#include <string.h>
#define SPINLOCK_INIT {0}
typedef struct { int held; } spinlock_t;
static void spin_lock_irqsave(spinlock_t *l,uint64_t *flags) {
    assert(!l->held); l->held=1; *flags=0;
}
static void spin_unlock_irqrestore(spinlock_t *l,uint64_t flags) {
    (void)flags; assert(l->held); l->held=0;
}
static void cpuid(uint32_t leaf,uint32_t *a,uint32_t *b,uint32_t *c,uint32_t *d) {
    (void)leaf; *a=*b=*c=*d=0; /* test the hardware-entropy-free path */
}
static uint64_t tick=1000;
static uint64_t __rdtsc(void) { return ++tick; }
static uint64_t get_unix_time_ms(void) { return 1700000000000ULL; }
''' + source + r'''
int main(void) {
    uint8_t first[32], second[32], third[32];
    random_fill(first, sizeof(first));
    random_fill(second, sizeof(second));
    random_fill(third, sizeof(third));
    assert(memcmp(first, second, sizeof(first)) != 0);
    assert(memcmp(second, third, sizeof(second)) != 0);
    assert(first[0] != 17 || first[1] != 48); /* old fixed getrandom */
}
'''
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp) / 'random.c'
    exe = Path(tmp) / 'random'
    path.write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', str(path), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('Repeated random reads vary, including without RDRAND')
