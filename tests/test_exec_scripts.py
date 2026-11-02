"""Exercise production #! parser and Linux execve errno translation."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
elf = (root / 'kernel/src/sys/elf.c').read_text()
proc = (root / 'kernel/src/syscall/sys_proc.c').read_text()
parser = elf[elf.index('static int parse_shebang('):elf.index('static int sys_execve_depth(')]
wrapper = proc[proc.index('uint64_t sys_execve_impl('):proc.index('uint64_t sys_brk_impl(')]
assert 'return sys_execve_depth(interpreter, script_argv, envp, depth + 1)' in elf
assert 'script_argv[count++] = (char *)pathname;' in elf
assert 'vfs_lseek(f, 0, VFS_SEEK_SET);' in elf
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
''' + parser + r'''
static int failure;
static int sys_execve(const char *path, char *const argv[], char *const envp[]) {
    (void)path; (void)argv; (void)envp;
    errno = failure;
    return -1;
}
''' + wrapper + r'''
int main(void) {
    char path[256], arg[256];
    const char *line = "#!/bin/sh -e\n";
    assert(parse_shebang(line, strlen(line), path, arg, 256) == 0);
    assert(!strcmp(path, "/bin/sh") && !strcmp(arg, "-e"));
    line = "#! /usr/bin/env -S sh -x\r\n";
    assert(parse_shebang(line, strlen(line), path, arg, 256) == 0);
    assert(!strcmp(path, "/usr/bin/env") && !strcmp(arg, "-S sh -x"));
    line = "#!/bin/sh"; // short script at EOF without a newline
    assert(parse_shebang(line, strlen(line), path, arg, 256) == 0);
    assert(!strcmp(path, "/bin/sh") && !arg[0]);
    line = "#!\n";
    assert(parse_shebang(line, strlen(line), path, arg, 256) == -ENOEXEC);
    line = "#!/bin/sh\n";
    assert(parse_shebang(line, strlen(line), path, arg, 3) == -ENOEXEC);
    for (int i = 0; i < 256; i++) path[i] = 'a';
    path[0] = '#'; path[1] = '!';
    assert(parse_shebang(path, 256, path, arg, 256) == -ENOEXEC);
    failure = ENOENT;
    assert((int64_t)sys_execve_impl(1, 0, 0) == -ENOENT);
    failure = ENOEXEC;
    assert((int64_t)sys_execve_impl(1, 0, 0) == -ENOEXEC);
    failure = ELOOP;
    assert((int64_t)sys_execve_impl(1, 0, 0) == -ELOOP);
    assert((int64_t)sys_execve_impl(0, 0, 0) == -EFAULT);
}
'''
with tempfile.TemporaryDirectory() as directory:
    src = Path(directory) / 'test.c'
    exe = Path(directory) / 'test'
    src.write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('ELF script #! parsing and Linux execve error propagation passed')
