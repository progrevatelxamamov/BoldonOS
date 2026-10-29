#include "../kernel/third_party/libpayload/drivers/usb/generic_hub.c"
#undef malloc
#undef calloc
#undef free
#undef assert
#include <assert.h>
extern void *malloc(size_t);
extern void *calloc(size_t, size_t);
extern void free(void *);
extern int puts(const char *);
static u64 now_ms;
static unsigned resets[3], attached[3], warnings;
static int connected[3] = {0, 1, 1};
static int enabled[3] = {0, 0, 1};
static int changed = 1;
static int changed_port[3] = {0, 1, 1};
void *lp_malloc(size_t n) { return calloc(1, n); }
void *lp_calloc(size_t n, size_t s) { return calloc(n, s); }
void lp_free(void *p) { free(p); }
void lp_fatal(const char *fmt, ...) { (void)fmt; __builtin_trap(); }
int lp_vprintf(const char *fmt, va_list ap) {
    (void)ap;
    if (strstr(fmt, "disabled after")) warnings++;
    return 0;
}
void lp_udelay(unsigned us) { now_ms += (us + 999) / 1000; }
int lp_gettimeofday(struct timeval *tv, void *tz) {
    (void)tz; tv->tv_sec = now_ms / 1000; tv->tv_usec = now_ms % 1000 * 1000; return 0;
}
void usb_detach_device(hci_t *hc, int addr) { (void)hc; (void)addr; }
int usb_attach_device(hci_t *hc, int hub, int port, usb_speed speed) {
    (void)hc; (void)hub; (void)speed; attached[port]++; return port;
}
static int hub_changed(usbdev_t *dev) { (void)dev; return changed; }
static int port_changed(usbdev_t *dev, int port) {
    (void)dev; int result = changed_port[port]; changed_port[port] = 0; return result;
}
static int port_connected(usbdev_t *dev, int port) { (void)dev; return connected[port]; }
static int port_enabled(usbdev_t *dev, int port) { (void)dev; return enabled[port]; }
static usb_speed port_speed(usbdev_t *dev, int port) { (void)dev; (void)port; return HIGH_SPEED; }
static int reset_port(usbdev_t *dev, int port) { (void)dev; resets[port]++; return 0; }
int main(void) {
    const generic_hub_ops_t ops = {.hub_status_changed = hub_changed,
        .port_status_changed = port_changed, .port_connected = port_connected,
        .port_enabled = port_enabled, .port_speed = port_speed, .reset_port = reset_port};
    usbdev_t dev = {0};
    assert(generic_hub_init(&dev, 2, &ops) == 0);
    dev.poll(&dev);
    generic_hub_t *hub = GEN_HUB(&dev);
    assert(resets[1] == 1 && !attached[1] && warnings == 1);
    assert(attached[2] == 1 && hub->ports[2] == 2);
    u64 deadline = hub->retry[1].deadline_ms;
    now_ms = deadline - 1;
    for (unsigned i = 0; i < 10000; i++) { changed_port[1] = 1; dev.poll(&dev); }
    assert(resets[1] == 1 && warnings == 1);
    changed = 0;
    now_ms = deadline;
    dev.poll(&dev);
    assert(resets[1] == 2 && !attached[1] && warnings == 1);
    assert(hub->retry[1].deadline_ms == now_ms + 2000);
    for (unsigned i = 0; i < 10; i++) {
        now_ms = hub->retry[1].deadline_ms;
        dev.poll(&dev);
        assert(hub->retry[1].deadline_ms - now_ms <= 30000);
    }
    assert(warnings == 1 && attached[2] == 1);
    connected[1] = 0; changed = 1; changed_port[1] = 1;
    dev.poll(&dev);
    assert(!hub->retry[1].failures && !hub->retry[1].deadline_ms);
    connected[1] = 1; enabled[1] = 1; changed_port[1] = 1;
    dev.poll(&dev);
    assert(attached[1] == 1 && !hub->retry[1].failures);
    generic_hub_destroy(&dev);
    puts("libpayload hub disabled-port backoff, log suppression, other ports and reconnect passed");
    return 0;
}
