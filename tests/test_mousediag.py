"""Read-only mouse diagnostics distinguish absent HID reports from bad decoding."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'kernel/src/inout/inout.c').read_text()
xhci = (root / 'kernel/src/pci/xhci.c').read_text()
destroy = xhci[xhci.index('void boldon_usb_input_destroy('):xhci.index('int boldon_usb_input_led_report(', xhci.index('void boldon_usb_input_destroy('))]
assert 'handle_mouse_report(hid, released, sizeof(released));' in destroy
assert 'inout_note_usb_mouse_report' not in destroy
assert 'inout_note_usb_mouse_report();' in xhci[xhci.index('void boldon_usb_input_report('):xhci.index('void boldon_usb_input_destroy(')]
note = source[source.index('void inout_note_usb_mouse_report('):source.index('/* Enumeration transitions only')]
read = source[source.index('static int mousediag_read('):source.index('static vfs_file_ops_t mousediag_ops', source.index('static int mousediag_read('))]
code = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
static uint64_t usb_mouse_reports;
static uint64_t usb_mouse_polls;
static int usb_mouse_devices;
static uint64_t decoded_mouse_events;
typedef struct { uint64_t offset; } vfs_file_t;
''' + note + read + r'''
int main(void) {
    vfs_file_t file = {0};
    char data[128] = {0};
    assert(mousediag_read(&file, data, 4) == 4);
    assert(mousediag_read(&file, data + 4, sizeof(data) - 4) > 0);
    assert(strcmp(data, "usb_devices=0 usb_polls=0 usb_reports=0 mouse_events=0\n") == 0);
    assert(mousediag_read(&file, data, sizeof(data)) == 0);
    inout_note_usb_mouse_device(1);
    inout_note_usb_mouse_poll();
    inout_note_usb_mouse_report();
    __atomic_fetch_add(&decoded_mouse_events, 1, __ATOMIC_RELAXED);
    file.offset = 0;
    memset(data, 0, sizeof(data));
    assert(mousediag_read(&file, data, sizeof(data)) > 0);
    assert(strcmp(data, "usb_devices=1 usb_polls=1 usb_reports=1 mouse_events=1\n") == 0);
    inout_note_usb_mouse_device(-1);
    assert(usb_mouse_devices == 0);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    src = Path(directory) / 'mousediag.c'
    exe = Path(directory) / 'mousediag'
    src.write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('Mouse diagnostic counters/partial reads passed')
