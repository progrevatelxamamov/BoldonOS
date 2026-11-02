"""Keep the bounded Xorg bind trace budget for the socket path, not earlier files."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "kernel/src/fs/ext4_vfs.c").read_text()
start = source.index("static void trace_xorg_ext4_create(")
end = source.index("static int ext4_vfs_op_create(", start)
helper = source[start:end]

code = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
typedef struct { const char *name; uint64_t pid; } process_t;
static process_t current = {"Xorg", 1002};
static process_t *get_current_process(void) { return &current; }
static unsigned stages;
static void xorgtrace_stage(const char *name, uint64_t pid, uint64_t detail) {
    (void)name; (void)detail;
    assert(pid == 1002);
    stages++;
}
''' + helper + r'''
int main(void) {
    trace_xorg_ext4_create("bind.ext4.fopen", 0, "/tmp/xorg-output.txt");
    trace_xorg_ext4_create("bind.ext4.fopen", 0, "/tmp/Xorg.2.log");
    assert(stages == 0);
    trace_xorg_ext4_create("bind.ext4.fopen", 0, "/tmp/.X11-unix/X2");
    assert(stages == 1);
    current.name = "sh";
    trace_xorg_ext4_create("bind.ext4.fopen", 0, "/tmp/.X11-unix/X2");
    assert(stages == 1);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    src = Path(directory) / "trace.c"
    exe = Path(directory) / "trace"
    src.write_text(code)
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", str(src), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("Xorg ext4 bind stages exclude earlier open(O_CREAT) calls")
