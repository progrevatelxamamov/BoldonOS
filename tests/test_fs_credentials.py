"""Host regression for independent filesystem UID/GID and DAC drop."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
proc=(root/'kernel/src/syscall/sys_proc.c').read_text()
impl=proc[proc.index('uint64_t sys_setfsuid_impl('):]
code=r'''
#include <stdint.h>
#include <assert.h>
#define BOLDON_CAP_SETUID (1ULL<<7)
#define BOLDON_CAP_SETGID (1ULL<<6)
#define BOLDON_CAP_DAC_OVERRIDE (1ULL<<1)
typedef struct { uint32_t uid, gid; } process_fd_table_t;
typedef struct { uint32_t uid,euid,suid,fsuid,gid,egid,sgid,fsgid;
                 uint64_t cap_effective, cap_permitted;
                 process_fd_table_t *fd_table; } process_t;
static process_fd_table_t files;
static process_t p={.cap_effective=BOLDON_CAP_SETUID|BOLDON_CAP_SETGID|BOLDON_CAP_DAC_OVERRIDE,
                    .cap_permitted=BOLDON_CAP_SETUID|BOLDON_CAP_SETGID|BOLDON_CAP_DAC_OVERRIDE,
                    .fd_table=&files};
static process_t *get_current_process(void) { return &p; }
''' + impl + r'''
int main(void) {
    assert(sys_setfsuid_impl(1000)==0);
    assert(p.euid==0 && p.fsuid==1000 && files.uid==1000);
    assert(!(p.cap_effective & BOLDON_CAP_DAC_OVERRIDE));
    assert(sys_setfsuid_impl(0)==1000);
    assert(p.euid==0 && p.fsuid==0 && files.uid==0);
    assert(p.cap_effective & BOLDON_CAP_DAC_OVERRIDE);
    assert(sys_setfsgid_impl(2000)==0);
    assert(p.egid==0 && p.fsgid==2000 && files.gid==2000);
    p.cap_effective = 0;
    assert(sys_setfsuid_impl(1234)==0 && p.fsuid==0);
    assert(sys_setfsgid_impl(1234)==2000 && p.fsgid==2000);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src=Path(tmp)/'test.c';exe=Path(tmp)/'test';src.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('fsuid/fsgid stay separate from euid/egid and gate DAC override')
