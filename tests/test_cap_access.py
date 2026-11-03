"""Verify dropping DAC override removes the root access bypass."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
src = (root / 'kernel/src/fs/vfs.c').read_text()
func = src[src.index('int vfs_check_access('):src.index('vfs_file_t *vfs_open(', src.index('int vfs_check_access('))]
code = r'''
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#define BOLDON_CAP_DAC_OVERRIDE (1ULL << 1)
#define VFS_PERM_EXEC 1
typedef struct { uint32_t fsuid; uint64_t cap_effective; } process_t;
typedef struct { uint32_t uid, gid, mode; } vfs_node_t;
static process_t p = { 0, BOLDON_CAP_DAC_OVERRIDE };
static process_t *get_current_process(void) { return &p; }
''' + func + r'''
int main(void) {
    vfs_node_t n = { .uid = 123, .gid = 123, .mode = 0000 };
    assert(vfs_check_access(&n, 0, 0, 4) == 0);
    assert(vfs_check_access(&n, 0, 0, VFS_PERM_EXEC) == -1);
    n.mode = 0001;
    assert(vfs_check_access(&n, 0, 0, VFS_PERM_EXEC) == 0);
    n.mode = 0000;
    p.cap_effective = 0;
    assert(vfs_check_access(&n, 0, 0, 4) == -1);
    n.mode = 0004;
    assert(vfs_check_access(&n, 0, 0, 4) == 0);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    source=Path(tmp)/'test.c'; binary=Path(tmp)/'test'
    source.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('Root access obeys effective DAC_OVERRIDE capability')
