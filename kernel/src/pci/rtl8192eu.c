#include <net/wifi.h>
#include <pci/xhci.h>
#include <mm/malloc.h>
#include <mm/pmm.h>
#include <memory.h>
#include <gconsole.h>
#include <string.h>
#include <scheduler/scheduler.h>
#include <scheduler/spinlock.h>
#include <cpu/time.h>
#include <kernel.h>
#include "rtl8192eu_tables.h"

extern int xhci_configure_endpoints_multi(xhci_controller_t *hc, uint8_t slot_id,
    const uint8_t *out_addrs, const uint64_t *out_rings, int out_count,
    uint8_t in_addr, uint64_t in_ring, uint16_t mps);
extern int xhci_bulk_transfer_len(xhci_controller_t *hc, uint8_t slot_id,
    volatile struct xhci_trb *ring, uint32_t *enqueue, uint8_t *cycle,
    uint8_t db_target, uint64_t data_phys, uint32_t length, uint8_t dir_in,
    uint32_t *out_actual);
extern int xhci_control_transfer(xhci_controller_t *hc, uint8_t slot_id,
    volatile struct xhci_trb *ep0_ring, uint32_t *ep0_enqueue, uint8_t *ep0_cycle,
    struct usb_setup_packet *setup, uint64_t data_buf_phys, uint16_t transfer_len);

#define REG_SYS_ISO_CTRL   0x0000
#define REG_SYS_FUNC       0x0002
#define REG_APS_FSMCO      0x0004
#define REG_SYS_CLKR       0x0008
#define REG_9346CR         0x000a
#define REG_8192E_LDOV12   0x0014
#define REG_RSV_CTRL       0x001c
#define REG_RF_CTRL        0x001f
#define REG_EFUSE_CTRL     0x0030
#define REG_EFUSE_TEST     0x0034
#define REG_AFE_CTRL4      0x0078
#define REG_LDO_SW_CTRL    0x007c
#define REG_MCU_FW_DL      0x0080
#define REG_AFE_PLL_CTRL   0x0094
#define REG_EFUSE_ACCESS   0x00cf
#define REG_HPON_FSM       0x00ec
#define REG_SYS_CFG        0x00f0
#define REG_GPIO_OUTSTS    0x00f4
#define REG_CR             0x0100
#define REG_FW_START_ADDR  0x1000
#define REG_SIE_EP_TX      0xfe66

#define FW_PAGE_SIZE       4096
#define EFUSE_MAP_LEN      512

typedef struct {
    xhci_controller_t *hc;
    uint8_t slot_id;
    uint16_t mps;

    volatile struct xhci_trb *ring_in;
    uint64_t ring_in_phys;
    uint32_t ring_in_enqueue;
    uint8_t ring_in_cycle;
    uint8_t ring_in_db;

    volatile struct xhci_trb *ring_out[4];
    uint64_t ring_out_phys[4];
    uint32_t ring_out_enqueue[4];
    uint8_t ring_out_cycle[4];
    uint8_t ring_out_db[4];
    int out_count;

    uint64_t bounce_phys;
    uint8_t *bounce;
    spinlock_t reg_lock;

    uint8_t efuse[EFUSE_MAP_LEN];
    bool efuse_ok;
    uint8_t tx_paths;
    uint8_t chip_cut;
    bool ep_tx_high;
    bool ep_tx_normal;
    bool ep_tx_low;
    int ep_tx_count;

    wifi_device_t *wdev;
    volatile bool running;
} rtl8192eu_t;

static inline void *rtl_p2v(uint64_t p) {
    return (void *)(p + get_bootinfo().hhdm->offset);
}

static void rtl_msleep(uint32_t ms) {
    uint64_t end = get_monotonic_ms() + ms;
    while (get_monotonic_ms() < end)
        schedule_yield();
}

static void rtl_udelay(uint32_t us) {
    for (volatile uint32_t i = 0; i < us * 40; i++)
        __asm__ volatile("pause");
}

static void rtl_log(const char *s) {
    console_print_string("[RTL8192EU] ");
    console_print_string(s);
}

static void rtl_log_hex(const char *s, uint64_t v) {
    console_print_string("[RTL8192EU] ");
    console_print_string(s);
    console_print_hex(v);
    console_print_string("\n");
}

