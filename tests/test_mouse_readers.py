"""Compile the real motion ring and verify that independent opens see the same events."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'kernel/src/inout/inout.c').read_text()
client = source[source.index('typedef struct {\n    uint8_t resp[8];'):source.index('static mouse_client_t default_mouse_client;')]
ring = source[source.index('static bool motion_pending_locked('):source.index('static void input_stream_reset(')]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#define MOVE_Q_SIZE 256
typedef struct { int8_t dx, dy, dz; uint8_t buttons; } mouse_motion_t;
static mouse_motion_t move_q[MOVE_Q_SIZE];
static uint64_t move_seq;
typedef int spinlock_t;
static spinlock_t input_q_lock;
static void spin_lock_irqsave(spinlock_t *lock, uint64_t *flags) {
    (void)lock; *flags = 0;
}
static void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
    (void)lock; (void)flags;
}
''' + client + ring + r'''
int main(void) {
    mouse_client_t nano = {0}, diag = {0};
    mouse_motion_t m;
    assert(!motion_pending_locked(&nano));
    motion_push(12, -4, 0, 1);
    assert(motion_pop_locked(&diag, &m) && m.dx == 12 && m.dy == -4);
    assert(motion_pending_locked(&nano));
    assert(motion_pop_locked(&nano, &m) && m.buttons == 1);
    assert(!motion_pending_locked(&diag));
    // Overflow drops old history only for the lagging reader.
    for (int i = 0; i < 300; ++i) motion_push(i % 120, 0, 0, 0);
    assert(motion_pop_locked(&nano, &m));
    assert(motion_pop_locked(&diag, &m));
    assert(nano.next_seq == diag.next_seq);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / 'mouse.c'
    binary = Path(directory) / 'mouse'
    path.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('Independent /dev/psaux and Nano-X mouse readers passed')
