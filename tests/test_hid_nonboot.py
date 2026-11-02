"""Compile the production HID descriptor parser against protocol-zero mice.

A report-protocol mouse may have an 8/64-byte endpoint, so classifying by
interrupt packet size (as a boot keyboard) silently loses the mouse.
"""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
source = (root / 'kernel/src/pci/xhci.c').read_text()
header = (root / 'kernel/include/pci/xhci.h').read_text()
structs = header[header.index('struct hid_field {'):header.index('#define TRB_NORMAL')]
parser = source[source.index('typedef struct {\n    uint32_t usage_page;'):source.index('static int32_t hid_extract_field(')]
parser += source[source.index('static void hid_parse_report_descriptor('):source.index('static void handle_keyboard_report(')]
program = '''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define HID_REPORT_QUEUE 8
#define COUNT_OF_TRB 64
struct xhci_trb { uint64_t param; uint32_t status, control; };
typedef struct xhci_controller xhci_controller_t;
''' + structs + parser + '''
int main(void) {
    // Standard mouse: 3 buttons, padding, relative X/Y/wheel.
    const uint8_t desc[] = {
        5,1,9,2,0xA1,1,9,1,0xA1,0,5,9,0x19,1,0x29,3,0x15,0,0x25,1,
        0x95,3,0x75,1,0x81,2,0x95,1,0x75,5,0x81,1,5,1,9,0x30,9,0x31,
        9,0x38,0x15,0x81,0x25,0x7F,0x75,8,0x95,3,0x81,6,0xC0,0xC0
    };
    for (unsigned protocol = 0; protocol <= 2; protocol += 2) {
        struct usb_hid hid = {0};
        hid.protocol = protocol;
        hid_parse_report_descriptor(&hid, desc, sizeof(desc));
        assert(hid.mouse_layout_valid && !hid.kbd_layout_valid);
        assert(hid.mouse_buttons.bit_offset == 0 && hid.mouse_x.bit_offset == 8);
        assert(hid.mouse_y.bit_offset == 16 && hid.mouse_report_bits == 32);
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = pathlib.Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Werror', '-Wno-unused-function',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)

# The native endpoint must resolve protocol 0 *after* descriptor parsing.
assert source.index('hid_parse_report_descriptor(hid, desc_buf, fetch_len)') < source.index('if (hid->protocol == 0) {')
assert 'int pret = protocol == 0 ? -1 : xhci_control_transfer' in source
print('Non-boot mouse descriptor and native classification checks passed')
