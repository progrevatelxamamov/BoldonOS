"""Prevent unprivileged KDSETMODE framebuffer ownership changes."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
src=(root/'kernel/src/syscall/sys_file.c').read_text()
helper=src[src.index('static bool console_mode_allowed('):src.index('uint64_t sys_ioctl_impl(',src.index('static bool console_mode_allowed('))]
assert 'if (!console_mode_allowed(p)) return (uint64_t)-EPERM;' in src
code=r'''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#define BOLDON_CAP_SYS_ADMIN (1ULL<<21)
typedef struct { bool is_kernel_process; uint64_t cap_effective; } process_t;
''' + helper + r'''
int main(void) {
    process_t p={0};
    assert(!console_mode_allowed(NULL));
    assert(!console_mode_allowed(&p));
    p.cap_effective=BOLDON_CAP_SYS_ADMIN;
    assert(console_mode_allowed(&p));
    p.cap_effective=0;p.is_kernel_process=true;
    assert(console_mode_allowed(&p));
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c=Path(tmp)/'test.c';exe=Path(tmp)/'test';c.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('KDSETMODE requires CAP_SYS_ADMIN or kernel process')
