"""Keep QEMU keyboard / USB polling independent of timed-sleep wakeups.

Hardware confirmation is needed before enabling timed sleep for input workers
again. The timer-backed version introduced in PR #46 regressed QEMU input.
"""
from pathlib import Path

root = Path(__file__).resolve().parents[1] / 'kernel/src'
for file, start, end, expected in (
    ('sys/init_runner.c', 'static void pty_input_task(', 'void launch_init_process(', 'schedule_yield();'),
    ('usb/libpayload_platform.c', 'static void libpayload_task(', 'void libpayload_usb_start(', 'schedule_yield();'),
    ('pci/xhci.c', 'static void xhci_pool_task(', 'void parse_and_configure_device(', 'schedule_yield_safe();'),
):
    text = (root / file).read_text()
    section = text[text.index(start):text.index(end, text.index(start))]
    assert expected in section, file
    assert 'task_sleep_ms(' not in section, file
print('PTY and USB input polling do not depend on timed wakeups')
