"""Exercise production libpayload endpoint selection for composite HID."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] /
          'kernel/third_party/libpayload/drivers/usb/usb.c').read_text()
block = source[source.index('/* Normalize bInterval'):source.index('usbdev_t *\ngeneric_set_address(')]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define DT_CFG 2
#define DT_INTF 4
#define DT_ENDP 5
typedef uint8_t u8;
typedef enum { FULL_SPEED = 0, LOW_SPEED, HIGH_SPEED, SUPER_SPEED, SUPER_SPEED_PLUS } usb_speed;
typedef enum { CONTROL, ISOCHRONOUS, BULK, INTERRUPT } endpoint_type;
typedef enum { SETUP, IN, OUT } direction_t;
typedef struct { uint8_t bLength, bDescriptorType, bInterfaceNumber, bAlternateSetting,
    bNumEndpoints, bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol, iInterface;
} __attribute__((packed)) interface_descriptor_t;
typedef struct { uint8_t bLength, bDescriptorType, bEndpointAddress, bmAttributes;
    uint16_t wMaxPacketSize; uint8_t bInterval;
} __attribute__((packed)) endpoint_descriptor_t;
typedef struct { uint16_t idVendor, idProduct; } device_descriptor_t;
struct usbdev;
typedef struct { struct usbdev *dev; int endpoint; direction_t direction;
    int toggle, maxpacketsize; endpoint_type type; int interval; } endpoint_t;
typedef struct usbdev { endpoint_t endpoints[32]; int num_endp;
    usb_speed speed; device_descriptor_t *descriptor; } usbdev_t;
static int extras;
static void inout_usb_trace(const char *s, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    (void)a; (void)b; (void)c; (void)d;
    if (s[3] == 'i') extras++;
}
''' + block + r'''
int main(void) {
    uint8_t config[] = {
        9,2,87,0,3,1,0,0x80,50,
        9,4,0,0,1,3,1,1,0, 7,5,0x81,3,8,0,10,
        9,4,0,1,1,3,1,1,0, 7,5,0x84,3,8,0,10, // alternate: skip
        9,4,1,0,3,3,1,2,0, 7,5,0x82,3,8,0,10, // mouse: include
        7,5,0x02,3,8,0,10, // interrupt OUT: not used by adapter
        7,5,0x85,3,0,2,10, // 512-byte IN: unsupported by adapter
        9,4,2,0,1,0xff,0,0,0, 7,5,0x83,3,8,0,10 // unrelated: skip
    };
    device_descriptor_t dd = {.idVendor = 0x062a, .idProduct = 0x4180};
    usbdev_t dev = {.speed = FULL_SPEED, .descriptor = &dd};
    usb_gather_config_endpoints(&dev, config + 18, config + sizeof(config), 1);
    assert(dev.num_endp == 3 && dev.endpoints[1].endpoint == 0x81);
    assert(dev.endpoints[2].endpoint == 0x82);
    assert(dev.endpoints[1].dev == &dev && dev.endpoints[2].dev == &dev);
    assert(extras == 3); // alternate, mouse and vendor interfaces were seen
    usb_gather_config_endpoints(&dev, config + 18, config + sizeof(config), 0);
    assert(dev.num_endp == 2 && dev.endpoints[1].endpoint == 0x81);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as temp:
    src = Path(temp) / 'test.c'
    exe = Path(temp) / 'test'
    src.write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('Composite HID endpoint configuration and unrelated/alternate filtering passed')
