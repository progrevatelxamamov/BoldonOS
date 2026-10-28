import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
source = (root / 'kernel/src/pci/xhci.c').read_text()
header = (root / 'kernel/include/pci/xhci.h').read_text()
structures = header[header.index('struct hid_field {'):header.index('#define TRB_NORMAL')]
capture = source[source.index('static void hid_capture_report(struct usb_hid *hid, const struct xhci_trb *evt, uint32_t len) {'):source.index('void xhci_poll(xhci_controller_t *hc) {')]
task = source[source.index('static void usb_hid_task(void *arg) {'):source.index('static bool xhci_configure_hid_endpoint(')]
program = '''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <setjmp.h>
#define HID_REPORT_QUEUE 8
#define COUNT_OF_TRB 256
#define TRB_LINK 6
#define TRB_NORMAL 1
struct xhci_trb { uint64_t param; uint32_t status, control; };
typedef struct {
    uint32_t ep_lock[2][4];
    uint32_t db_regs[2];
} xhci_controller_t;
''' + structures + '''
static jmp_buf finished;
static struct usb_hid *active;
static unsigned iterations, resets, mouse_reports;
static uint32_t now;
static uint64_t get_monotonic_ms(void) { return now; }
static bool xhci_lock_timeout(volatile uint32_t *lock, uint32_t spins) {
    (void)spins; assert(!*lock); *lock = 1; return true;
}
static void xhci_unlock(volatile uint32_t *lock) { assert(*lock); *lock = 0; }
static void usb_msd_clear_halt(xhci_controller_t *hc, int slot, int ep) {
    (void)hc; (void)slot; (void)ep;
}
static void xhci_reset_endpoint(xhci_controller_t *hc, int slot, int dci) {
    (void)hc; (void)slot; (void)dci; resets++;
}
static void xhci_set_tr_dequeue(xhci_controller_t *hc, int slot, int dci,
                                uint64_t ring, int cycle) {
    (void)hc; (void)slot; (void)dci; (void)ring; (void)cycle;
}
static bool usb_hid_send_current_leds(struct usb_hid *hid, uint8_t leds) {
    (void)hid; (void)leds; return true;
}
static void handle_keyboard_report(struct usb_hid *hid, uint8_t *data, int len) {
    (void)hid; (void)data; (void)len;
}
static void handle_mouse_report(struct usb_hid *hid, uint8_t *data, int len) {
    (void)hid; assert(len == 4 && data[1] == 1); mouse_reports++;
}
static void *get_current_process(void) { return 0; }
static void task_sleep(void *process) { (void)process; assert(0); }
static void schedule_yield_safe(void);
''' + capture + task + '''
static void schedule_yield_safe(void) {
    iterations++;
    now += 1000;
    assert(active->transfer_pending);
    if (iterations <= 10) {
        assert(resets == 0 && active->enqueue == 1);
    }
    if (iterations == 10) {
        struct xhci_trb evt = {.param = active->pending_trb_phys};
        active->report[1] = 1;
        hid_capture_report(active, &evt, 4);
    }
    if (iterations == 11) {
        assert(mouse_reports == 1 && active->enqueue == 2 && resets == 0);
    }
    if (iterations == 20) {
        assert(resets == 0 && active->enqueue == 2);
        struct xhci_trb evt = {.param = active->pending_trb_phys + 16};
        hid_capture_report(active, &evt, 0);
        assert(active->transfer_pending);
        evt.param = active->pending_trb_phys;
        hid_capture_report(active, &evt, 0);
        assert(!active->transfer_pending);
    }
    if (iterations == 21) {
        assert(active->enqueue == 3 && resets == 0 && mouse_reports == 1);
        struct xhci_trb evt = {.param = active->pending_trb_phys};
        hid_capture_fail(active, &evt);
        assert(active->halted);
    }
    if (iterations == 22) {
        assert(resets == 1 && active->enqueue == 1);
        longjmp(finished, 1);
    }
    assert(iterations < 23);
}
int main(void) {
    static xhci_controller_t hc;
    static struct usb_hid hid;
    static struct xhci_trb ring[COUNT_OF_TRB];
    static uint8_t report[4];
    hid.hc = &hc;
    hid.protocol = 2;
    hid.slot_id = 1;
    hid.dci = 3;
    hid.ep_addr = 0x81;
    hid.ring = ring;
    hid.ring_phys = 0x1000;
    hid.report = report;
    hid.report_size = sizeof(report);
    hid.cycle = 1;
    active = &hid;
    if (setjmp(finished) == 0) usb_hid_task(&hid);
    assert(iterations == 22);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    cfile = pathlib.Path(directory) / 'hid.c'
    executable = pathlib.Path(directory) / 'hid'
    cfile.write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    str(cfile), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=10)
print('HID idle, resumed motion, zero-length completion and error recovery passed')