static int rtl_ctrl(rtl8192eu_t *rtl, uint8_t type, uint16_t addr,
                    void *data, uint16_t len) {
    if (len > 2048) return -1;
    if (data && len) memcpy(rtl->bounce, data, len);
    struct usb_setup_packet sp;
    sp.bmRequestType = type;
    sp.bRequest = 0x05;
    sp.wValue = addr;
    sp.wIndex = 0;
    sp.wLength = len;
    spin_lock(&rtl->reg_lock);
    int r = xhci_control_transfer(rtl->hc, rtl->slot_id,
        rtl->hc->ep0_ring[rtl->slot_id], &rtl->hc->ep0_enqueue[rtl->slot_id],
        &rtl->hc->ep0_cycle[rtl->slot_id], &sp,
        len ? rtl->bounce_phys : 0, len);
    if (r == 0 && data && len && (type & 0x80))
        memcpy(data, rtl->bounce, len);
    spin_unlock(&rtl->reg_lock);
    return r;
}

static uint8_t rtl_r8(rtl8192eu_t *rtl, uint16_t addr) {
    uint8_t v = 0;
    rtl_ctrl(rtl, 0xC0, addr, &v, 1);
    return v;
}

static uint16_t rtl_r16(rtl8192eu_t *rtl, uint16_t addr) {
    uint16_t v = 0;
    rtl_ctrl(rtl, 0xC0, addr, &v, 2);
    return v;
}

static uint32_t rtl_r32(rtl8192eu_t *rtl, uint16_t addr) {
    uint32_t v = 0;
    rtl_ctrl(rtl, 0xC0, addr, &v, 4);
    return v;
}

static void rtl_w8(rtl8192eu_t *rtl, uint16_t addr, uint8_t v) {
    rtl_ctrl(rtl, 0x40, addr, &v, 1);
}

static void rtl_w16(rtl8192eu_t *rtl, uint16_t addr, uint16_t v) {
    rtl_ctrl(rtl, 0x40, addr, &v, 2);
}

static void rtl_w32(rtl8192eu_t *rtl, uint16_t addr, uint32_t v) {
    rtl_ctrl(rtl, 0x40, addr, &v, 4);
}

static void rtl_w8_set(rtl8192eu_t *rtl, uint16_t addr, uint8_t bits) {
    rtl_w8(rtl, addr, rtl_r8(rtl, addr) | bits);
}

static void rtl_w8_clr(rtl8192eu_t *rtl, uint16_t addr, uint8_t bits) {
    rtl_w8(rtl, addr, rtl_r8(rtl, addr) & (uint8_t)~bits);
}

static void rtl_w16_set(rtl8192eu_t *rtl, uint16_t addr, uint16_t bits) {
    rtl_w16(rtl, addr, rtl_r16(rtl, addr) | bits);
}

static void rtl_w16_clr(rtl8192eu_t *rtl, uint16_t addr, uint16_t bits) {
    rtl_w16(rtl, addr, rtl_r16(rtl, addr) & (uint16_t)~bits);
}

static void rtl_w32_set(rtl8192eu_t *rtl, uint16_t addr, uint32_t bits) {
    rtl_w32(rtl, addr, rtl_r32(rtl, addr) | bits);
}

static void rtl_w32_clr(rtl8192eu_t *rtl, uint16_t addr, uint32_t bits) {
    rtl_w32(rtl, addr, rtl_r32(rtl, addr) & ~bits);
}

static int rtl_writeN(rtl8192eu_t *rtl, uint16_t addr, const uint8_t *buf, uint16_t len) {
    while (len) {
        uint16_t n = len > 128 ? 128 : len;
        if (rtl_ctrl(rtl, 0x40, addr, (void *)buf, n) != 0) return -1;
        addr = (uint16_t)(addr + n);
        buf += n;
        len = (uint16_t)(len - n);
    }
    return 0;
}

