"""Verify Ctrl/Shift are interpreted in PTY event order, not latest global state."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'kernel/src/sys/init_runner.c').read_text()
functions = source[source.index('typedef struct {\n    bool left_ctrl'):source.index('static void pty_input_task(')]
code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#define KEY_LCTRL 29
#define KEY_RCTRL 97
#define KEY_LSHIFT 42
#define KEY_RSHIFT 54
''' + functions + r'''
int main(void) {
    pty_modifiers_t mod = {0};
    pty_update_modifiers(&mod, KEY_LCTRL, true);
    assert(mod.left_ctrl);
    // Press C, then release Ctrl before the task reads the next event:
    // at C the local state must still have Ctrl down.
    pty_update_modifiers(&mod, 46, true);
    assert(mod.left_ctrl || mod.right_ctrl);
    pty_update_modifiers(&mod, KEY_LCTRL, false);
    assert(!mod.left_ctrl);
    pty_update_modifiers(&mod, KEY_LSHIFT, true);
    pty_update_modifiers(&mod, KEY_RCTRL, true);
    assert(mod.left_shift && mod.right_ctrl);
    pty_update_modifiers(&mod, KEY_LSHIFT, false);
    assert(!mod.left_shift && mod.right_ctrl);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    src = Path(directory) / 'pty.c'
    dst = Path(directory) / 'pty'
    src.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(dst)], check=True)
    subprocess.run([str(dst)], check=True)
print('PTY modifier event-order regression passed')
