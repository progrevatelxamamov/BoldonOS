"""Host-compile the Linux ABI helpers used during desktop startup."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compat = (root / "kernel/src/syscall/sys_compat.c").read_text()
compat = compat[compat.index("extern process_t *tasks[];"):]
syscalls = (root / "kernel/src/syscall/syscall.c").read_text()
for name in ("SYS_SCHED_GETAFFINITY", "SYS_MADVISE", "SYS_PPOLL", "SYS_GETPEERNAME"):
    assert f"case {name}:" in syscalls

code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#define SMP_MAX_CPUS 256
#define TASK_DEAD 4
#define TASK_ZOMBIE 3
typedef struct { uint64_t pid; int state; } process_t;
static process_t running = { 42, 0 };
process_t *tasks[512];
static process_t *get_current_process(void) { return &running; }
static uint32_t cpus = 4;
static uint32_t smp_cpu_count(void) { return cpus; }
static bool smp_cpu_online(int id) { return id != 1; }
''' + compat + r'''
int main(void) {
    uint8_t mask[64];
    memset(mask, 0xa5, sizeof(mask));
    assert(sys_sched_getaffinity_impl(0, 7, (uintptr_t)mask) == (uint64_t)-EINVAL);
    assert(sys_sched_getaffinity_impl(0, sizeof(mask), (uintptr_t)mask) == 8);
    assert(mask[0] == 0x0d && mask[8] == 0xa5); // only actual mask size cleared
    assert(sys_sched_getaffinity_impl(42, 8, (uintptr_t)mask) == 8);
    assert(sys_sched_getaffinity_impl(999, 8, (uintptr_t)mask) == (uint64_t)-ESRCH);
    tasks[2] = &(process_t){.pid = 99};
    assert(sys_sched_getaffinity_impl(99, 8, (uintptr_t)mask) == 8);
    cpus = 65;
    assert(sys_sched_getaffinity_impl(0, 8, (uintptr_t)mask) == (uint64_t)-EINVAL);
    assert(sys_sched_getaffinity_impl(0, sizeof(mask), (uintptr_t)mask) == 16);
    assert(mask[8] == 1 && mask[16] == 0xa5);
    assert(sys_madvise_impl(0x20000, 4096, 0) == 0);
    assert(sys_madvise_impl(0x20000, 4096, 3) == 0);
    assert(sys_madvise_impl(0x20000, 4096, 4) == (uint64_t)-ENOSYS);
    assert(sys_madvise_impl(0x20001, 4096, 0) == (uint64_t)-EINVAL);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    src = Path(directory) / "wayland_abi.c"
    exe = Path(directory) / "wayland_abi"
    src.write_text(code)
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", str(src), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("Wayland startup ABI: affinity, madvise and syscall dispatch passed")
