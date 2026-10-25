#include <net/wifi.h>
#include <pci/xhci.h>
#include <mm/malloc.h>
#include <mm/pmm.h>
#include <memory.h>
#include <gconsole.h>
#include <string.h>
#include <scheduler/scheduler.h>
#include <kernel.h>

extern int xhci_configure_bulk_endpoints(xhci_controller_t *hc, usb_msd_t *msd,
    uint8_t ep_out_addr, uint8_t ep_in_addr, uint16_t mps);
extern int xhci_bulk_transfer(xhci_controller_t *hc, uint8_t slot_id,
    volatile struct xhci_trb *ring, uint32_t *enqueue, uint8_t *cycle,
    uint8_t db_target, uint64_t data_phys, uint32_t length, uint8_t dir_in);
extern int xhci_control_transfer(xhci_controller_t *hc, uint8_t slot_id,
    volatile struct xhci_trb *ep0_ring, uint32_t *ep0_enqueue, uint8_t *ep0_cycle,
    struct usb_setup_packet *setup, uint64_t data_buf_phys, uint16_t transfer_len);

typedef struct {
    xhci_controller_t *hc;
    uint8_t slot_id;
    uint8_t ep_in, ep_out;
    uint16_t mps;

    volatile struct xhci_trb *bulk_out_ring;
    uint64_t bulk_out_ring_phys;
    uint32_t bulk_out_enqueue;
    uint8_t  bulk_out_cycle;
    uint8_t  bulk_out_db;

    volatile struct xhci_trb *bulk_in_ring;
    uint64_t bulk_in_ring_phys;
    uint32_t bulk_in_enqueue;
    uint8_t  bulk_in_cycle;
    uint8_t  bulk_in_db;

    wifi_device_t *wdev;
    volatile bool running;
} rtl8192eu_t;

static inline void *rtl_p2v(uint64_t p) {
    return (void *)(p + get_bootinfo().hhdm->offset);
}

static void rtl_write8(rtl8192eu_t *rtl, uint16_t addr, uint8_t val) {
    uint64_t phys = (uint64_t)pmm_alloc_pages(1);
    *(uint8_t *)rtl_p2v(phys) = val;
    struct usb_setup_packet sp = {
        .bmRequestType = 0x40, .bRequest = 0x05,
        .wValue = addr, .wIndex = 0, .wLength = 1
    };
    xhci_control_transfer(rtl->hc, rtl->slot_id,
        rtl->hc->ep0_ring[rtl->slot_id], &rtl->hc->ep0_enqueue[rtl->slot_id],
        &rtl->hc->ep0_cycle[rtl->slot_id], &sp, phys, 1);
    pmm_free_pages((void *)phys, 1);
}

static int rtl_download_fw(rtl8192eu_t *rtl, const uint8_t *fw, size_t fw_len) {
    if (!fw || fw_len == 0) return -1;

    console_print_string("[RTL8192EU] Uploading FW to MCU...\n");
    rtl_write8(rtl, 0x001F, 0x00); /* halt 8051 */

    uint64_t page_phys = (uint64_t)pmm_alloc_pages(1);
    uint8_t *page = rtl_p2v(page_phys);
    size_t off = 0;
    uint16_t page_i = 0;

    while (off < fw_len) {
        size_t n = fw_len - off;
        if (n > 1024) n = 1024;
        memcpy(page, fw + off, n);

        struct usb_setup_packet sp = {
            .bmRequestType = 0x40, .bRequest = 0x05,
            .wValue = (uint16_t)(0x1000 + page_i * 1024),
            .wIndex = 0, .wLength = (uint16_t)n
        };
        if (xhci_control_transfer(rtl->hc, rtl->slot_id,
                rtl->hc->ep0_ring[rtl->slot_id], &rtl->hc->ep0_enqueue[rtl->slot_id],
                &rtl->hc->ep0_cycle[rtl->slot_id], &sp, page_phys, (uint16_t)n) != 0) {
            pmm_free_pages((void *)page_phys, 1);
            return -1;
        }
        off += n;
        page_i++;
    }
    pmm_free_pages((void *)page_phys, 1);

    rtl_write8(rtl, 0x001F, 0x04); /* run MCU */
    console_print_string("[RTL8192EU] MCU started\n");
    return 0;
}

/* ---- ops: вызываются ТОЛЬКО из wifi_boot_all ---- */

