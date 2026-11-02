"""Compile the kernel formatter and exercise procfs/partition format strings."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'kernel/src/c_stdlib.c').read_text()
assert '"%4u %5u %10llu %s\\n"' in (root / 'kernel/src/disk/hotplug.c').read_text()
formatter = source[source.index('typedef struct {\n    char *data;'):source.index('int fflush(', source.index('typedef struct {\n    char *data;'))]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <limits.h>
#include <string.h>
#define snprintf kernel_snprintf
#define sprintf kernel_sprintf
''' + formatter + r'''
int main(void) {
    char buf[256];
    assert(kernel_snprintf(buf, sizeof(buf), "%4u %5u %10llu %s\n",
                           8u, 2u, 4294967297ULL, "sda1") == 27);
    assert(strcmp(buf, "   8     2 4294967297 sda1\n") == 0);
    assert(kernel_snprintf(buf, sizeof(buf), "%s %s %s rw 0 0\n",
                           "disk", "/", "ext4") == 19);
    assert(strcmp(buf, "disk / ext4 rw 0 0\n") == 0);
    assert(kernel_snprintf(buf, sizeof(buf), "%u 1 0:1 / %s - - rw\n",
                           12u, "disk") == 23);
    assert(strcmp(buf, "12 1 0:1 / disk - - rw\n") == 0);
    assert(kernel_snprintf(buf, sizeof(buf), "%02X:%04x:%ld:%zu",
                           0xabu, 0x9u, -12L, (size_t)42) == 14);
    assert(strcmp(buf, "AB:0009:-12:42") == 0);
    assert(kernel_snprintf(buf, 5, "hello %s", "world") == 11);
    assert(strcmp(buf, "hell") == 0);
    assert(kernel_snprintf(buf, 0, "hello %s", "world") == 11);
    assert(kernel_snprintf(buf, sizeof(buf), "%c %% %d %#x", 'A', -19, 31u) == 12);
    assert(strcmp(buf, "A % -19 0x1f") == 0);
    assert(kernel_sprintf(buf, "%s:%u:%llu", "disk", 2u, 4294967297ULL) == 17);
    assert(strcmp(buf, "disk:2:4294967297") == 0);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    src = Path(directory) / 'formatter.c'
    binary = Path(directory) / 'formatter'
    src.write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror', '-fno-builtin',
                    str(src), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('Kernel snprintf consumes width/length arguments and truncates safely')
