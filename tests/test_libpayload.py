import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
KERNEL = ROOT / 'kernel'
INCLUDES = ['-I' + str(KERNEL / p) for p in (
    'usb-compat', 'third_party/libpayload/include',
    'third_party/libpayload/drivers/usb', 'include', 'limine-protocol/include')]
objects = ['obj/src/usb/lp/backend.c.o', 'obj/src/pci/xhci.c.o',
           'obj/third_party/libpayload/drivers/usb/usbmsc.c.o',
           'obj/src/usb/libpayload_platform.c.o']
subprocess.run(['make', '-C', str(KERNEL), *objects], check=True)
with tempfile.TemporaryDirectory() as directory:
    binary = pathlib.Path(directory) / 'libpayload-test'
    subprocess.run(['cc', '-std=gnu11', '-no-pie', '-ffunction-sections',
                    '-fdata-sections', '-Wl,--gc-sections', *INCLUDES,
                    str(ROOT / 'tests/libpayload_backend.c'),
                    *(str(KERNEL / p) for p in objects[:2]), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    hub = pathlib.Path(directory) / 'libpayload-hub-test'
    subprocess.run(['cc', '-std=gnu11', '-no-pie', '-ffunction-sections',
                    '-fdata-sections', '-Wl,--gc-sections', *INCLUDES,
                    str(ROOT / 'tests/libpayload_hub.c'), '-o', str(hub)], check=True)
    subprocess.run([str(hub)], check=True)
    wrapper = pathlib.Path(directory) / 'msc-wrapper.c'
    wrapper.write_text('#include "' + str(KERNEL / 'third_party/libpayload/drivers/usb/usbmsc.c') +
                       '"\nvoid test_msc_poll(usbdev_t *dev) { usb_msc_poll(dev); }\n')
    msc = pathlib.Path(directory) / 'libpayload-msc-test'
    subprocess.run(['cc', '-std=gnu11', '-no-pie', '-ffunction-sections',
                    '-fdata-sections', '-Wl,--gc-sections', *INCLUDES,
                    str(ROOT / 'tests/libpayload_msc.c'),
                    str(wrapper), '-o', str(msc)], check=True)
    subprocess.run([str(msc)], check=True)
    ehci = pathlib.Path(directory) / 'ehci-cleanup.o'
    subprocess.run(['cc', '-std=gnu11', '-fno-pie', '-ffunction-sections',
                    '-fdata-sections', *INCLUDES, '-c',
                    str(ROOT / 'tests/libpayload_ehci_cleanup.c'),
                    '-o', str(ehci)], check=True)
    memory = pathlib.Path(directory) / 'libpayload-memory-test'
    subprocess.run(['cc', '-std=gnu11', '-no-pie', '-ffunction-sections',
                    '-fdata-sections', '-Wl,--gc-sections',
                    '-I' + str(KERNEL / 'include'),
                    '-I' + str(KERNEL / 'limine-protocol/include'),
                    str(ROOT / 'tests/libpayload_memory.c'),
                    str(KERNEL / objects[3]), str(ehci), '-o', str(memory)], check=True)
    subprocess.run([str(memory)], check=True)

pci = (KERNEL / 'src/pci/pci.c').read_text()
boot = (KERNEL / 'src/kernel.c').read_text()
assert 'xhci_init(' not in pci
assert 'libpayload_pci_register(devices[dev])' in pci
assert 'xhci_mark_boot_complete' not in boot
assert 'libpayload_usb_ready()' in boot
print('PCI backend switch and boot gate checks passed')
