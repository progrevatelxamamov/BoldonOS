import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
source = (root / 'kernel/src/pci/xhci.c').read_text()


def section(text, start, end):
    return text[text.index(start):text.index(end, text.index(start))]


def run_c(program):
    with tempfile.TemporaryDirectory() as directory:
        path = pathlib.Path(directory)
        (path / 'test.c').write_text(program)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        str(path / 'test.c'), '-o', str(path / 'test')], check=True)
        subprocess.run([str(path / 'test')], check=True, timeout=10)


helpers = section(source, 'static bool xhci_wait_bits(',
                  'static struct xhci_trb *cmd_ring_enqueue')
run_c('''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
typedef struct { uint8_t bus, slot, func; } pci_device_t;
typedef struct {
    volatile uint8_t *cap_regs;
    uint8_t port_major[33], port_slot_type[33];
} xhci_controller_t;
static uint32_t config[64];
static uint64_t clock_ticks;
static uint64_t hpet_read_counter(void) { return clock_ticks++; }
static uint64_t hpet_ticks_for_ms(uint32_t ms) { return ms; }
static uint32_t xhci_read32(volatile uint8_t *b, uint32_t off) {
    return *(volatile uint32_t *)(b + off);
}
static void xhci_write32(volatile uint8_t *b, uint32_t off, uint32_t value) {
    *(volatile uint32_t *)(b + off) = value;
}
static uint32_t pci_read_config(int b, int s, int f, uint8_t off) {
    (void)b; (void)s; (void)f; return config[off / 4];
}
static void pci_write_config(int b, int s, int f, uint8_t off, uint32_t v) {
    (void)b; (void)s; (void)f; config[off / 4] = v;
}
static void console_print_string(const char *s) { (void)s; }
''' + helpers + '''
int main(void) {
    static uint32_t mmio[0x100000 / 4];
    xhci_controller_t hc = {.cap_regs = (uint8_t *)mmio};
    mmio[0x100 / 4] = 1 | (4u << 8) | (1u << 16);
    mmio[0x104 / 4] = 0xffff;
    mmio[0x110 / 4] = 2 | (2u << 24);
    mmio[0x118 / 4] = 1 | (4u << 8);
    mmio[0x11c / 4] = 7;
    assert(xhci_extended_caps(&hc, 0x40u << 16));
    assert(mmio[0x100 / 4] & (1u << 24));
    assert(!(mmio[0x100 / 4] & (1u << 16)));
    assert(!(mmio[0x104 / 4] & 0xe011));
    for (int p = 1; p <= 4; p++) {
        assert(hc.port_major[p] == 2 && hc.port_slot_type[p] == 7);
    }
    assert(hc.port_major[5] == 0);
    assert(!xhci_extended_caps(&hc, 1u << 16));
    mmio[0x100 / 4] = 0xffffffff;
    assert(!xhci_extended_caps(&hc, 0x40u << 16));
    mmio[1] = 0;
    assert(wait_controller_ready(hc.cap_regs));
    mmio[1] = 1;
    assert(wait_hchalt(hc.cap_regs, true));
    assert(!wait_hchalt(hc.cap_regs, false));
    pci_device_t pci = {0};
    config[0] = 0x1e318086;
    config[0xd4 / 4] = 0x0f;
    config[0xdc / 4] = 0x03;
    xhci_intel_route_ports(&pci);
    assert(config[0xd0 / 4] == 0x0f && config[0xd8 / 4] == 3);
    config[0xd0 / 4] = 0;
    config[0] = 0x12341022;
    xhci_intel_route_ports(&pci);
    assert(config[0xd0 / 4] == 0);
    config[0] = 0x1e318086;
    config[0x2c / 4] = 0x90a8104d;
    xhci_intel_route_ports(&pci);
    assert(config[0xd0 / 4] == 0);
    return 0;
}
''')
command = section(source, 'static int xhci_submit_command(', 'void xhci_set_tr_dequeue(')
run_c('''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#define TRB_ADDRESS_DEVICE 11
struct xhci_trb { uint64_t param; uint32_t status, control; };
struct evt_queue { unsigned reads; };
typedef struct {
    bool command_failed;
    struct xhci_trb *cmd_ring;
    uint64_t cmd_ring_phys;
    uint32_t db_regs[1];
    struct evt_queue cmd_evt_q;
    volatile uint8_t *op_regs;
    uint8_t bus, slot, func;
} xhci_controller_t;
static uint64_t clock_ticks;
static uint32_t pci_command = 4;
static unsigned submissions;
static bool xhci_cmd_exec_lock(xhci_controller_t *hc) { (void)hc; return true; }
static void xhci_cmd_exec_unlock(xhci_controller_t *hc) { (void)hc; }
static void evtq_drain(struct evt_queue *q) { q->reads = 0; }
static struct xhci_trb *cmd_ring_enqueue(xhci_controller_t *hc, uint8_t *cycle) {
    submissions++; *cycle = 1; return hc->cmd_ring;
}
static uint64_t hpet_read_counter(void) { return clock_ticks++; }
static uint64_t hpet_ticks_for_ms(uint32_t ms) { return ms; }
static void xhci_poll(xhci_controller_t *hc) { (void)hc; }
static bool evtq_pop(struct evt_queue *q, struct xhci_trb *evt) {
    if (++q->reads == 1) { evt->param = 0x9990; return true; }
    if (clock_ticks >= 1500) { evt->param = 0x1000; evt->status = 1u << 24; return true; }
    return false;
}
static void schedule_yield_safe(void) {}
static uint32_t xhci_read32(volatile uint8_t *b, uint32_t off) {
    return *(volatile uint32_t *)(b + off);
}
static void xhci_write32(volatile uint8_t *b, uint32_t off, uint32_t value) {
    *(volatile uint32_t *)(b + off) = value;
}
static bool wait_hchalt(volatile uint8_t *b, bool halted) { (void)b; return halted; }
static void console_print_string(const char *s) { (void)s; }
static uint32_t pci_read_config(int b, int s, int f, int off) {
    (void)b; (void)s; (void)f; (void)off; return pci_command;
}
static void pci_write_config(int b, int s, int f, int off, uint32_t v) {
    (void)b; (void)s; (void)f; (void)off; pci_command = v;
}
''' + command + '''
int main(void) {
    struct xhci_trb ring = {0}, evt;
    uint32_t registers[16] = {1};
    xhci_controller_t hc = {.cmd_ring = &ring, .cmd_ring_phys = 0x1000,
                            .op_regs = (uint8_t *)registers};
    assert(xhci_submit_command(&hc, 0, 0, TRB_ADDRESS_DEVICE << 10, &evt) == 0);
    assert(evt.param == 0x1000 && !hc.command_failed && clock_ticks >= 1500);
    clock_ticks = 0;
    assert(xhci_submit_command(&hc, 0, 0, 23 << 10, &evt) == -1);
    assert(hc.command_failed && !(registers[0] & 1) && !(pci_command & 4));
    assert(submissions == 2);
    assert(xhci_submit_command(&hc, 0, 0, 23 << 10, &evt) == -1);
    assert(submissions == 2);
    return 0;
}
''')
pmm = (root / 'kernel/src/mm/pmm.c').read_text()
allocator = section(pmm, 'void* pmm_alloc_pages_below(', 'void* pmm_alloc_page_and_clear(')
run_c('''
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <assert.h>
static uint64_t bitmap = 1, total_pages = 64, free_pages = 63;
static int pmm_lock;
static void spin_lock_irqsave(int *lock, uint64_t *flags) { (void)lock; *flags = 0; }
static void spin_unlock_irqrestore(int *lock, uint64_t flags) { (void)lock; (void)flags; }
static bool bitmap_test(uint64_t i) { return (bitmap & (1ull << i)) != 0; }
static void bitmap_set(uint64_t i) { bitmap |= 1ull << i; }
''' + allocator + '''
int main(void) {
    assert(pmm_alloc_pages_below(0, 4096) == NULL);
    assert(pmm_alloc_pages_below(3, 4 * 4096) == (void *)4096);
    assert(free_pages == 60);
    assert(pmm_alloc_pages_below(1, 4 * 4096) == NULL);
    assert(pmm_alloc_pages_below(1, 5 * 4096) == (void *)(4 * 4096));
    assert(pmm_alloc_pages_below(64, 0x100000000ull) == NULL);
    return 0;
}
''')
print('xHCI handoff, protocol caps, routing, command matching/deadlines and DMA allocator passed')
