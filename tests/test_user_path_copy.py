"""A pathname longer than the kernel buffer must not resolve as a prefix."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
source = (root/'kernel/src/syscall/sys_file.c').read_text()
helper = source[source.index('static int copy_str_from_user('):source.index('uint64_t sys_memfd_create_w(')]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
static bool user_range_ok(uint64_t ptr, uint64_t len) { return ptr && len == 1; }
''' + helper + r'''
int main(void) {
    char output[4];
    assert(copy_str_from_user(output, 0, sizeof(output)) == -EFAULT);
    assert(copy_str_from_user(output, (uint64_t)"abc", sizeof(output)) == 0);
    assert(copy_str_from_user(output, (uint64_t)"abcd", sizeof(output)) == -ENAMETOOLONG);
    assert(copy_str_from_user(output, (uint64_t)"", sizeof(output)) == 0);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src=Path(tmp)/'test.c';binary=Path(tmp)/'test';src.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(src),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('User pathname copy rejects overlong input instead of using truncated prefix')