static int rtl_efuse_read8(rtl8192eu_t *rtl, uint16_t offset, uint8_t *data) {
    rtl_w8(rtl, REG_EFUSE_CTRL + 1, (uint8_t)(offset & 0xff));
    uint8_t v8 = rtl_r8(rtl, REG_EFUSE_CTRL + 2);
    v8 &= 0xfc;
    v8 |= (offset >> 8) & 0x03;
    rtl_w8(rtl, REG_EFUSE_CTRL + 2, v8);

    v8 = rtl_r8(rtl, REG_EFUSE_CTRL + 3);
    rtl_w8(rtl, REG_EFUSE_CTRL + 3, v8 & 0x7f);

    int i;
    uint32_t v32 = 0;
    for (i = 0; i < 500; i++) {
        v32 = rtl_r32(rtl, REG_EFUSE_CTRL);
        if (v32 & 0x80000000u) break;
    }
    if (i == 500) return -1;

    rtl_udelay(50);
    v32 = rtl_r32(rtl, REG_EFUSE_CTRL);
    *data = (uint8_t)(v32 & 0xff);
    return 0;
}

static int rtl_read_efuse(rtl8192eu_t *rtl) {
    uint16_t v16 = rtl_r16(rtl, REG_9346CR);
    (void)v16;

    rtl_w8(rtl, REG_EFUSE_ACCESS, 0x69);

    v16 = rtl_r16(rtl, REG_SYS_ISO_CTRL);
    if (!(v16 & (1u << 15))) {
        v16 |= (1u << 15);
        rtl_w16(rtl, REG_SYS_ISO_CTRL, v16);
    }
    v16 = rtl_r16(rtl, REG_SYS_FUNC);
    if (!(v16 & (1u << 12))) {
        v16 |= (1u << 12);
        rtl_w16(rtl, REG_SYS_FUNC, v16);
    }
    v16 = rtl_r16(rtl, REG_SYS_CLKR);
    if (!(v16 & (1u << 5)) || !(v16 & (1u << 1))) {
        v16 |= (1u << 5) | (1u << 1);
        rtl_w16(rtl, REG_SYS_CLKR, v16);
    }

    memset(rtl->efuse, 0xff, EFUSE_MAP_LEN);

    int ret = 0;
    uint16_t efuse_addr = 0;
    while (efuse_addr < EFUSE_MAP_LEN) {
        uint8_t header;
        ret = rtl_efuse_read8(rtl, efuse_addr++, &header);
        if (ret || header == 0xff) break;

        uint16_t offset;
        uint8_t word_mask;
        if ((header & 0x1f) == 0x0f) {
            offset = (header & 0xe0) >> 5;
            uint8_t ext;
            ret = rtl_efuse_read8(rtl, efuse_addr++, &ext);
            if (ret) break;
            if ((ext & 0x0f) == 0x0f) continue;
            offset |= ((ext & 0xf0) >> 1);
            word_mask = ext & 0x0f;
        } else {
            offset = (header >> 4) & 0x0f;
            word_mask = header & 0x0f;
        }

        uint16_t map_addr = offset * 8;
        for (int i = 0; i < 8; i++) {
            if (word_mask & (1u << i)) {
                map_addr += 2;
                continue;
            }
            uint8_t val;
            ret = rtl_efuse_read8(rtl, efuse_addr++, &val);
            if (ret) goto out;
            if (map_addr >= EFUSE_MAP_LEN - 1) { ret = -1; goto out; }
            rtl->efuse[map_addr++] = val;
            ret = rtl_efuse_read8(rtl, efuse_addr++, &val);
            if (ret) goto out;
            rtl->efuse[map_addr++] = val;
        }
    }
    ret = 0;
out:
    rtl_w8(rtl, REG_EFUSE_ACCESS, 0x00);
    return ret;
}

