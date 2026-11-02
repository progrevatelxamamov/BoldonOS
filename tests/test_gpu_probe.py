"""PCI display inventory and read-only /dev/gpuinfo use production functions."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
pci = (root / 'kernel/src/pci/pci.c').read_text()
fb = (root / 'kernel/src/dev/fbdev.c').read_text()
probe = pci[pci.index('uint32_t pci_display_devices('):pci.index('uint8_t pci_find_capability(')]
read = fb[fb.index('/* Hardware-independent first step'):fb.index('void fbdev_init(void)')]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pci/pci.h>
typedef struct { uint64_t offset; } vfs_file_t;
typedef struct { int (*read)(vfs_file_t *, void *, uint64_t); } vfs_file_ops_t;
struct limine_framebuffer { uint64_t width, height, bpp, pitch; };
static struct limine_framebuffer screen = {1920, 1080, 32, 7680};
static struct limine_framebuffer *fb_current(void) { return &screen; }
static pci_device_t entries[4];
static pci_device_t *devices[1024] = {&entries[0], &entries[1], &entries[2], &entries[3]};
static uint64_t pci_count = 4;
uint32_t pci_read_config(uint8_t bus, uint8_t slot, uint8_t fn, uint8_t off) {
    (void)bus; (void)fn;
    assert(off == 0 || off == 8);
    if (off == 8) return 0x12;
    return slot == 1 ? 0x12348086 : slot == 2 ? 0xabcd1002 : 0xffffffff;
}
static uint8_t pci_get_revision(uint8_t bus, uint8_t slot, uint8_t fn) {
    return (uint8_t)pci_read_config(bus, slot, fn, 8);
}
''' + probe + read + r'''
int main(void) {
    entries[0] = (pci_device_t){ .base_class = 1, .slot = 0 };
    entries[1] = (pci_device_t){ .base_class = 3, .slot = 1, .bus = 0 };
    entries[2] = (pci_device_t){ .base_class = 3, .slot = 2, .bus = 3, .func = 1 };
    entries[3] = (pci_device_t){ .base_class = 3, .slot = 3 };
    pci_display_id_t found[2] = {{0}};
    assert(pci_display_devices(NULL, 2) == 0);
    assert(pci_display_devices(found, 0) == 0);
    assert(pci_display_devices(found, 1) == 1);
    assert(found[0].vendor == 0x8086 && found[0].device == 0x1234);
    assert(pci_display_devices(found, 2) == 2);
    assert(found[1].bus == 3 && found[1].func == 1 && found[1].vendor == 0x1002);
    vfs_file_t file = {0}; char data[1024] = {0};
    assert(gpuinfo_ops.read(&file, data, 17) == 17);
    assert(gpuinfo_ops.read(&file, data + 17, sizeof(data) - 17) > 0);
    assert(strstr(data, "backend=limine-fb drm=none vblank=none\n"));
    assert(strstr(data, "bootfb=1920x1080 bpp=32 pitch=7680\n"));
    assert(strstr(data, "pci=00:01.0 vendor=8086 device=1234 subclass=00 rev=12\n"));
    assert(strstr(data, "pci=03:02.1 vendor=1002 device=abcd"));
    assert(!strstr(data, "pci=00:03.0"));
    assert(gpuinfo_ops.read(&file, data, sizeof(data)) == 0);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    src = Path(directory) / 'test.c'
    exe = Path(directory) / 'test'
    src.write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-idirafter', str(root / 'kernel/include'), str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('read-only multi-GPU PCI and boot framebuffer inventory passed')
