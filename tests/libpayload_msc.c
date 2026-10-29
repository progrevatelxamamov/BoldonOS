#include <usb/usb.h>
#include <usb/usbmsc.h>
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
static unsigned commands, syncs;
static uint8_t opcode;
static uint32_t read32le(const uint8_t *p) { return p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static uint32_t read32be(const uint8_t *p) { return p[3] | (uint32_t)p[2]<<8 | (uint32_t)p[1]<<16 | (uint32_t)p[0]<<24; }
static int bulk(endpoint_t *ep, int n, uint8_t *data, int finalize) {
    (void)finalize;
    if (ep->direction == OUT && n == 31) {
        assert(read32le(data) == 0x43425355);
        tag = read32le(data + 4);
        assert(data[14] == 10);
        opcode = data[15];
        if (opcode == 0x35) { syncs++; assert(read32le(data + 8) == 0); }
        else {
            assert(opcode == 0x28 || opcode == 0x2A);
            chunk_lba[commands] = read32be(data + 17);
            chunk_count[commands++] = ((uint32_t)data[22] << 8) | data[23];
        }
    } else if (ep->direction == IN && n == 13) {
        const uint32_t csw[] = {0x53425355, tag, 0};
        memcpy(data, csw, sizeof(csw)); data[12] = 0;
    } else {
        assert(n <= 65536);
        if (ep->direction == IN) memset(data, 0xA5, n);
    }
    return n;
}
int clear_stall(endpoint_t *ep) { (void)ep; return 0; }
void usb_detach_device(hci_t *hc, int address) { (void)hc; (void)address; assert(0); }
int lp_printf(const char *fmt, ...) { (void)fmt; return 0; }
int lp_vprintf(const char *fmt, va_list ap) { (void)fmt; (void)ap; return 0; }
void lp_udelay(unsigned us) { (void)us; }
int main(void) {
    hci_t hc = {.bulk = bulk};
    endpoint_t in = {.direction = IN}, out = {.direction = OUT};
    usbmsc_inst_t msc = {.blocksize = 4096, .bulk_in = &in, .bulk_out = &out};
    usbdev_t dev = {.data = &msc, .controller = &hc};
    in.dev = out.dev = &dev;
    uint8_t *buffer = malloc(33 * 4096);
    assert(buffer);
    assert(lp_usb_msc_rw(&dev, 0x90000000, 33, cbw_direction_data_in, buffer) == 0);
    assert(commands == 3 && chunk_count[0] == 16 && chunk_count[1] == 16 && chunk_count[2] == 1);
    assert(chunk_lba[0] == 0x90000000 && chunk_lba[1] == 0x90000010 && chunk_lba[2] == 0x90000020);
    assert(buffer[0] == 0xA5 && buffer[33*4096 - 1] == 0xA5);
    assert(lp_usb_msc_rw(&dev, 2, 1, cbw_direction_data_out, buffer) == 0);
    assert(opcode == 0x2A);
    assert(lp_usb_msc_sync(&dev) == 0 && syncs == 1);
    free(buffer);
    puts("libpayload real BOT read/write chunking, unsigned LBA and cache sync passed");
    return 0;
}
