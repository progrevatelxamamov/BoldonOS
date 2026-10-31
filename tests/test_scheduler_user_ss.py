"""Only a user return with bare USER_DATA may have its SS RPL repaired."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'kernel/src/scheduler/scheduler.c').read_text()
start = source.index('static bool scheduler_repair_user_ss(')
end = source.index('void schedule(', start)
function = source[start:end]
assert 'if (!proc->is_kernel_process && scheduler_repair_user_ss(frame))' in source
assert 'restore->ss = frame->ss;' in source
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define USER_CODE 0x20
#define USER_DATA 0x18
struct interrupt_frame { uint64_t cs, ss; };
''' + function + r'''
int main(void) {
    struct interrupt_frame f = {0x23, 0x18};
    assert(scheduler_repair_user_ss(&f) && f.ss == 0x1b);
    assert(!scheduler_repair_user_ss(&f) && f.ss == 0x1b);
    f = (struct interrupt_frame){0x08, 0x10};
    assert(!scheduler_repair_user_ss(&f) && f.ss == 0x10);
    f = (struct interrupt_frame){0x08, 0x18};
    assert(!scheduler_repair_user_ss(&f) && f.ss == 0x18);
    f = (struct interrupt_frame){0x23, 0x10};
    assert(!scheduler_repair_user_ss(&f) && f.ss == 0x10);
    f = (struct interrupt_frame){0x23, 0};
    assert(!scheduler_repair_user_ss(&f) && f.ss == 0);
    f = (struct interrupt_frame){0x1b, 0x18};
    assert(!scheduler_repair_user_ss(&f) && f.ss == 0x18);
    assert(!scheduler_repair_user_ss(0));
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    src = Path(directory) / 'test.c'
    binary = Path(directory) / 'test'
    src.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('Scheduler repairs only bare user SS on a user return')