static int rtl_identify(rtl8192eu_t *rtl) {
    uint32_t sys_cfg = rtl_r32(rtl, REG_SYS_CFG);
    rtl->chip_cut = (uint8_t)((sys_cfg >> 12) & 0xf);
    if (sys_cfg & (1u << 23)) {
        rtl_log("test chip unsupported\n");
        return -1;
    }

    uint32_t bonding = rtl_r32(rtl, REG_HPON_FSM) & ((1u << 22) | (1u << 23));
    if (bonding == (1u << 22)) {
        rtl->tx_paths = 1;
        rtl_log("chip 8191EU\n");
    } else {
        rtl->tx_paths = 2;
        rtl_log("chip 8192EU\n");
    }
    rtl_log_hex("chip cut=", rtl->chip_cut);

    uint16_t sie = rtl_r16(rtl, REG_SIE_EP_TX);
    if (sie & 0x000f) { rtl->ep_tx_high = true; rtl->ep_tx_count++; }
    if (sie & 0x00f0) { rtl->ep_tx_normal = true; rtl->ep_tx_count++; }
    if (sie & 0x0f00) { rtl->ep_tx_low = true; rtl->ep_tx_count++; }

    if (!rtl->ep_tx_count) {
        switch (rtl->out_count) {
            case 6: case 5: case 4: case 3:
                rtl->ep_tx_low = true; rtl->ep_tx_count++;
            case 2:
                rtl->ep_tx_normal = true; rtl->ep_tx_count++;
            case 1:
                rtl->ep_tx_high = true; rtl->ep_tx_count++;
                break;
            default:
                rtl_log("unsupported TX endpoint count\n");
                return -1;
        }
    }
    rtl_log_hex("tx queues=", (uint64_t)rtl->ep_tx_count);
    return 0;
}

static void rtl_parse_efuse(rtl8192eu_t *rtl, wifi_device_t *dev) {
    uint16_t rtl_id = (uint16_t)(rtl->efuse[0] | (rtl->efuse[1] << 8));
    if (rtl_id != 0x8129) {
        rtl_log_hex("bad efuse id=", rtl_id);
        return;
    }
    memcpy(dev->mac_addr, &rtl->efuse[0xd7], 6);
    dev->mac_valid = true;
    rtl->efuse_ok = true;
    console_print_string("[RTL8192EU] MAC=");
    for (int i = 0; i < 6; i++) {
        if (i) console_print_string(":");
        uint8_t b = dev->mac_addr[i];
        char hex[3];
        hex[0] = "0123456789abcdef"[b >> 4];
        hex[1] = "0123456789abcdef"[b & 15];
        hex[2] = 0;
        console_print_string(hex);
    }
    console_print_string("\n");
}

static void rtl_crystal_afe_adjust(rtl8192eu_t *rtl) {
    uint8_t v8 = rtl_r8(rtl, REG_AFE_PLL_CTRL);
    rtl_w8(rtl, REG_AFE_PLL_CTRL, v8 & 0xfb);

    uint32_t v32 = rtl_r32(rtl, REG_AFE_CTRL4);
    rtl_w32(rtl, REG_AFE_CTRL4, v32 & 0xfffffc7f);

    v8 = rtl_r8(rtl, REG_AFE_PLL_CTRL);
    rtl_w8(rtl, REG_AFE_PLL_CTRL, v8 & 0xbf);

    v32 = rtl_r32(rtl, REG_AFE_CTRL4);
    rtl_w32(rtl, REG_AFE_CTRL4, v32 & 0xffdfffff);
}

static void rtl_disabled_to_emu(rtl8192eu_t *rtl) {
    uint8_t v8 = rtl_r8(rtl, REG_APS_FSMCO + 1);
    rtl_w8(rtl, REG_APS_FSMCO + 1, v8 & (uint8_t)~((1u << 3) | (1u << 4)));
}

static int rtl_emu_to_active(rtl8192eu_t *rtl) {
    uint8_t v8 = rtl_r8(rtl, REG_APS_FSMCO + 1);
    rtl_w8(rtl, REG_APS_FSMCO + 1, v8 & (uint8_t)~(1u << 7));

    v8 = rtl_r8(rtl, REG_APS_FSMCO + 1);
    rtl_w8(rtl, REG_APS_FSMCO + 1, v8 & (uint8_t)~(1u << 2));

    v8 = rtl_r8(rtl, REG_APS_FSMCO + 1);
    rtl_w8(rtl, REG_APS_FSMCO + 1, v8 & (uint8_t)~((1u << 3) | (1u << 4)));

    int count;
    for (count = 500; count; count--) {
        if (rtl_r32(rtl, REG_APS_FSMCO) & (1u << 17)) break;
        rtl_udelay(10);
    }
    if (!count) return -1;

    v8 = rtl_r8(rtl, REG_APS_FSMCO + 2);
    rtl_w8(rtl, REG_APS_FSMCO + 2, v8 | (1u << 0));

    rtl_w32_set(rtl, REG_APS_FSMCO, (1u << 8));
    for (count = 500; count; count--) {
        if (!(rtl_r32(rtl, REG_APS_FSMCO) & (1u << 8))) break;
        rtl_udelay(10);
    }
    if (!count) return -1;
    return 0;
}

