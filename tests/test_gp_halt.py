"""Kernel #GP must halt for a stable hardware photo, not exit the user task."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'kernel/src/desc/idt.c').read_text()
start = source.index('static bool exception_is_kernel_gp(')
end = source.index('void execption_handlerer(', start)
function = source[start:end]
assert 'bool exit_process = user_mode;' in source
assert 'if (exception_is_kernel_gp(frame)) {' in source
assert '[GP-HALT] Kernel #GP; halted for diagnosis' in source
assert 'for (;;) __asm__ volatile("cli; hlt");' in source
code = r'''
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
struct interrupt_frame { uint64_t isr_number, cs; };
''' + function + r'''
int main(void) {
    struct interrupt_frame frame = {13, 8};
    assert(exception_is_kernel_gp(&frame));
    frame.cs = 0x23;
    assert(!exception_is_kernel_gp(&frame));
    frame.cs = 0x08; frame.isr_number = 14;
    assert(!exception_is_kernel_gp(&frame));
    assert(!exception_is_kernel_gp(0));
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    src = Path(directory) / 'test.c'
    exe = Path(directory) / 'test'
    src.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('Kernel GP halt and user-only exit regressions passed')
