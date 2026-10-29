#include <usb/usb.h>
#include <usb/usbmsc.h>
#include <usb/usbdisk.h>
#include <platform.h>
#include <disk/block.h>
#include <inout/inout.h>
#include <errno.h>
#undef malloc
#undef calloc
#undef free
#undef printf
#undef assert
#include <assert.h>
extern void *malloc(size_t);
extern void *calloc(size_t, size_t);
extern void free(void *);
extern int posix_memalign(void **, size_t, size_t);
extern int puts(const char *);

static struct block_device registered;
static bool keys[256];
static unsigned key_changes, movements, button_changes, locks;
static int dx, dy, dz;
static uint8_t leds, idle_value = 255;
static unsigned protocol_value = 255, queue_destroyed, unmounted, unregistered;
static uint64_t last_lba;
static uint32_t last_count;
static bool remove_during_io;
static uint8_t packet[8];
static bool packet_ready;
static usbdev_t *disk_dev;
static const uint8_t *report_descriptor;
static size_t report_length;

void *lp_malloc(size_t n) { return calloc(1, n ? n : 1); }
void *lp_calloc(size_t n, size_t s) { return calloc(n, s); }
void *lp_memalign(size_t a, size_t n) { void *p = NULL; return posix_memalign(&p, a, n) ? NULL : p; }
void lp_free(void *p) { free(p); }
void *malloc_alloc(uint64_t n) { return calloc(1, n); }
void malloc_free(void *p) { free(p); }
int lp_printf(const char *fmt, ...) { (void)fmt; return 0; }
void inout_set_key_state(uint64_t code, bool down) {
    assert(code < 256);
    if (keys[code] != down) key_changes++;
    keys[code] = down;
}
void inout_keyboard_request_resync(void) {}
void inout_add_event(inout_event_t ev) {
    assert(ev.type == INOUT_EVENT_MOUSE);
    if (ev.data.mouse.mouse_state == INOUT_MOUSE_MOVE) {
        movements++;
        dx = ev.data.mouse.x; dy = ev.data.mouse.y; dz = ev.data.mouse.z;
    } else button_changes++;
}
void lp_usb_lock(void) { locks++; }
void lp_usb_unlock(void) { assert(locks); locks--; }
int lp_usb_failed(void) { return 0; }
uint8_t lp_platform_leds(void) { return leds; }
void register_device(struct block_device *b) { strcpy(b->name, "usb0"); registered = *b; }
void unregister_device(const char *name) { assert(!strcmp(name, "usb0")); unregistered++; }
void partitions_device_remove(const char *name) { assert(!strcmp(name, "usb0")); unmounted++; }
int lp_usb_msc_sync(usbdev_t *dev) { assert(dev == disk_dev); return 0; }
int lp_usb_msc_rw(usbdev_t *dev, uint32_t lba, uint32_t count, cbw_direction dir, uint8_t *data) {
    assert(dev == disk_dev && data);
    assert(dir == cbw_direction_data_in || dir == cbw_direction_data_out);
    last_lba = lba; last_count = count;
    if (remove_during_io) { usbdisk_remove(dev); return 1; }
    return 0;
}
static int control(usbdev_t *dev, direction_t dir, int sz, void *dr, int n, uint8_t *data) {
    (void)dev; (void)dir;
    assert(sz == 8);
    dev_req_t *r = dr;
    if (r->bRequest == 6 && report_descriptor) {
        assert(n == (int)report_length);
        memcpy(data, report_descriptor, report_length);
    }
    if (r->bRequest == 0x0B) protocol_value = r->wValue;
    if (r->bRequest == 0x0A) idle_value = r->wValue;
    if (r->bRequest == 9) { assert(n == 1); assert(data[0] == leds); }
    return n;
}
static void *create_queue(endpoint_t *ep, int size, int count, int interval) {
    assert(ep->type == INTERRUPT && size == 8 && count == 4 && interval > 0);
    return packet;
}
static void destroy_queue(endpoint_t *ep, void *q) { (void)ep; assert(q == packet); queue_destroyed++; }
static uint8_t *poll_queue(void *q) { assert(q == packet); if (!packet_ready) return NULL; packet_ready = false; return packet; }
static void test_hid(int protocol) {
    hci_t hc = {.control = control, .create_intr_queue = create_queue,
        .destroy_intr_queue = destroy_queue, .poll_intr_queue = poll_queue};
    uint8_t config[] = {9,2,18,0,1,1,0,0x80,50, 9,4,0,0,1,3,1,0,0};
    config[16] = protocol;
    device_descriptor_t descriptor = {.idVendor = 1, .idProduct = 2};
    usbdev_t dev = {.controller = &hc, .configuration = (void *)config, .descriptor = &descriptor, .num_endp = 2};
    dev.endpoints[1] = (endpoint_t){.dev = &dev, .type = INTERRUPT, .direction = IN, .maxpacketsize = 8};
    usb_hid_init(&dev);
    assert(dev.data && dev.poll && dev.destroy);
    assert(protocol_value == 0 && idle_value == 0);
    memset(packet, 0, sizeof(packet));
    if (protocol == 1) { packet[0] = 2; packet[2] = 4; }
    else { packet[0] = 1; packet[1] = 7; packet[2] = (uint8_t)-3; }
    packet_ready = true;
    dev.poll(&dev);
    if (protocol == 1) {
        assert(keys[KEY_LSHIFT] && keys[KEY_A]);
        unsigned before = key_changes;
        for (unsigned n = 0; n < 10000; n++) dev.poll(&dev);
        assert(keys[KEY_LSHIFT] && keys[KEY_A] && before == key_changes);
        leds = 3; dev.poll(&dev);
    } else assert(movements == 1 && dx == 7 && dy == -3 && button_changes == 1);
    dev.destroy(&dev);
    assert(!dev.data);
    if (protocol == 1) assert(!keys[KEY_LSHIFT] && !keys[KEY_A]);
    else assert(button_changes == 2);
}
static void test_report_mouse(void) {
    static const uint8_t descriptor[] = {
        5,1,9,2,0xA1,1,9,1,0xA1,0,5,9,0x19,1,0x29,3,0x15,0,0x25,1,
        0x95,3,0x75,1,0x81,2,0x95,1,0x75,5,0x81,1,5,1,9,0x30,9,0x31,
        9,0x38,0x15,0x81,0x25,0x7F,0x75,8,0x95,3,0x81,6,0xC0,0xC0
    };
    report_descriptor = descriptor; report_length = sizeof(descriptor);
    hci_t hc = {.control = control, .create_intr_queue = create_queue,
        .destroy_intr_queue = destroy_queue, .poll_intr_queue = poll_queue};
    uint8_t config[] = {9,2,27,0,1,1,0,0x80,50, 9,4,0,0,1,3,1,2,0,
        9,0x21,0x11,1,0,1,0x22,sizeof(descriptor),0};
    device_descriptor_t dd = {0};
    usbdev_t dev = {.controller = &hc, .configuration = (void *)config, .descriptor = &dd, .num_endp = 2};
    dev.endpoints[1] = (endpoint_t){.dev = &dev, .type = INTERRUPT, .direction = IN, .maxpacketsize = 8};
    usb_hid_init(&dev);
    assert(dev.data && protocol_value == 1);
    memset(packet, 0, sizeof(packet));
    packet[1] = (uint8_t)-2; packet[2] = 5; packet[3] = 1;
    packet_ready = true; dev.poll(&dev);
    assert(dx == -2 && dy == 5 && dz == 1);
    dev.destroy(&dev);
    report_descriptor = NULL; report_length = 0;
}
static void test_disk(void) {
    usbmsc_inst_t msc = {.numblocks = 0xF0000000, .blocksize = 4096};
    usbdev_t dev = {.data = &msc};
    disk_dev = &dev;
    usbdisk_create(&dev);
    assert(msc.data && registered.sector_size == 4096 && registered.sector_count == 0xF0000000);
    uint8_t buffer[4096];
    assert(registered.read(registered.local_device_struct, 0x90000000, 1, buffer) == 0);
    assert(last_lba == 0x90000000 && last_count == 1 && !locks);
    assert(registered.write(registered.local_device_struct, 0xF0000000 - 1, 2, buffer) == -EINVAL);
    assert(registered.flush(registered.local_device_struct) == 0);
    remove_during_io = true;
    assert(registered.read(registered.local_device_struct, 1, 1, buffer) == -EIO);
    assert(!msc.data && unmounted == 1 && unregistered == 1 && !locks);
    assert(registered.read(registered.local_device_struct, 1, 1, buffer) == -ENODEV);
    assert(registered.flush(registered.local_device_struct) == -ENODEV);
    lp_free(registered.local_device_struct);
}
int main(void) {
    _Static_assert(sizeof(dev_req_t) == 8, "USB setup ABI");
    test_hid(1);
    test_hid(2);
    assert(queue_destroyed == 2);
    test_report_mouse();
    assert(queue_destroyed == 3);
    test_disk();
    puts("libpayload HID, idle, LEDs, disconnect and block lifetime tests passed");
    return 0;
}
