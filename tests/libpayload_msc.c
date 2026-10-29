#include <usb/usb.h>
#include <usb/usbmsc.h>
#include <usb/usbdisk.h>
#undef malloc
#undef calloc
#undef free
#undef printf
#undef assert
#include <assert.h>
extern void *malloc(size_t);
extern void free(void *);
extern int puts(const char *);
extern int lp_usb_msc_rw(usbdev_t *, uint32_t, uint32_t, cbw_direction, u8 *);
extern int lp_usb_msc_sync(usbdev_t *);
static uint32_t tag, chunk_lba[8], chunk_count[8];
static unsigned commands, syncs, tur, spinups, capacities, creates, removes, detached;
static uint64_t now_ms;
static bool no_media, fail_transport;
static usbdev_t *detaching_dev;
extern void test_msc_poll(usbdev_t *dev);
static uint8_t opcode;
static uint32_t read32le(const uint8_t *p) { return p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static uint32_t read32be(const uint8_t *p) { return p[3] | (uint32_t)p[2]<<8 | (uint32_t)p[1]<<16 | (uint32_t)p[0]<<24; }
static int bulk(endpoint_t *ep, int n, uint8_t *data, int finalize) {
    (void)finalize;
    if (ep->direction == OUT && n == 31) {
        assert(read32le(data) == 0x43425355);
        tag = read32le(data + 4);
        opcode = data[15];
        assert(data[14] == ((opcode == 0 || opcode == 3 || opcode == 0x1b) ? 6 : 10));
        if (fail_transport) return -1;
        if (opcode == 0x35) { syncs++; assert(read32le(data + 8) == 0); }
        else if (opcode == 0) tur++;
        else if (opcode == 0x1b) spinups++;
        else if (opcode == 0x25) capacities++;
        else if (opcode == 3) {}
        else {
            assert(opcode == 0x28 || opcode == 0x2A);
            chunk_lba[commands] = read32be(data + 17);
            chunk_count[commands++] = ((uint32_t)data[22] << 8) | data[23];
        }
    } else if (ep->direction == IN && n == 13) {
        const uint32_t csw[] = {0x53425355, tag, 0};
        memcpy(data, csw, sizeof(csw)); data[12] = opcode == 0 && no_media ? 1 : 0;
    } else {
        assert(n <= 65536);
        if (ep->direction == IN) {
            memset(data, 0xA5, n);
            if (opcode == 3) { memset(data, 0, n); data[2] = 2; data[12] = 0x3A; }
            if (opcode == 0x25) {
                const uint8_t capacity[] = {0,0,0x0F,0xFF, 0,0,0x10,0};
                memcpy(data, capacity, sizeof(capacity));
            }
        }
    }
    return n;
}
int clear_stall(endpoint_t *ep) { (void)ep; return 0; }
void usb_detach_device(hci_t *hc, int address) { (void)hc; (void)address; detached++; detaching_dev->data = NULL; }
void usbdisk_create(usbdev_t *dev) { (void)dev; creates++; }
void usbdisk_remove(usbdev_t *dev) { (void)dev; removes++; }
int lp_gettimeofday(struct timeval *tv, void *tz) {
    (void)tz; tv->tv_sec = now_ms / 1000; tv->tv_usec = (now_ms % 1000) * 1000; return 0;
}
static int control(usbdev_t *dev, direction_t dir, int sz, void *req, int n, uint8_t *data) {
    (void)dev; (void)dir; (void)sz; (void)req; (void)n; (void)data; return -1;
}
int lp_printf(const char *fmt, ...) { (void)fmt; return 0; }
int lp_vprintf(const char *fmt, va_list ap) { (void)fmt; (void)ap; return 0; }
void lp_udelay(unsigned us) { now_ms += (us + 999) / 1000; }
int main(void) {
    hci_t hc = {.bulk = bulk, .control = control};
    endpoint_t in = {.direction = IN}, out = {.direction = OUT};
    usbmsc_inst_t msc = {.blocksize = 4096, .bulk_in = &in, .bulk_out = &out};
    usbdev_t dev = {.data = &msc, .controller = &hc};
    in.dev = out.dev = &dev;
    detaching_dev = &dev;
    uint8_t *buffer = malloc(33 * 4096);
    assert(buffer);
    assert(lp_usb_msc_rw(&dev, 0x90000000, 33, cbw_direction_data_in, buffer) == 0);
    assert(commands == 3 && chunk_count[0] == 16 && chunk_count[1] == 16 && chunk_count[2] == 1);
    assert(chunk_lba[0] == 0x90000000 && chunk_lba[1] == 0x90000010 && chunk_lba[2] == 0x90000020);
    assert(buffer[0] == 0xA5 && buffer[33*4096 - 1] == 0xA5);
    assert(lp_usb_msc_rw(&dev, 2, 1, cbw_direction_data_out, buffer) == 0);
    assert(opcode == 0x2A);
    assert(lp_usb_msc_sync(&dev) == 0 && syncs == 1);
    msc.ready = USB_MSC_READY;
    msc.num_luns = 1;
    msc.usbdisk_created = 1;
    test_msc_poll(&dev);
    assert(tur == 1 && spinups == 0 && capacities == 0);
    for (unsigned i = 0; i < 10000; i++) test_msc_poll(&dev);
    assert(tur == 1);
    now_ms = 999; test_msc_poll(&dev); assert(tur == 1);
    now_ms = 1000; test_msc_poll(&dev); assert(tur == 2);
    no_media = true;
    now_ms = 2000; test_msc_poll(&dev);
    assert(msc.ready == USB_MSC_NOT_READY && removes == 1);
    now_ms = 3000; test_msc_poll(&dev);
    assert(msc.ready == USB_MSC_NOT_READY && removes == 1 && spinups == 0 && capacities == 0);
    no_media = false;
    now_ms = 4000; test_msc_poll(&dev);
    assert(msc.ready == USB_MSC_READY && creates == 1 && spinups == 1 && capacities == 1);
    assert(msc.blocksize == 4096 && msc.numblocks == 4096);
    now_ms = 5000; test_msc_poll(&dev);
    assert(spinups == 1 && capacities == 1);
    fail_transport = true;
    now_ms = 6000; test_msc_poll(&dev);
    assert(detached == 1 && !dev.data);
    free(buffer);
    puts("libpayload polling deadline, quiet ready/empty media, reinsertion and detach passed");
    puts("libpayload real BOT read/write chunking, unsigned LBA and cache sync passed");
    return 0;
}
