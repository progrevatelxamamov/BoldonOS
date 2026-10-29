import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
source = (root / 'kernel/src/desc/gdt.c').read_text()
start = source.index('void create_descriptor(')
end = source.index('void create_tss_descriptor(', start)
code = '''
#include <stdint.h>
#include <assert.h>
#include <desc/gdt.h>
static uint64_t gdt_descriptors[1][7];
''' + source[start:end] + '''
int main(void) {
    create_descriptor(0, 1, 0, 0xFFFFF, GDT_CODE_PL0);
    create_descriptor(0, 2, 0, 0xFFFFF, GDT_DATA_PL0);
    create_descriptor(0, 3, 0, 0xFFFFF, GDT_DATA_PL3);
    create_descriptor(0, 4, 0, 0xFFFFF, GDT_CODE_PL3);
    assert(gdt_descriptors[0][1] == 0x00AF9A000000FFFFULL);
    assert(gdt_descriptors[0][2] == 0x00CF92000000FFFFULL);
    assert(gdt_descriptors[0][3] == 0x00CFF2000000FFFFULL);
    assert(gdt_descriptors[0][4] == 0x00AFFA000000FFFFULL);
    assert((USER_DATA | 3) == 0x1B);
    assert((USER_CODE | 3) == 0x23);
    for (unsigned i = 2; i <= 3; i++) {
        uint64_t d = gdt_descriptors[0][i];
        assert(d & (1ULL << 54));
        assert(!(d & (1ULL << 53)));
        assert(((d >> 40) & 0x12) == 0x12);
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = pathlib.Path(directory) / 'gdt.c'
    binary = pathlib.Path(directory) / 'gdt-test'
    path.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(root / 'kernel/include'), str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('GDT flat data/stack descriptors, long-mode code and user selectors passed')