static int rtl_power_on(rtl8192eu_t *rtl) {
    uint32_t v32 = rtl_r32(rtl, REG_SYS_CFG);
    if (v32 & (1u << 24)) {
        rtl_w8(rtl, REG_LDO_SW_CTRL, 0xc3);
    } else {
        v32 = rtl_r32(rtl, REG_8192E_LDOV12);
        v32 &= 0xff0fffff;
        v32 |= 0x00500000;
        rtl_w32(rtl, REG_8192E_LDOV12, v32);
        rtl_w8(rtl, REG_LDO_SW_CTRL, 0x83);
    }

    rtl_crystal_afe_adjust(rtl);
    rtl_disabled_to_emu(rtl);
    if (rtl_emu_to_active(rtl) != 0) {
        rtl_log("emu_to_active timeout\n");
        return -1;
    }

    rtl_w16(rtl, REG_CR, 0x0000);
    uint16_t v16 = rtl_r16(rtl, REG_CR);
    v16 |= (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) |
           (1u << 5) | (1u << 6) | (1u << 7) | (1u << 9) | (1u << 10);
    rtl_w16(rtl, REG_CR, v16);
    return 0;
}

static void rtl_reset_8051(rtl8192eu_t *rtl) {
    uint8_t v8 = rtl_r8(rtl, REG_RSV_CTRL + 1);
    rtl_w8(rtl, REG_RSV_CTRL + 1, v8 & (uint8_t)~(1u << 0));

    uint16_t sys_func = rtl_r16(rtl, REG_SYS_FUNC);
    rtl_w16(rtl, REG_SYS_FUNC, sys_func & (uint16_t)~(1u << 10));

    v8 = rtl_r8(rtl, REG_RSV_CTRL + 1);
    rtl_w8(rtl, REG_RSV_CTRL + 1, v8 | (1u << 0));

    rtl_w16(rtl, REG_SYS_FUNC, sys_func | (1u << 10));
}

static int rtl_download_fw(rtl8192eu_t *rtl, const uint8_t *fw, size_t fw_len) {
    uint8_t v8 = rtl_r8(rtl, REG_SYS_FUNC + 1);
    rtl_w8(rtl, REG_SYS_FUNC + 1, v8 | 4);
    rtl_w16_set(rtl, REG_SYS_FUNC, (1u << 10));

    v8 = rtl_r8(rtl, REG_MCU_FW_DL);
    if (v8 & (1u << 7)) {
        rtl_log("firmware already running, resetting MCU\n");
        rtl_w8(rtl, REG_MCU_FW_DL, 0x00);
        rtl_reset_8051(rtl);
    }

    rtl_w8_set(rtl, REG_MCU_FW_DL, (1u << 0));
    rtl_w32_clr(rtl, REG_MCU_FW_DL, (1u << 19));
    rtl_w8_set(rtl, REG_MCU_FW_DL, (1u << 2));

    int ret = 0;
    size_t pages = fw_len / FW_PAGE_SIZE;
    size_t remainder = fw_len % FW_PAGE_SIZE;
    const uint8_t *ptr = fw;

    for (size_t i = 0; i < pages; i++) {
        v8 = rtl_r8(rtl, REG_MCU_FW_DL + 2) & 0xF8;
        rtl_w8(rtl, REG_MCU_FW_DL + 2, v8 | (uint8_t)i);
        if (rtl_writeN(rtl, REG_FW_START_ADDR, ptr, FW_PAGE_SIZE) != 0) {
            ret = -1;
            goto out;
        }
        ptr += FW_PAGE_SIZE;
    }
    if (remainder) {
        v8 = rtl_r8(rtl, REG_MCU_FW_DL + 2) & 0xF8;
        rtl_w8(rtl, REG_MCU_FW_DL + 2, v8 | (uint8_t)pages);
        if (rtl_writeN(rtl, REG_FW_START_ADDR, ptr, (uint16_t)remainder) != 0)
            ret = -1;
    }

out:
    rtl_w16_clr(rtl, REG_MCU_FW_DL, (1u << 0));
    return ret;
}

