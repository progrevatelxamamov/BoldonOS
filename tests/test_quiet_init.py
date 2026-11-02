"""Routine exec/init and USB enumeration traces stay off; failures stay visible."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
init = (root / 'kernel/src/sys/init_runner.c').read_text()
elf = (root / 'kernel/src/sys/elf.c').read_text()
usb = (root / 'kernel/src/usb/lp/backend.c').read_text()
compat = (root / 'kernel/usb-compat/libpayload.h').read_text()
platform = (root / 'kernel/src/usb/libpayload_platform.c').read_text()

assert 'console_print_string("[EXEC] IRET target SS=0x1B' not in elf
assert '[INIT] Starting /etc/init.sh' not in init
assert '[INIT] C: device=' not in init
assert '[INIT] /proc/partitions missing' in init
assert '[INIT] execve /bin/sh (' in init
assert '#define USB_DEBUG' not in compat
for message in ('initializing PCI', 'controller ready', 'controllers ready',
                'HID ready', 'disk %s sectors=', 'removed disk %s'):
    assert message not in usb, message
assert '[LPUSB] controller initialization failed' in usb
assert '[LPUSB] fatal backend error' in platform
print('Routine init traces disabled, failures retained')
