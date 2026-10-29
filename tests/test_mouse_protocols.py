import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def section(text, start, end):
    return text[text.index(start):text.index(end, text.index(start))]


def run_c(text):
    with tempfile.TemporaryDirectory() as directory:
        source = pathlib.Path(directory) / 'test.c'
        binary = pathlib.Path(directory) / 'test'
        source.write_text(text)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


source = (ROOT / 'kernel/src/inout/inout.c').read_text()
client = section(source, 'typedef struct {\n    uint8_t resp[8];',
                 'static mouse_client_t default_mouse_client;')
response = section(source, 'static void mouse_response(',
                   'static mouse_client_t *mouse_client_of(')
run_c('''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#define MOUSE_MODE_PS2 0
#define MOUSE_MODE_IMPS 1
#define MOUSE_MODE_EXPS 2
typedef struct { int8_t dx, dy, dz; uint8_t buttons; } mouse_motion_t;
''' + client + '''
static bool motion_pop_locked(mouse_motion_t *m) { (void)m; return false; }
static void mouse_encode_packet(mouse_client_t *c, const mouse_motion_t *m) {
    (void)c; (void)m;
}
''' + response + '''
int main(void) {
    mouse_client_t c = {0};
    uint8_t sequence[] = {0xf3, 200, 0xf3, 100, 0xf3, 80};
    for (unsigned i = 0; i < sizeof(sequence); i++) mouse_response(&c, sequence[i]);
    assert(c.mode == MOUSE_MODE_IMPS);
    assert(c.resp_len == 1 && c.resp[0] == 0xfa);
    mouse_response(&c, 0xf2);
    assert(c.resp_len == 2 && c.resp[0] == 0xfa && c.resp[1] == 3);
    mouse_response(&c, 0xff);
    assert(c.mode == MOUSE_MODE_PS2 && c.resp_len == 3);
    uint8_t explorer[] = {0xf3, 200, 0xf3, 200, 0xf3, 80};
    for (unsigned i = 0; i < sizeof(explorer); i++) mouse_response(&c, explorer[i]);
    mouse_response(&c, 0xf2);
    assert(c.resp[1] == 4);
    return 0;
}
''')
source = (ROOT / 'kernel/src/inout/ps2mouse.c').read_text()
feed = source[source.index('void ps2mouse_feed('):]
run_c('''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#define INOUT_MOUSE_LEFTBTN 1
#define INOUT_MOUSE_RIGHTBTN 2
#define INOUT_MOUSE_CENTERBTN 3
static uint8_t packet[4], prev_btns;
static int packet_index, packet_size = 3;
static uint64_t last_byte_ms;
static bool stream_ready = true;
static int x, y, z, moves;
static uint64_t get_monotonic_ms(void) { return 0; }
static void emit_button(int b, bool d) { (void)b; (void)d; }
static void emit_move(int dx, int dy, int dz) { x=dx; y=dy; z=dz; moves++; }
''' + feed + '''
int main(void) {
    ps2mouse_feed(0x38); ps2mouse_feed(0xff); ps2mouse_feed(0xff);
    assert(moves == 1 && x == -1 && y == 1);
    ps2mouse_feed(0x08); ps2mouse_feed(0xc0); ps2mouse_feed(0x40);
    assert(moves == 2 && x == 192 && y == -64);
    packet_size = 4;
    ps2mouse_feed(0x38); ps2mouse_feed(0x80); ps2mouse_feed(0x80); ps2mouse_feed(0xff);
    assert(moves == 3 && x == -128 && y == 128 && z == -1);
    return 0;
}
''')
print('Mouse protocol regression tests passed')

source = (ROOT / 'kernel/src/pci/xhci.c').read_text()
wait = section(source, '#define XHCI_BOOT_TIMEOUT_LIMIT', 'int xhci_wait_cmd(')
run_c('''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#define __ATOMIC_RELAXED 0
typedef struct { bool command_failed; } xhci_controller_t;
struct xhci_trb { int unused; };
struct evt_queue { int ready; };
static uint64_t clock_ticks;
static bool timer = true;
static unsigned polls;
static uint64_t hpet_read_counter(void) { return clock_ticks++; }
static uint64_t hpet_ticks_for_ms(uint32_t ms) { return timer ? ms : 0; }
static bool interrupts_enabled(void) { return false; }
static void schedule_yield_safe(void) {}
static void console_print_string(const char *s) { (void)s; }
static void xhci_poll(xhci_controller_t *hc) { (void)hc; polls++; }
static bool evtq_pop(struct evt_queue *q, struct xhci_trb *evt) {
    (void)evt; return q->ready;
}
''' + wait + '''
int main(void) {
    xhci_controller_t hc = {0};
    struct xhci_trb evt;
    struct evt_queue q = {1};
    xhci_enum_begin();
    assert(wait_event_from(&hc, &q, &evt) == 0);
    q.ready = 0;
    assert(wait_event_from(&hc, &q, &evt) == -1);
    assert(clock_ticks < 200 && polls < 200);
    clock_ticks = 800;
    assert(xhci_boot_exhausted());
    xhci_enum_end();
    timer = false;
    xhci_enum_begin();
    polls = 0;
    assert(wait_event_from(&hc, &q, &evt) == -1);
    assert(polls > 0 && polls <= 3125);
    xhci_mark_boot_complete();
    assert(!xhci_boot_exhausted());
    return 0;
}
''')
print('xHCI wait deadline regression tests passed')
