"""Check the real kill syscall's UID/CAP_KILL authorization."""
from pathlib import Path
import subprocess
import tempfile
src=(Path(__file__).resolve().parents[1]/'kernel/src/syscall/sys_proc.c').read_text()
func=src[src.index('uint64_t sys_kill_impl('):src.index('uint64_t sys_getpid_impl(',src.index('uint64_t sys_kill_impl('))]
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <assert.h>
#define BOLDON_CAP_KILL (1ULL<<5)
#define TASK_DEAD 4
#define TASK_ZOMBIE 3
typedef struct { int state; } thread_t;
typedef struct { uint64_t pid,pgid; uint32_t uid,euid; uint64_t cap_effective,sigpending;
                 bool is_kernel_process; int state; thread_t *threads[256]; } process_t;
static process_t caller={ .pid=1, .pgid=1, .euid=1000 };
static process_t target={ .pid=7, .pgid=7, .uid=2000, .euid=2000 };
static process_t *tasks[512];
static process_t *get_current_process(void) { return &caller; }
static void schedule_yield(void) { assert(0); }
''' + func + r'''
int main(void) {
    tasks[1]=&caller; tasks[2]=&target;
    assert(sys_kill_impl(7,2)==(uint64_t)-EPERM && !target.sigpending);
    assert(sys_kill_impl(7,0)==(uint64_t)-EPERM);
    assert(sys_kill_impl(99,0)==(uint64_t)-ESRCH);
    caller.cap_effective=BOLDON_CAP_KILL;
    assert(sys_kill_impl(7,2)==0 && target.sigpending);
    target.sigpending=0; caller.cap_effective=0; target.uid=1000;
    assert(sys_kill_impl(7,2)==0 && target.sigpending);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    from pathlib import Path
    source=Path(tmp)/'test.c'; exe=Path(tmp)/'test'; source.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(source),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('kill: foreign UID denied unless effective CAP_KILL')
