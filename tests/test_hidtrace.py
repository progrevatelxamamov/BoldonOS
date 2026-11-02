"""Production bounded HID sample trace: raw bytes, decoded deltas and snapshots."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'kernel/src/inout/inout.c').read_text()
trace = source[source.index('/* Raw samples and pre-clamp'):source.index('typedef struct {\n    int8_t  dx;')]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef int spinlock_t;
#define SPINLOCK_INIT 0
static void spin_lock_irqsave(spinlock_t *p, uint64_t *f) { (void)p; *f = 0; }
static void spin_unlock_irqrestore(spinlock_t *p, uint64_t f) { (void)p; (void)f; }
static int fail_alloc;
static void *malloc_alloc(size_t n) { return fail_alloc ? NULL : calloc(1, n); }
static void malloc_free(void *p) { free(p); }
typedef struct { uint64_t offset; void *private_data; } vfs_file_t;
typedef struct { int (*open)(vfs_file_t *);
    int (*read)(vfs_file_t *, void *, uint64_t);
    int (*close)(vfs_file_t *); } vfs_file_ops_t;
''' + trace + r'''
int main(void) {
    const uint8_t raw[8] = {2, 0xff, 0x7f, 0x01, 0, 0, 0, 0};
    uint64_t first = inout_hid_mouse_raw(raw, 4, true, 2, 8, 16, 24, 8);
    inout_hid_mouse_decoded(first, 32767, -1, 0);
    vfs_file_t original = {0};
    assert(hidtrace_ops.open(&original) == 0);
    char out[HID_TRACE_TEXT] = {0};
    assert(hidtrace_ops.read(&original, out, 12) == 12);
    assert(hidtrace_ops.read(&original, out + 12, sizeof(out) - 12) > 0);
    assert(strstr(out, "len=4 id=02 layout=1 x=8/16 y=24/8"));
    assert(strstr(out, "raw=02 ff 7f 01 00 00 00 00 00 00 00 00 00 00 00 00 decoded=1 dx=32767 dy=-1"));
    assert(inout_hid_mouse_raw(raw, 4, true, 2, 8, 16, 24, 8) == 0); // repeated idle data
    assert(hidtrace_ops.read(&original, out, 1) == 0);
    for (unsigned i = 0; i < 20; i++) {
        uint8_t changing[8] = {0}; changing[1] = (uint8_t)(i + 1);
        uint64_t seq = inout_hid_mouse_raw(changing, 8, false, 0, 0, 0, 0, 0);
        if (i == 0) inout_hid_mouse_decoded(seq, 1, 2, 3);
    }
    vfs_file_t recent = {0};
    assert(hidtrace_ops.open(&recent) == 0);
    memset(out, 0, sizeof(out));
    assert(hidtrace_ops.read(&recent, out, sizeof(out)) > 0);
    assert(strstr(out, "samples=21 overwritten=5"));
    assert(!strstr(out, "dx=32767"));
    assert(strstr(out, "decoded=0 dx=0 dy=0"));
    assert(hidtrace_ops.close(&original) == 0);
    assert(hidtrace_ops.close(&recent) == 0);
    fail_alloc = 1;
    vfs_file_t fallback = {0};
    assert(hidtrace_ops.open(&fallback) == 0);
    memset(out, 0, sizeof(out));
    assert(hidtrace_ops.read(&fallback, out, sizeof(out)) > 0);
    assert(strcmp(out, "hidtrace: snapshot allocation failed\n") == 0);
    assert(hidtrace_ops.close(&fallback) == 0);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    src = Path(directory) / 'test.c'
    exe = Path(directory) / 'test'
    src.write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('HID raw sample, pre-clamp deltas, ring overflow and snapshots passed')
