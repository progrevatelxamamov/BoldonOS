"""Production USB trace ring: bounded, ordered, stable snapshots and partial reads."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'kernel/src/inout/inout.c').read_text()
trace = source[source.index('/* Enumeration transitions only'):source.index('/* Raw samples and pre-clamp')]
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef int spinlock_t;
#define SPINLOCK_INIT 0
static void spin_lock_irqsave(spinlock_t *p, uint64_t *f) { (void)p; *f = 0; }
static void spin_unlock_irqrestore(spinlock_t *p, uint64_t f) { (void)p; (void)f; }
static uint64_t get_monotonic_ms(void) { return 42; }
static int fail_alloc;
static void *malloc_alloc(size_t n) { return fail_alloc ? NULL : calloc(1, n); }
static void malloc_free(void *p) { free(p); }
typedef struct { uint64_t offset; void *private_data; } vfs_file_t;
typedef struct {
    int (*open)(vfs_file_t *);
    int (*read)(vfs_file_t *, void *, uint64_t);
    int (*close)(vfs_file_t *);
} vfs_file_ops_t;
''' + trace + r'''
int main(void) {
    vfs_file_t first = {0}, second = {0};
    inout_usb_trace("lp.hc", 0x1234, 1, 1, 2);
    assert(usbtrace_ops.open(&first) == 0);
    for (unsigned i = 0; i < 120; i++)
        inout_usb_trace("lp.port-on", i, 0, 0, 0);
    assert(usbtrace_ops.open(&second) == 0);
    char first_data[2048] = {0}, second_data[USB_TRACE_TEXT] = {0};
    int n = usbtrace_ops.read(&first, first_data, 11);
    assert(n == 11);
    n = usbtrace_ops.read(&first, first_data + 11, sizeof(first_data) - 11);
    assert(n > 0 && strstr(first_data, "lp.hc 00001234"));
    assert(!strstr(first_data, "lp.port-on"));
    assert(usbtrace_ops.read(&first, first_data, 1) == 0);
    n = usbtrace_ops.read(&second, second_data, sizeof(second_data));
    assert(n > 0 && strstr(second_data, "overwritten=25"));
    assert(!strstr(second_data, "lp.hc"));
    assert(strstr(second_data, "25 42 lp.port-on 00000018"));
    assert(strstr(second_data, "120 42 lp.port-on 00000077"));
    assert(usbtrace_ops.close(&first) == 0);
    assert(usbtrace_ops.close(&second) == 0);
    // An allocation failure must be visible, not disguised as ENOENT.
    fail_alloc = 1;
    vfs_file_t fallback = {0};
    assert(usbtrace_ops.open(&fallback) == 0);
    char msg[80] = {0};
    assert(usbtrace_ops.read(&fallback, msg, sizeof(msg)) > 0);
    assert(strcmp(msg, "usbtrace: snapshot allocation failed\n") == 0);
    assert(usbtrace_ops.close(&fallback) == 0);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    src = Path(directory) / 'trace.c'
    exe = Path(directory) / 'trace'
    src.write_text(program)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('USB trace ring overwrite, independent open snapshot and partial reads passed')
