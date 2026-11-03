"""Exercise the real prctl ambient/bounding/no-new-privs branches."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
proc = (root / 'kernel/src/syscall/sys_proc.c').read_text()
part = proc[proc.index('#define PR_SET_PDEATHSIG'):proc.index('uint64_t sys_clock_nanosleep_impl(',proc.index('#define PR_SET_PDEATHSIG'))]
code = r'''
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#define BOLDON_CAP_SETPCAP (1ULL<<8)
#define BOLDON_CAP_SUPPORTED ((1ULL<<1)|(1ULL<<3)|(1ULL<<5)|(1ULL<<6)|(1ULL<<7)|(1ULL<<8)|(1ULL<<21)|(1ULL<<22))
typedef struct { char *name; bool no_new_privs; uint64_t cap_effective, cap_permitted,
                 cap_inheritable, cap_ambient, cap_bounding; } process_t;
static process_t p = { .cap_effective=BOLDON_CAP_SUPPORTED,
  .cap_permitted=BOLDON_CAP_SUPPORTED, .cap_bounding=BOLDON_CAP_SUPPORTED };
static process_t *get_current_process(void) { return &p; }
static void *malloc_alloc(size_t n) { return malloc(n); }
static bool user_range_ok(uint64_t p, uint64_t n) { return p >= 0x10000 && n < 4096; }
''' + part + r'''
int main(void) {
    assert(sys_prctl_impl(23, 1,0,0,0) == 1); /* bounding DAC */
    assert(sys_prctl_impl(23, 40,0,0,0) == 0); /* unsupported */
    assert(sys_prctl_impl(47, 2,1,0,0) == (uint64_t)-EPERM); /* not inheritable */
    p.cap_inheritable = 1ULL<<1;
    assert(sys_prctl_impl(47, 2,1,0,0) == 0);
    assert(sys_prctl_impl(47, 1,1,0,0) == 1);
    assert(sys_prctl_impl(47, 3,1,0,0) == 0);
    assert(sys_prctl_impl(47, 1,1,0,0) == 0);
    assert(sys_prctl_impl(47, 2,1,0,0) == 0);
    assert(sys_prctl_impl(47, 4,0,0,0) == 0 && !p.cap_ambient);
    assert(sys_prctl_impl(24, 1,0,0,0) == 0);
    assert(sys_prctl_impl(23, 1,0,0,0) == 0);
    p.cap_effective &= ~BOLDON_CAP_SETPCAP;
    assert(sys_prctl_impl(24, 7,0,0,0) == (uint64_t)-EPERM);
    assert(sys_prctl_impl(38, 0,0,0,0) == (uint64_t)-EINVAL);
    assert(sys_prctl_impl(38, 1,0,0,0) == 0);
    assert(sys_prctl_impl(39, 0,0,0,0) == 1);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src=Path(tmp)/'test.c'; exe=Path(tmp)/'test'
    src.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('prctl ambient, bounding and no_new_privs transitions passed')