static int rtl_start_fw(rtl8192eu_t *rtl) {
    int i;
    uint32_t v32 = 0;
    for (i = 0; i < 1000; i++) {
        v32 = rtl_r32(rtl, REG_MCU_FW_DL);
        if (v32 & (1u << 2)) break;
    }
    if (i == 1000) {
        rtl_log("firmware checksum timeout\n");
        return -1;
    }

    v32 |= (1u << 1);
    v32 &= ~(1u << 6);
    rtl_w32(rtl, REG_MCU_FW_DL, v32);

    rtl_reset_8051(rtl);

    for (i = 0; i < 1000; i++) {
        v32 = rtl_r32(rtl, REG_MCU_FW_DL);
        if (v32 & (1u << 6)) break;
        rtl_udelay(100);
    }
    if (i == 1000) {
        rtl_log("firmware failed to start\n");
        return -1;
    }
    return 0;
}

static volatile struct xhci_trb *rtl_make_ring(uint64_t *out_phys) {
    uint64_t phys = (uint64_t)pmm_alloc_pages(1);
    volatile struct xhci_trb *ring = rtl_p2v(phys);
    memset((void *)ring, 0, 4096);
    ring[COUNT_OF_TRB - 1].param = phys;
    ring[COUNT_OF_TRB - 1].control = (TRB_LINK << 10) | (1u << 1) | 1u;
    *out_phys = phys;
    return ring;
}

static int rtl_init(wifi_device_t *dev, const uint8_t *fw, size_t fw_len) {
    if (!fw || fw_len == 0) {
        rtl_log("no firmware data\n");
        return -1;
    }

    rtl8192eu_t *rtl = malloc_alloc(sizeof(rtl8192eu_t));
    if (!rtl) return -1;
    memset(rtl, 0, sizeof(*rtl));

    rtl->hc = dev->hc;
    rtl->slot_id = dev->slot_id;
    rtl->mps = dev->mps ? dev->mps : 512;
    rtl->wdev = dev;
    rtl->reg_lock = (spinlock_t)SPINLOCK_INIT;
    dev->priv = rtl;

    rtl->bounce_phys = (uint64_t)pmm_alloc_pages(1);
    rtl->bounce = rtl_p2v(rtl->bounce_phys);

    rtl->out_count = dev->ep_out_count;
    if (rtl->out_count > 4) rtl->out_count = 4;
    if (rtl->out_count == 0) {
        rtl->out_count = 1;
        dev->ep_out_q[0] = dev->ep_out;
    }

    uint8_t out_addrs[4];
    uint64_t out_rings[4];
    for (int i = 0; i < rtl->out_count; i++) {
        out_addrs[i] = dev->ep_out_q[i];
        rtl->ring_out[i] = rtl_make_ring(&rtl->ring_out_phys[i]);
        rtl->ring_out_enqueue[i] = 0;
        rtl->ring_out_cycle[i] = 1;
        rtl->ring_out_db[i] = (out_addrs[i] & 0x0F) * 2 + 0;
        out_rings[i] = rtl->ring_out_phys[i];
    }
    rtl->ring_in = rtl_make_ring(&rtl->ring_in_phys);
    rtl->ring_in_enqueue = 0;
    rtl->ring_in_cycle = 1;
    rtl->ring_in_db = (dev->ep_in & 0x0F) * 2 + 1;

    if (xhci_configure_endpoints_multi(dev->hc, dev->slot_id,
            out_addrs, out_rings, rtl->out_count,
            dev->ep_in, rtl->ring_in_phys, rtl->mps) != 0) {
        rtl_log("endpoint configure failed\n");
        return -1;
    }
    rtl_log("endpoints configured\n");

    if (rtl_identify(rtl) != 0) return -1;

    if (rtl_read_efuse(rtl) != 0) {
        rtl_log("efuse read failed\n");
        return -1;
    }
    rtl_parse_efuse(rtl, dev);
    if (!dev->mac_valid) {
        rtl_log("no valid MAC in efuse\n");
        return -1;
    }

    rtl_log("power on...\n");
    if (rtl_power_on(rtl) != 0) {
        rtl_log("power on failed\n");
        return -1;
    }
    rtl_log("power on OK\n");

    rtl_log_hex("firmware bytes=", fw_len);
    if (rtl_download_fw(rtl, fw, fw_len) != 0) {
        rtl_log("firmware download failed\n");
        return -1;
    }
    rtl_log("firmware downloaded\n");

    if (rtl_start_fw(rtl) != 0) return -1;
    rtl_log("firmware running\n");
    return 0;
}

