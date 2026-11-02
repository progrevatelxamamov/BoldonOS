"""Guard against accidentally making periodic workers runnable 100% of the time."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
scheduler = (root / 'kernel/src/scheduler/scheduler.c').read_text()
header = (root / 'kernel/include/scheduler/scheduler.h').read_text()
assert 'uint64_t sleep_deadline_ms;' in header
assert 'proc->sleep_deadline_ms = now + ms;' in scheduler
assert 'thread->state = TASK_SLEEPING;' in scheduler
assert 'schedule_yield();' in scheduler[scheduler.index('void task_sleep_ms('):scheduler.index('void task_wake(')]
assert '(int64_t)(now_ms - p->sleep_deadline_ms) < 0' in scheduler
assert 'p->threads[j]->state = TASK_WAITING;' in scheduler
assert 'proc->sleep_deadline_ms = 0;' in scheduler[scheduler.index('void task_wake('):scheduler.index('void task_sleep_thread(')]
assert '"sti; hlt"' in scheduler

for file, worker, end, sleep in [
    ('usb/libpayload_platform.c', 'static void libpayload_task(', 'void libpayload_usb_start(', 'task_sleep_ms(2);'),
    ('pci/xhci.c', 'static void xhci_hotplug_task(', 'void parse_and_configure_device(', 'task_sleep_ms(5);'),
    ('disk/hotplug.c', 'static void hotplug_task(', 'void partitions_hotplug_start(', 'task_sleep_ms(20);'),
    ('inout/inout.c', 'void inout_keyboard_task(', 'void inout_call_scheduler(', 'task_sleep_ms(10);'),
    ('sys/init_runner.c', 'static void pty_input_task(', 'void launch_init_process(', 'task_sleep_ms(2);'),
    ('gui/gui.c', 'void gui_server(', 'void gui_init(', 'task_sleep_ms(8);'),
]:
    source = (root / 'kernel/src' / file).read_text()
    section = source[source.index(worker):source.index(end, source.index(worker))]
    assert sleep in section, file
print('Timer wakeups, idle/hlt and periodic worker blocking checks passed')
