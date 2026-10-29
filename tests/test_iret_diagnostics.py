import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
source = (root / 'kernel/src/desc/idt.c').read_text()
start = source.index('static bool panic_read_word(')
end = source.index('static void dump_frame_raw(', start)
functions = source[start:end].replace('__asm__ volatile("sgdt %0" : "=m"(gdtr));', 'gdtr = test_gdtr;')
prefix = r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
typedef struct { int unused; } page_table_t;
typedef struct { page_table_t *pml4; } process_t;
struct interrupt_frame { uint64_t isr_number, cs, rip, rsp, error_code; };
struct gdt_ptr { uint16_t limit; uint64_t base; };
static struct gdt_ptr test_gdtr = {55, 0x5000};
static page_table_t user, kernel;
static uint8_t pages[6][4096];
static uint64_t values[32];
static unsigned count, messages;
static struct { uint64_t offset; } hhdm;
struct boot { void *unused; typeof(hhdm) *hhdm; };
static struct boot get_bootinfo(void) { return (struct boot){0, &hhdm}; }
static page_table_t *get_kernel_pml4(void) { return &kernel; }
static uint64_t vmm_get_phys(page_table_t *table, uint64_t address) {
    assert(table == &user);
    if (address >= 0x6000) return 0;
    return (uintptr_t)pages[address / 4096];
}
static void console_print_string(const char *s) { (void)s; messages++; }
static void console_print_hex(uint64_t value) { assert(count < 32); values[count++] = value; }
'''
suffix = r'''
int main(void) {
    uint64_t value = 0;
    pages[1][4095] = 0x12; pages[2][0] = 0x34;
    assert(panic_read_word(&user, 0x1fff, &value) && value == 0x3412);
    assert(!panic_read_word(&user, UINT64_MAX, &value));
    assert(!panic_read_word(&user, 0x0000800000000000ULL, &value));
    assert(!panic_read_word(&user, 0xFFFF7FFFFFFFFFE0ULL, &value));
    assert(!panic_read_word(&user, 0x6000, &value));
    process_t proc = {&user};
    struct interrupt_frame f = {13, 8, 0x1000, 0x3000, 0x18};
    pages[1][0] = 0x48; pages[1][1] = 0xcf;
    uint64_t target[] = {0x4002756e, 0x23, 0x246, 0x7fffffe788, 0x1b};
    memcpy(pages[3], target, sizeof(target));
    uint64_t cs = 0x00affa000000ffff, ss = 0x0080f2000000ffff;
    memcpy(pages[5] + 0x20, &cs, 8); memcpy(pages[5] + 0x18, &ss, 8);
    dump_iret_target(&f, &proc);
    assert(count == 10 && values[0] == target[0] && values[4] == 0x1b);
    assert(values[7] == ss && values[8] == cs && values[9] == ss);
    unsigned before = messages;
    f.isr_number = 14; dump_iret_target(&f, &proc); assert(messages == before);
    f.isr_number = 13; f.rsp = 0x6000; count = 0;
    dump_iret_target(&f, &proc); assert(messages > before && count == 5);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    test = pathlib.Path(directory) / 'iret.c'
    binary = pathlib.Path(directory) / 'iret'
    test.write_text(prefix + functions + suffix)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', str(test), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('IRET target diagnostics, descriptor selection, cross-page and unmapped reads passed')