static int rtl_start(wifi_device_t *dev) {
    rtl8192eu_t *rtl = (rtl8192eu_t *)dev->priv;
    if (!rtl) return -1;
    rtl_log("chip up; radio init in next stage\n");
    return 0;
}

static int rtl_stop(wifi_device_t *dev) {
    rtl8192eu_t *rtl = (rtl8192eu_t *)dev->priv;
    if (rtl) rtl->running = false;
    return 0;
}

static int rtl_send(wifi_device_t *dev, const void *data, size_t len) {
    (void)dev; (void)data; (void)len;
    return -1;
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

/* Полный список USB ID RTL8192EU (как в Linux rtl8xxxu): адаптеры
   TP-Link / D-Link / Mercusys / Realtek часто отличаются только PID. */
static const wifi_driver_info_t rtl8192eu_devices[] = {
    { .vid = 0x2357, .pid = 0x0107, .name = "TP-Link TL-WN821N v5/v6 (RTL8192EU)",
      .fw_path = "/lib/firmware/rtlwifi/rtl8192eu_nic.bin", .ops = &rtl8192eu_ops },
    { .vid = 0x2357, .pid = 0x0108, .name = "TP-Link TL-WN822N v4 (RTL8192EU)",
      .fw_path = "/lib/firmware/rtlwifi/rtl8192eu_nic.bin", .ops = &rtl8192eu_ops },
    { .vid = 0x2357, .pid = 0x0109, .name = "TP-Link (RTL8192EU)",
      .fw_path = "/lib/firmware/rtlwifi/rtl8192eu_nic.bin", .ops = &rtl8192eu_ops },
    { .vid = 0x2357, .pid = 0x0126, .name = "TP-Link TL-WN8200ND v2 (RTL8192EU)",
      .fw_path = "/lib/firmware/rtlwifi/rtl8192eu_nic.bin", .ops = &rtl8192eu_ops },
    { .vid = 0x0BDA, .pid = 0x818B, .name = "Realtek RTL8192EU",
      .fw_path = "/lib/firmware/rtlwifi/rtl8192eu_nic.bin", .ops = &rtl8192eu_ops },
    { .vid = 0x0BDA, .pid = 0x818C, .name = "Realtek RTL8192EU (default ID)",
      .fw_path = "/lib/firmware/rtlwifi/rtl8192eu_nic.bin", .ops = &rtl8192eu_ops },
    { .vid = 0x2001, .pid = 0x3319, .name = "D-Link DWA-131 E1 (RTL8192EU)",
      .fw_path = "/lib/firmware/rtlwifi/rtl8192eu_nic.bin", .ops = &rtl8192eu_ops },
    { .vid = 0x2001, .pid = 0x3312, .name = "D-Link DWA-131 C1 (RTL8192EU)",
      .fw_path = "/lib/firmware/rtlwifi/rtl8192eu_nic.bin", .ops = &rtl8192eu_ops },
    { .vid = 0x2019, .pid = 0xAB33, .name = "RTL8192EU (2019:AB33)",
      .fw_path = "/lib/firmware/rtlwifi/rtl8192eu_nic.bin", .ops = &rtl8192eu_ops },
    { .vid = 0x2C4E, .pid = 0x0100, .name = "Mercusys MW300UM (RTL8192EU)",
      .fw_path = "/lib/firmware/rtlwifi/rtl8192eu_nic.bin", .ops = &rtl8192eu_ops },
    { .vid = 0x2C4E, .pid = 0x0104, .name = "Mercusys MW300UH (RTL8192EU)",
      .fw_path = "/lib/firmware/rtlwifi/rtl8192eu_nic.bin", .ops = &rtl8192eu_ops },
};

void rtl8192eu_driver_register(void) {
    for (size_t i = 0; i < sizeof(rtl8192eu_devices) / sizeof(rtl8192eu_devices[0]); i++)
        wifi_register_driver(&rtl8192eu_devices[i]);
}