static int rtl_init(wifi_device_t *dev, const uint8_t *fw, size_t fw_len) {
    rtl8192eu_t *rtl = malloc_alloc(sizeof(rtl8192eu_t));
    if (!rtl) return -1;
    memset(rtl, 0, sizeof(*rtl));

    rtl->hc = dev->hc;
    rtl->slot_id = dev->slot_id;
    rtl->ep_in = dev->ep_in;
    rtl->ep_out = dev->ep_out;
    rtl->mps = dev->mps;
    rtl->wdev = dev;
    dev->priv = rtl;

    uint8_t dci_out = (dev->ep_out & 0x0F) * 2 + 0;
    uint8_t dci_in  = (dev->ep_in  & 0x0F) * 2 + 1;
    rtl->bulk_out_db = dci_out;
    rtl->bulk_in_db  = dci_in;

    rtl->bulk_out_ring_phys = (uint64_t)pmm_alloc_pages(1);
    rtl->bulk_out_ring = rtl_p2v(rtl->bulk_out_ring_phys);
    memset((void *)rtl->bulk_out_ring, 0, 4096);
    rtl->bulk_out_enqueue = 0;
    rtl->bulk_out_cycle = 1;
    rtl->bulk_out_ring[COUNT_OF_TRB - 1].param = rtl->bulk_out_ring_phys;
    rtl->bulk_out_ring[COUNT_OF_TRB - 1].control = (TRB_LINK << 10) | (1u << 1) | 1u;

    rtl->bulk_in_ring_phys = (uint64_t)pmm_alloc_pages(1);
    rtl->bulk_in_ring = rtl_p2v(rtl->bulk_in_ring_phys);
    memset((void *)rtl->bulk_in_ring, 0, 4096);
    rtl->bulk_in_enqueue = 0;
    rtl->bulk_in_cycle = 1;
    rtl->bulk_in_ring[COUNT_OF_TRB - 1].param = rtl->bulk_in_ring_phys;
    rtl->bulk_in_ring[COUNT_OF_TRB - 1].control = (TRB_LINK << 10) | (1u << 1) | 1u;

    usb_msd_t dummy;
    memset(&dummy, 0, sizeof(dummy));
    dummy.slot_id = dev->slot_id;
    dummy.bulk_out_ring_phys = rtl->bulk_out_ring_phys;
    dummy.bulk_in_ring_phys = rtl->bulk_in_ring_phys;

    if (xhci_configure_bulk_endpoints(dev->hc, &dummy, dev->ep_out, dev->ep_in, dev->mps) != 0) {
        console_print_string("[RTL8192EU] EP config fail\n");
        return -1;
    }

    /* MAC 0x011A */
    uint64_t phys = (uint64_t)pmm_alloc_pages(1);
    struct usb_setup_packet sp = {
        .bmRequestType = 0xC0, .bRequest = 0x05,
        .wValue = 0x011A, .wIndex = 0, .wLength = 6
    };
    if (xhci_control_transfer(dev->hc, dev->slot_id,
            dev->hc->ep0_ring[dev->slot_id], &dev->hc->ep0_enqueue[dev->slot_id],
            &dev->hc->ep0_cycle[dev->slot_id], &sp, phys, 6) == 0) {
        memcpy(dev->mac_addr, rtl_p2v(phys), 6);
        dev->mac_valid = true;
    }
    pmm_free_pages((void *)phys, 1);

    if (rtl_download_fw(rtl, fw, fw_len) != 0) {
        console_print_string("[RTL8192EU] FW upload failed\n");
        return -1;
    }
    return 0;
}

static int rtl_start(wifi_device_t *dev) {
    rtl8192eu_t *rtl = (rtl8192eu_t *)dev->priv;
    if (!rtl) return -1;
    rtl->running = true;
    /* RX task / scan / connect — следующим этапом */
    console_print_string("[RTL8192EU] start OK (radio assoc TBD)\n");
    return 0;
}

static int rtl_stop(wifi_device_t *dev) {
    rtl8192eu_t *rtl = (rtl8192eu_t *)dev->priv;
    if (rtl) rtl->running = false;
    return 0;
}

static int rtl_send(wifi_device_t *dev, const void *data, size_t len) {
    (void)dev; (void)data; (void)len;
    return -1; /* после assoc */
}

static const wifi_ops_t rtl8192eu_ops = {
    .init       = rtl_init,
    .start      = rtl_start,
    .stop       = rtl_stop,
    .scan       = NULL,
    .connect    = NULL,
    .disconnect = NULL,
    .send_frame = rtl_send,
};

static const wifi_driver_info_t rtl8192eu_tplink = {
    .vid = 0x2357,
    .pid = 0x0107,
    .name = "TP-Link TL-WN821N (RTL8192EU)",
    .fw_path = "C:/firmware/rtl8192eufw.bin",
    .ops = &rtl8192eu_ops,
};

/* Другой USB id того же чипа — можно ещё одну запись */
static const wifi_driver_info_t rtl8192eu_realtek = {
    .vid = 0x0BDA,
    .pid = 0x818B,
    .name = "Realtek RTL8192EU",
    .fw_path = "C:/firmware/rtl8192eufw.bin",
    .ops = &rtl8192eu_ops,
};

void rtl8192eu_driver_register(void) {
    wifi_register_driver(&rtl8192eu_tplink);
    wifi_register_driver(&rtl8192eu_realtek);
}