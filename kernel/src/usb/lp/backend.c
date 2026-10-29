#include <usb/usb.h>
#include <usb/usbmsc.h>
#include <usb/usbdisk.h>
#include <platform.h>
#include "xhci.h"
#include "ehci.h"
#include "ohci.h"
#include "uhci.h"
#include <disk/block.h>
#include <disk/partitions.h>
#include <errno.h>

struct lp_transport {
    hci_t *hc;
    int (*control)(usbdev_t *, direction_t, int, void *, int, u8 *);
    int (*bulk)(endpoint_t *, int, u8 *, int);
    int (*bulk_timeout)(endpoint_t *, int, u8 *, int);
};
static struct lp_transport transports[32];
static unsigned transport_count;
extern u32 pci_quirk_check(pcidev_t dev);
extern int lp_usb_msc_sync(usbdev_t *dev);
extern int lp_usb_msc_rw(usbdev_t *, uint32_t, uint32_t, cbw_direction, u8 *);

static struct lp_transport *transport(hci_t *hc) {
    for (unsigned i = 0; i < transport_count; i++) if (transports[i].hc == hc) return &transports[i];
    return NULL;
}

static int safe_control(usbdev_t *dev, direction_t dir, int drlen, void *request, int n, u8 *data) {
    struct lp_transport *t = transport(dev->controller);
    if (!t || drlen < 8 || drlen > 4096 || n < 0 || n > 65535 || (n && !data)) return -1;
    u8 *dr = malloc(drlen), *buffer = n ? memalign(65536, n) : NULL;
    if (!dr || (n && !buffer)) { free(dr); free(buffer); return -1; }
    memcpy(dr, request, drlen);
    if (n && dir != IN) memcpy(buffer, data, n);
    int ret = t->control(dev, dir, drlen, dr, n, buffer);
    if (n && dir == IN && ret > 0) memcpy(data, buffer, MIN(ret, n));
    free(dr); free(buffer);
    return ret;
}

static int safe_bulk_common(endpoint_t *ep, int n, u8 *data, int value, bool timeout) {
    struct lp_transport *t = transport(ep->dev->controller);
    if (!t || n < 0 || n > 65536 || (n && !data)) return -1;
    u8 *buffer = memalign(65536, n ? (size_t)n : 1);
    if (!buffer) return -1;
    if (ep->direction == OUT && n) memcpy(buffer, data, n);
    int ret = timeout ? t->bulk_timeout(ep, n, buffer, value) : t->bulk(ep, n, buffer, value);
    if (ep->direction == IN && ret > 0) memcpy(data, buffer, MIN(ret, n));
    free(buffer);
    return ret;
}
static int safe_bulk(endpoint_t *ep, int n, u8 *data, int finalize) { return safe_bulk_common(ep, n, data, finalize, false); }
static int safe_bulk_timeout(endpoint_t *ep, int n, u8 *data, int us) { return safe_bulk_common(ep, n, data, us, true); }

void lp_backend_initialize(void) {
    const uint8_t order[] = {0x30, 0x20, 0x10, 0x00};
    for (unsigned pass = 0; pass < ARRAY_SIZE(order); pass++) {
        uint32_t pci; uint8_t type; uintptr_t bar;
        for (unsigned i = 0; lp_platform_controller(i, &pci, &type, &bar); i++) {
            if (type != order[pass] || transport_count >= ARRAY_SIZE(transports)) continue;
            printf("[LPUSB] initializing PCI %02x:%02x.%x type=%02x BAR=%lx\n", PCI_BUS(pci), PCI_SLOT(pci), PCI_FUNC(pci), type, (unsigned long)bar);
            lp_platform_prepare(pci, type, bar);
            hci_t *hc = NULL;
            if (type == 0x30) hc = xhci_init(bar);
            else if (type == 0x20) hc = ehci_init(bar);
            else if (type == 0x10) hc = ohci_init(bar);
            else hc = uhci_pci_init(pci);
            if (!hc) { printf("[LPUSB] controller initialization failed\n"); continue; }
            hc->pcidev = pci;
            if (hc->devices[0]) hc->devices[0]->quirks |= pci_quirk_check(pci);
            struct lp_transport *t = &transports[transport_count++];
            *t = (struct lp_transport){hc, hc->control, hc->bulk, hc->bulk_timeout};
            hc->control = safe_control;
            hc->bulk = safe_bulk;
            hc->bulk_timeout = safe_bulk_timeout;
            printf("[LPUSB] controller ready\n");
        }
    }
    printf("[LPUSB] %u controllers ready, polling devices\n", transport_count);
}
void lp_backend_poll(void) { usb_poll(); }

struct lp_hid {
    endpoint_t *ep;
    void *queue;
    void *input;
    uint8_t iface, leds;
};
static void lp_hid_destroy(usbdev_t *dev) {
    struct lp_hid *hid = dev->data;
    if (!hid) return;
    dev->controller->destroy_intr_queue(hid->ep, hid->queue);
    boldon_usb_input_destroy(hid->input);
    free(hid);
    dev->data = NULL;
}
static void lp_hid_poll(usbdev_t *dev) {
    struct lp_hid *hid = dev->data;
    if (!hid) return;
    uint8_t desired = lp_platform_leds();
    if (desired != hid->leds) {
        uint8_t data[64], id = 0;
        int n = boldon_usb_input_led_report(hid->input, desired, data, &id);
        hid->leds = desired;
        if (n > 0) {
            dev_req_t request = {.bmRequestType = 0x21, .bRequest = 9,
                .wValue = 0x0200 | id, .wIndex = hid->iface, .wLength = n};
            dev->controller->control(dev, OUT, sizeof(request), &request, n, data);
        }
    }
    u8 *report;
    unsigned budget = 32;
    while (budget-- && (report = dev->controller->poll_intr_queue(hid->queue)))
        boldon_usb_input_report(hid->input, report, MIN(hid->ep->maxpacketsize, 64));
}

void usb_hid_init(usbdev_t *dev) {
    u8 *ptr = (u8 *)dev->configuration;
    u8 *end = ptr + dev->configuration->wTotalLength;
    interface_descriptor_t *iface = NULL;
    uint16_t report_length = 0;
    while (ptr + 2 <= end && ptr[0] >= 2 && ptr + ptr[0] <= end) {
        if (ptr[1] == DT_INTF && ptr[0] >= sizeof(*iface)) {
            if (iface) break;
            interface_descriptor_t *candidate = (void *)ptr;
            if (candidate->bInterfaceClass == 3) iface = candidate;
        } else if (iface && ptr[1] == 0x21 && ptr[0] >= 9) {
            report_length = ptr[7] | ((uint16_t)ptr[8] << 8);
        }
        ptr += ptr[0];
    }
    endpoint_t *ep = NULL;
    for (int i = 1; i < dev->num_endp; i++)
        if (dev->endpoints[i].type == INTERRUPT && dev->endpoints[i].direction == IN) { ep = &dev->endpoints[i]; break; }
    if (!iface || !ep || ep->maxpacketsize < 3 || ep->maxpacketsize > 64) return;
    struct lp_hid *hid = calloc(1, sizeof(*hid));
    if (!hid) return;
    hid->ep = ep;
    hid->iface = iface->bInterfaceNumber;
    hid->leds = 255;
    u8 *descriptor = NULL;
    int descriptor_length = 0;
    if (report_length && report_length <= 4096) {
        descriptor = malloc(report_length);
        if (descriptor) {
            dev_req_t request = {.bmRequestType = 0x81, .bRequest = 6,
                .wValue = 0x2200, .wIndex = iface->bInterfaceNumber, .wLength = report_length};
            descriptor_length = dev->controller->control(dev, IN, sizeof(request), &request, report_length, descriptor);
        }
    }
    int mode = 0;
    int protocol = iface->bInterfaceSubClass == 1 ? iface->bInterfaceProtocol : 0;
    hid->input = boldon_usb_input_create(protocol, descriptor,
                        descriptor_length > 0 ? (uint32_t)descriptor_length : 0, &mode);
    free(descriptor);
    if (!hid->input) { free(hid); printf("[LPUSB] unsupported HID report layout\n"); return; }
    if (iface->bInterfaceSubClass == 1) {
        dev_req_t request = {.bmRequestType = 0x21, .bRequest = 0x0B,
            .wValue = mode, .wIndex = iface->bInterfaceNumber};
        if (dev->controller->control(dev, OUT, sizeof(request), &request, 0, NULL) < 0 && mode) {
            request.wValue = 0;
            if (dev->controller->control(dev, OUT, sizeof(request), &request, 0, NULL) >= 0)
                boldon_usb_input_protocol(hid->input, 0);
        }
    }
    dev_req_t idle = {.bmRequestType = 0x21, .bRequest = 0x0A, .wIndex = iface->bInterfaceNumber};
    dev->controller->control(dev, OUT, sizeof(idle), &idle, 0, NULL);
    hid->queue = dev->controller->create_intr_queue(ep, ep->maxpacketsize, 4, 10);
    if (!hid->queue) { boldon_usb_input_destroy(hid->input); free(hid); return; }
    dev->data = hid;
    dev->destroy = lp_hid_destroy;
    dev->poll = lp_hid_poll;
    printf("[LPUSB] HID ready %04x:%04x protocol=%d\n", dev->descriptor->idVendor, dev->descriptor->idProduct, protocol);
}

struct lp_disk { usbdev_t *dev; char name[32]; bool removing; };
static int lp_disk_io(void *context, uint64_t lba, uint32_t count, void *buffer, bool write) {
    struct lp_disk *disk = context;
    lp_usb_lock();
    if (!disk->dev || disk->removing || lp_usb_failed()) { lp_usb_unlock(); return -ENODEV; }
    usbmsc_inst_t *msc = MSC_INST(disk->dev);
    if (!buffer || !count || lba >= msc->numblocks ||
        count > msc->numblocks - lba) {
        lp_usb_unlock(); return -EINVAL;
    }
    int result = lp_usb_msc_rw(disk->dev, (uint32_t)lba, count,
                    write ? cbw_direction_data_out : cbw_direction_data_in, buffer);
    lp_usb_unlock();
    return result ? -EIO : 0;
}
static int lp_disk_read(void *context, uint64_t lba, uint32_t count, void *buffer) { return lp_disk_io(context, lba, count, buffer, false); }
static int lp_disk_write(void *context, uint64_t lba, uint32_t count, void *buffer) { return lp_disk_io(context, lba, count, buffer, true); }
static int lp_disk_flush(void *context) {
    struct lp_disk *disk = context;
    lp_usb_lock();
    int result = !disk->dev || disk->removing || lp_usb_failed() ? -ENODEV : lp_usb_msc_sync(disk->dev) ? -EIO : 0;
    lp_usb_unlock();
    return result;
}
void usbdisk_create(usbdev_t *dev) {
    usbmsc_inst_t *msc = MSC_INST(dev);
    if (msc->data || !msc->numblocks || msc->blocksize < 512 ||
        msc->blocksize > 4096 || (msc->blocksize & (msc->blocksize - 1))) return;
    struct lp_disk *disk = calloc(1, sizeof(*disk));
    if (!disk) return;
    disk->dev = dev;
    struct block_device block = {.type = DISK_TYPE_USB, .sector_size = msc->blocksize,
        .sector_count = msc->numblocks, .local_device_struct = disk,
        .read = lp_disk_read, .write = lp_disk_write, .flush = lp_disk_flush};
    register_device(&block);
    if (!block.name[0]) { free(disk); return; }
    memcpy(disk->name, block.name, sizeof(disk->name));
    msc->data = disk;
    printf("[LPUSB] disk %s sectors=%u bytes/sector=%u\n", disk->name, msc->numblocks, msc->blocksize);
}
void usbdisk_remove(usbdev_t *dev) {
    struct lp_disk *disk = MSC_INST(dev)->data;
    if (!disk) return;
    disk->removing = true;
    partitions_device_remove(disk->name);
    unregister_device(disk->name);
    disk->dev = NULL;
    MSC_INST(dev)->data = NULL;
    printf("[LPUSB] removed disk %s\n", disk->name);
}
void usb_generic_create(usbdev_t *dev) {
    printf("[LPUSB] unclaimed device %04x:%04x; legacy TP-Link/USB-net driver is not attached\n",
        dev->descriptor->idVendor, dev->descriptor->idProduct);
}
void usb_generic_remove(usbdev_t *dev) { (void)dev; }
