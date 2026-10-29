#include <stddef.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <pci/pci.h>
#include <pci/xhci.h>
#include <mm/malloc.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <kernel.h>
#include <bootloader.h>
#include <gconsole.h>
#include <memory.h>
#include <disk/block.h>
#include <disk/partitions.h>
#include <scheduler/scheduler.h>
#include <inout/inout.h>
#include <cpu/apic.h>
#include <cpu/time.h>
#include <cpu/acpi.h>
#include <net/wifi.h>

extern int init_rtl8192eu_wifi(xhci_controller_t *hc, uint8_t slot_id, uint8_t speed, uint8_t ep_in, uint8_t ep_out, uint16_t mps);

static void hid_capture_report(struct usb_hid *hid, const struct xhci_trb *evt, uint32_t len);
static void hid_capture_fail(struct usb_hid *hid, const struct xhci_trb *evt);
void handle_port_connect(xhci_controller_t *hc, uint8_t port);
void handle_port_disconnect(xhci_controller_t *hc, uint8_t port);
void listing_all_ports(xhci_controller_t *hc);
static void xhci_ack_port_changes(volatile uint8_t *portsc_reg);
static uint8_t xhci_find_slot_by_port(xhci_controller_t *hc, uint8_t port);

static xhci_controller_t *g_xhci_list[MAX_XHCI_CONTROLLERS];
static int g_xhci_count = 0;

static inline bool interrupts_enabled(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0" : "=r"(flags));
    return (flags & (1ULL << 9)) != 0;
}

static inline void schedule_yield_safe(void) {
    if (interrupts_enabled()) schedule_yield();
}

static int xhci_register_controller(xhci_controller_t *hc) {
    if (g_xhci_count >= MAX_XHCI_CONTROLLERS) return -1;
    g_xhci_list[g_xhci_count++] = hc;
    return g_xhci_count - 1;
}

static inline bool xhci_lock_timeout(volatile uint32_t *lock, uint32_t spins) {
    for (uint32_t i = 0; i < spins; i++) {
        if (!__sync_lock_test_and_set(lock, 1)) return true;
        if ((i & 0x3FF) == 0) schedule_yield_safe();
        __asm__ volatile("pause");
    }
    return false;
}

static inline void xhci_lock(volatile uint32_t *lock) {
    (void)xhci_lock_timeout(lock, 0xFFFFFFFFu);
}

static inline void xhci_unlock(volatile uint32_t *lock) {
    __sync_lock_release(lock);
}

static void *xhci_dma_alloc_pages(uint64_t pages) {
    return pmm_alloc_pages_below(pages, 0x100000000ULL);
}

static void delay_loop(uint32_t count) {
    for (volatile uint32_t i = 0; i < count; i++) __asm__ volatile("pause");
}

static inline uint8_t  xhci_read8(volatile uint8_t *base, uint32_t off)  { return *(volatile uint8_t *)(base + off); }
static inline uint32_t xhci_read32(volatile uint8_t *base, uint32_t off) { return *(volatile uint32_t *)(base + off); }
static inline uint64_t xhci_read64(volatile uint8_t *base, uint32_t off) {
    uint32_t lo = *(volatile uint32_t *)(base + off);
    uint32_t hi = *(volatile uint32_t *)(base + off + 4);
    return ((uint64_t)hi << 32) | lo;
}
static inline void xhci_write32(volatile uint8_t *base, uint32_t off, uint32_t v) { *(volatile uint32_t *)(base + off) = v; }
static inline void xhci_write64(volatile uint8_t *base, uint32_t off, uint64_t v) {
    *(volatile uint32_t *)(base + off)     = (uint32_t)v;
    *(volatile uint32_t *)(base + off + 4) = (uint32_t)(v >> 32);
}

static inline uint64_t virt_to_phys(void *virt) {
    uint64_t v = (uint64_t)virt;
    uint64_t hhdm = get_bootinfo().hhdm->offset;
    if (v >= hhdm) {
        return v - hhdm;
    }
    return vmm_get_phys(get_kernel_pml4(), v & ~0xFFFULL) | (v & 0xFFFULL);
}

static inline void* phys_to_virt(uint64_t phys) {
    return (void*)(phys + get_bootinfo().hhdm->offset);
}

static inline bool evtq_push(struct evt_queue *q, const struct xhci_trb *e)
{
    uint32_t next = (q->tail + 1) % EVT_QUEUE_SIZE;
    if (next == q->head) return false;
    q->buf[q->tail] = *e;
    __sync_synchronize();
    q->tail = next;
    return true;
}

static inline bool evtq_pop(struct evt_queue *q, struct xhci_trb *out)
{
    if (q->head == q->tail) return false;
    *out = q->buf[q->head];
    __sync_synchronize();
    q->head = (q->head + 1) % EVT_QUEUE_SIZE;
    return true;
}

static void evtq_drain(struct evt_queue *q)
{
    struct xhci_trb dummy;
    while (evtq_pop(q, &dummy)) { }
}

static void cooperative_delay(uint32_t ms_ticks)
{
    for (uint32_t i = 0; i < ms_ticks; i++) {
        if ((i & 0xFF) == 0) schedule_yield_safe();
        __asm__ volatile("pause");
    }
}

static bool xhci_wait_bits(volatile uint8_t *base, uint32_t off,
                           uint32_t mask, uint32_t expected, uint32_t ms) {
    uint64_t start = hpet_read_counter();
    uint64_t ticks = hpet_ticks_for_ms(ms);
    for (uint32_t i = 0; i < 2000000; i++) {
        uint32_t value = xhci_read32(base, off);
        if (value == 0xFFFFFFFFu) return false;
        if ((value & mask) == expected) return true;
        if (ticks && hpet_read_counter() - start >= ticks) return false;
        for (uint32_t j = 0; j < 32; j++) __asm__ volatile("pause");
    }
    return false;
}

static bool wait_controller_ready(volatile uint8_t *op_regs) {
    return xhci_wait_bits(op_regs, 0x04, 1u << 11, 0, 1000);
}

static bool wait_hchalt(volatile uint8_t *op_regs, bool halted) {
    return xhci_wait_bits(op_regs, 0x04, 1u, halted ? 1u : 0, 1000);
}

static void xhci_intel_route_ports(pci_device_t *dev) {
    uint32_t id = pci_read_config(dev->bus, dev->slot, dev->func, 0);
    if ((id & 0xFFFF) != 0x8086) return;
    switch (id >> 16) {
    case 0x1E31: case 0x8C31: case 0x8CB1: case 0x8D31:
    case 0x9C31: case 0x9CB1:
        break;
    default:
        return;
    }
    uint32_t subsystem = pci_read_config(dev->bus, dev->slot, dev->func, 0x2C);
    if (subsystem == 0x90A8104D) return;
    uint32_t usb2 = pci_read_config(dev->bus, dev->slot, dev->func, 0xD4);
    uint32_t usb3 = pci_read_config(dev->bus, dev->slot, dev->func, 0xDC);
    if (usb2 != 0xFFFFFFFFu)
        pci_write_config(dev->bus, dev->slot, dev->func, 0xD0, usb2);
    if (usb3 != 0xFFFFFFFFu)
        pci_write_config(dev->bus, dev->slot, dev->func, 0xD8, usb3);
    console_print_string("[XHCI] Intel switchable USB ports routed to xHCI\n");
}

static bool xhci_extended_caps(xhci_controller_t *hc, uint32_t hccparams1) {
    uint32_t off = (hccparams1 >> 16) * 4;
    for (uint32_t n = 0; off && n < 256; n++) {
        if (off < 0x20 || off > 0x100000 - 16) return false;
        uint32_t cap = xhci_read32(hc->cap_regs, off);
        if (cap == 0xFFFFFFFFu) return false;
        uint8_t id = cap & 0xFF;
        if (id == 1) {
            xhci_write32(hc->cap_regs, off, cap | (1u << 24));
            if (!xhci_wait_bits(hc->cap_regs, off, 1u << 16, 0, 1000)) {
                console_print_string("[XHCI] BIOS handoff timeout, forcing OS ownership\n");
                xhci_write32(hc->cap_regs, off,
                             (cap | (1u << 24)) & ~(1u << 16));
            }
            uint32_t ctl = xhci_read32(hc->cap_regs, off + 4);
            ctl &= (0x7u << 1) | (0xFFu << 5) | (0x7u << 17);
            xhci_write32(hc->cap_regs, off + 4, ctl | (0x7u << 29));
        } else if (id == 2) {
            uint32_t ports = xhci_read32(hc->cap_regs, off + 8);
            uint32_t first = ports & 0xFF;
            uint32_t count = (ports >> 8) & 0xFF;
            uint8_t type = xhci_read32(hc->cap_regs, off + 12) & 0x1F;
            for (uint32_t p = first; first && p < first + count && p <= 32; p++) {
                hc->port_major[p] = cap >> 24;
                hc->port_slot_type[p] = type;
            }
        }
        uint32_t next = (cap >> 8) & 0xFF;
        if (!next) return true;
        off += next * 4;
    }
    return off == 0;
}

static struct xhci_trb *cmd_ring_enqueue(xhci_controller_t *hc, uint8_t *out_cycle) {
    xhci_lock(&hc->cmd_lock);
    if (hc->cmd_enqueue >= COUNT_OF_TRB - 1) {
        struct xhci_trb *link = &hc->cmd_ring[COUNT_OF_TRB - 1];
        link->control = (link->control & ~1u) | hc->cmd_cycle;
        hc->cmd_enqueue = 0;
        hc->cmd_cycle ^= 1;
    }
    struct xhci_trb *ret = &hc->cmd_ring[hc->cmd_enqueue++];
    *out_cycle = hc->cmd_cycle & 1;
    xhci_unlock(&hc->cmd_lock);
    return ret;
}

void ep_ring_enqueue(volatile struct xhci_trb *ring, uint32_t *enqueue, uint8_t *cycle,
                     uint64_t param, uint32_t status, uint32_t control_no_cycle)
{
    uint32_t idx = *enqueue;
    if (idx >= COUNT_OF_TRB - 1) {
        ring[idx].param   = virt_to_phys((void*)ring);
        ring[idx].status  = 0;
        ring[idx].control = (TRB_LINK << 10) | (1u << 1) | *cycle;
        *enqueue = 0;
        *cycle ^= 1;
        idx = 0;
    }
    ring[idx].param   = param;
    ring[idx].status  = status;
    ring[idx].control = control_no_cycle | *cycle;
    (*enqueue)++;
}

int xhci_poll_event(xhci_controller_t *hc, struct xhci_trb *out_evt) {
    volatile struct xhci_trb *ring = (volatile struct xhci_trb *)hc->evt_ring;
    uint32_t control = ring[hc->evt_dequeue].control;
    __sync_synchronize();
    if ((control & 1) != hc->evt_cycle) {
        return 0;
    }
    out_evt->param   = ring[hc->evt_dequeue].param;
    out_evt->status  = ring[hc->evt_dequeue].status;
    out_evt->control = control;
    hc->evt_dequeue++;
    if (hc->evt_dequeue >= COUNT_OF_TRB) {
        hc->evt_dequeue = 0;
        hc->evt_cycle ^= 1;
    }
    uint64_t erdp = hc->evt_ring_phys + (hc->evt_dequeue * sizeof(struct xhci_trb));
    xhci_write64(hc->rt_regs, 0x20 + 0x18, erdp | (1u << 3));
    return 1;
}

static void hid_capture_report(struct usb_hid *hid, const struct xhci_trb *evt, uint32_t len) {
    if (!hid || !evt || !hid->report || hid->disconnected) return;
    if (len > 64) len = 64;
    if (len > hid->report_size) len = hid->report_size;

    if (!xhci_lock_timeout(&hid->report_lock, 200000)) return;
    if (!hid->transfer_pending || evt->param != hid->pending_trb_phys) {
        xhci_unlock(&hid->report_lock);
        return;
    }
    hid->transfer_pending = 0;
    if (len == 0) {
        xhci_unlock(&hid->report_lock);
        return;
    }

    uint8_t next = (uint8_t)((hid->report_qh + 1) % HID_REPORT_QUEUE);
    if (next == hid->report_qt)
        hid->report_qt = (uint8_t)((hid->report_qt + 1) % HID_REPORT_QUEUE);
    memcpy(hid->report_data[hid->report_qh], (const void *)hid->report, len);
    hid->report_data_len[hid->report_qh] = (uint8_t)len;
    hid->report_qh = next;
    hid->report_ready = 1;
    xhci_unlock(&hid->report_lock);
}

static void hid_capture_fail(struct usb_hid *hid, const struct xhci_trb *evt) {
    if (!hid || !evt || hid->disconnected) return;
    if (!xhci_lock_timeout(&hid->report_lock, 200000)) return;
    if (hid->transfer_pending && evt->param == hid->pending_trb_phys) {
        hid->transfer_pending = 0;
        hid->halted = 1;
    }
    xhci_unlock(&hid->report_lock);
}

void xhci_poll(xhci_controller_t *hc) {
    if (!xhci_lock_timeout(&hc->evt_lock, 2000000)) return;
    for (;;) {
        struct xhci_trb evt;
        if (xhci_poll_event(hc, &evt) != 1) break;

        uint8_t type = (evt.control >> 10) & 0x3F;

        if (type == TRB_COMMAND_COMPLETION) {
            evtq_push(&hc->cmd_evt_q, &evt);
        } else if (type == TRB_TRANSFER_EVENT) {
            uint8_t slot = (evt.control >> 24) & 0xFF;
            uint8_t dci  = (evt.control >> 16) & 0x1F;
            uint8_t code = (evt.status >> 24) & 0xFF;

            if (slot < MAX_XHCI_SLOTS) {
                evtq_push(&hc->transfer_evt_q[slot][dci], &evt);

                struct usb_hid *hid = hc->hid_table[slot][dci];
                if (hid && !hid->disconnected) {
                    if (code == 1 || code == 13) {
                        uint32_t residual = evt.status & 0x00FFFFFFu;
                        uint32_t len = (residual <= hid->report_size)
                            ? (uint32_t)(hid->report_size - residual)
                            : (uint32_t)hid->report_size;
                        hid->report_len = (uint16_t)len;
                        hid_capture_report(hid, &evt, len);
                    } else if (code >= 2) {
                        hid_capture_fail(hid, &evt);
                    }
                }
            }
        } else if (type == TRB_PORT_STATUS_CHANGE) {
            uint8_t port = (evt.param >> 24) & 0xFF;
            uint8_t next = (hc->port_change_tail + 1) % 16;
            if (next != hc->port_change_head) {
                hc->port_change_queue[hc->port_change_tail] = port;
                hc->port_change_tail = next;
            }
        }
    }
    xhci_unlock(&hc->evt_lock);
}

#define XHCI_BOOT_TIMEOUT_LIMIT 40
static volatile uint32_t xhci_enum_timeouts = 0;
static volatile bool xhci_enum_active = false;
static volatile bool xhci_boot_done = false;
static uint64_t xhci_enum_start;
static uint64_t xhci_enum_ticks;

void xhci_mark_boot_complete(void) {
    xhci_boot_done = true;
}

static void xhci_enum_begin(void) {
    xhci_enum_start = hpet_read_counter();
    xhci_enum_ticks = hpet_ticks_for_ms(750);
    xhci_enum_timeouts = 0;
    xhci_enum_active = true;
}

static void xhci_enum_end(void) {
    xhci_enum_active = false;
}

static bool xhci_boot_exhausted(void) {
    if (xhci_boot_done || interrupts_enabled()) return false;
    if (!xhci_enum_active) return false;
    return xhci_enum_timeouts >= XHCI_BOOT_TIMEOUT_LIMIT ||
           (xhci_enum_ticks && hpet_read_counter() - xhci_enum_start >= xhci_enum_ticks);
}

static int wait_event_from(xhci_controller_t *hc, struct evt_queue *q,
                           struct xhci_trb *out_evt)
{
    if (hc->command_failed || xhci_boot_exhausted()) return -1;
    bool boot = !xhci_boot_done && !interrupts_enabled();
    uint64_t start = hpet_read_counter();
    uint64_t ticks = hpet_ticks_for_ms(boot ? 100 : 1000);
    uint64_t spins = boot ? 200000 : 2000000;
    for (uint64_t t = 0; t < spins; t++) {
        if (evtq_pop(q, out_evt)) return 0;
        if ((t & 0x3F) == 0) {
            if (ticks && hpet_read_counter() - start >= ticks) break;
            if (xhci_boot_exhausted()) break;
        }

        if ((t & 0x3F) == 0) xhci_poll(hc);

        if ((t & 0x3FF) == 0) schedule_yield_safe();
        __asm__ volatile("pause");
    }
    if (!xhci_boot_done && !interrupts_enabled() && xhci_enum_active) {
        uint32_t n = __atomic_add_fetch(&xhci_enum_timeouts, 1, __ATOMIC_RELAXED);
        if (n == XHCI_BOOT_TIMEOUT_LIMIT)
            console_print_string("[XHCI] boot timeout budget exhausted, deferring to hotplug\n");
    }
    return -1;
}

int xhci_wait_cmd(xhci_controller_t *hc, struct xhci_trb *out_evt)
{
    return wait_event_from(hc, &hc->cmd_evt_q, out_evt);
}

int xhci_wait_transfer(xhci_controller_t *hc, uint8_t slot, uint8_t dci,
                       struct xhci_trb *out_evt)
{
    if (slot >= MAX_XHCI_SLOTS || dci >= 32) return -1;
    return wait_event_from(hc, &hc->transfer_evt_q[slot][dci], out_evt);
}

int xhci_wait_event(xhci_controller_t *hc, uint8_t expected_type, struct xhci_trb *out_evt) {
    (void)expected_type;
    return xhci_wait_cmd(hc, out_evt);
}

/* Одна команда = один владелец cmd_exec_lock.
   Раньше несколько задач могли одновременно поставить команды в кольцо и
   потом забрать чужие Command Completion Events из общей очереди. Из-за этого
   после переподключения мыши ENABLE_SLOT/ADDRESS_DEVICE получали ответ не от
   своей команды (вплоть до неверного slot_id). */
static bool xhci_cmd_exec_lock(xhci_controller_t *hc) {
    for (uint32_t i = 0; i < 2000000; i++) {
        if (!__sync_lock_test_and_set(&hc->cmd_exec_lock, 1)) return true;
        if ((i & 0x3FF) == 0) schedule_yield_safe();
        __asm__ volatile("pause");
    }
    return false;
}

static void xhci_cmd_exec_unlock(xhci_controller_t *hc) {
    __sync_lock_release(&hc->cmd_exec_lock);
}

static int xhci_submit_command(xhci_controller_t *hc,
                               uint64_t param, uint32_t status,
                               uint32_t control_no_cycle,
                               struct xhci_trb *out_evt)
{
    struct xhci_trb evt;
    memset(&evt, 0, sizeof(evt));

    if (hc->command_failed || !xhci_cmd_exec_lock(hc)) return -1;
    if (hc->command_failed) { xhci_cmd_exec_unlock(hc); return -1; }
    evtq_drain(&hc->cmd_evt_q);

    uint8_t cyc;
    struct xhci_trb *trb = cmd_ring_enqueue(hc, &cyc);
    trb->param   = param;
    trb->status  = status;
    trb->control = control_no_cycle | cyc;

    __asm__ volatile("mfence" ::: "memory");
    hc->db_regs[0] = 0;

    uint64_t expected = hc->cmd_ring_phys + (uint64_t)(trb - hc->cmd_ring) * sizeof(*trb);
    uint32_t kind = (control_no_cycle >> 10) & 0x3F;
    uint64_t start = hpet_read_counter();
    uint64_t ticks = hpet_ticks_for_ms(kind == TRB_ADDRESS_DEVICE ? 5000 : 1000);
    int res = -1;
    for (uint32_t i = 0; i < 4000000; i++) {
        xhci_poll(hc);
        if (evtq_pop(&hc->cmd_evt_q, &evt) && evt.param == expected) {
            res = 0;
            break;
        }
        if (ticks && hpet_read_counter() - start >= ticks) break;
        schedule_yield_safe();
        for (uint32_t j = 0; j < 32; j++) __asm__ volatile("pause");
    }
    if (res != 0) {
        hc->command_failed = true;
        xhci_write32(hc->op_regs, 0, xhci_read32(hc->op_regs, 0) & ~1u);
        if (!wait_hchalt(hc->op_regs, true)) {
            console_print_string("[XHCI] command timeout, controller could not halt\n");
        } else {
            console_print_string("[XHCI] command timeout, controller halted\n");
        }
        uint32_t cmd = pci_read_config(hc->bus, hc->slot, hc->func, 0x04) & 0xFFFF;
        pci_write_config(hc->bus, hc->slot, hc->func, 0x04, cmd & ~(1u << 2));
        (void)pci_read_config(hc->bus, hc->slot, hc->func, 0x04);
    }
    xhci_cmd_exec_unlock(hc);

    if (out_evt) *out_evt = evt;
    return res;
}

void xhci_set_tr_dequeue(xhci_controller_t *hc, uint8_t slot_id, uint8_t dci, uint64_t ring_phys, uint8_t cycle) {
    struct xhci_trb evt;
    xhci_submit_command(hc, ring_phys | (cycle & 1), 0,
                        (TRB_SET_TR_DEQUEUE << 10) | ((uint32_t)dci << 16) |
                        ((uint32_t)slot_id << 24), &evt);
}

void xhci_reset_endpoint(xhci_controller_t *hc, uint8_t slot_id, uint8_t dci) {
    struct xhci_trb evt;
    xhci_submit_command(hc, 0, 0,
                        (TRB_RESET_ENDPOINT << 10) | ((uint32_t)dci << 16) |
                        ((uint32_t)slot_id << 24), &evt);
}

int xhci_control_transfer(xhci_controller_t *hc, uint8_t slot_id,
                          volatile struct xhci_trb *ep0_ring, uint32_t *ep0_enqueue, uint8_t *ep0_cycle,
                          struct usb_setup_packet *setup, uint64_t data_buf_phys, uint16_t transfer_len)
{
    if (!hc || !setup || slot_id >= MAX_XHCI_SLOTS || hc->command_failed) return -1;
    if (!hc->dma64 && transfer_len &&
        (data_buf_phys >= 0x100000000ULL || transfer_len > 0x100000000ULL - data_buf_phys))
        return -EOVERFLOW;
    if (!xhci_lock_timeout(&hc->ep0_lock[slot_id], 2000000)) return -1;

    int result = -2;
    uint64_t setup_data;
    memcpy(&setup_data, setup, 8);

    bool is_dir_in = (setup->bmRequestType & 0x80) != 0;
    uint32_t trt = 0, status_dir = 0;

    if (transfer_len == 0)      { trt = 0; status_dir = 1; }
    else if (is_dir_in)         { trt = 3; status_dir = 0; }
    else                        { trt = 2; status_dir = 1; }

    uint32_t needed = (transfer_len > 0) ? 3u : 2u;
    uint32_t start  = *ep0_enqueue;

    if (start + needed > COUNT_OF_TRB - 1) {
        ep0_ring[COUNT_OF_TRB - 1].param   = virt_to_phys((void*)ep0_ring);
        ep0_ring[COUNT_OF_TRB - 1].status  = 0;
        ep0_ring[COUNT_OF_TRB - 1].control = (TRB_LINK << 10) | (1u << 1) | *ep0_cycle;
        *ep0_enqueue = 0;
        *ep0_cycle  ^= 1;
        start = 0;
    }

    uint32_t trb_start = start;
    uint8_t  cyc_start = *ep0_cycle;

    uint32_t setup_ctrl = (TRB_SETUP_STAGE << 10) | (1u << 6) | (trt << 16);
    ep_ring_enqueue(ep0_ring, ep0_enqueue, ep0_cycle, setup_data, 8, setup_ctrl);

    if (transfer_len > 0) {
        uint32_t data_ctrl = (TRB_DATA_STAGE << 10) | ((is_dir_in ? 1u : 0u) << 16);
        ep_ring_enqueue(ep0_ring, ep0_enqueue, ep0_cycle, data_buf_phys, transfer_len, data_ctrl);
    }

    uint32_t status_ctrl = (TRB_STATUS_STAGE << 10) | (1u << 5) | (status_dir << 16);
    ep_ring_enqueue(ep0_ring, ep0_enqueue, ep0_cycle, 0, 0, status_ctrl);

    uint64_t cur_ring_phys = virt_to_phys((void*)ep0_ring);

    evtq_drain(&hc->transfer_evt_q[slot_id][1]);

    __asm__ volatile("mfence" ::: "memory");
    hc->db_regs[slot_id] = 1;

    struct xhci_trb evt;
    if (xhci_wait_transfer(hc, slot_id, 1, &evt) != 0) {
        xhci_reset_endpoint(hc, slot_id, 1);
        xhci_set_tr_dequeue(hc, slot_id, 1,
                            cur_ring_phys + trb_start * sizeof(struct xhci_trb),
                            cyc_start);
        *ep0_enqueue = trb_start;
        *ep0_cycle   = cyc_start;
        evtq_drain(&hc->transfer_evt_q[slot_id][1]);
        result = -1;
        goto out;
    }

    uint8_t code = (evt.status >> 24) & 0xFF;
    if (code == 1 || code == 13) {
        result = 0;
        goto out;
    }

    if (code == 6) {
        xhci_reset_endpoint(hc, slot_id, 1);
        xhci_set_tr_dequeue(hc, slot_id, 1,
                            cur_ring_phys + trb_start * sizeof(struct xhci_trb),
                            cyc_start);
        *ep0_enqueue = trb_start;
        *ep0_cycle   = cyc_start;
        evtq_drain(&hc->transfer_evt_q[slot_id][1]);
        result = -6;
        goto out;
    }

    evtq_drain(&hc->transfer_evt_q[slot_id][1]);

out:
    __sync_lock_release(&hc->ep0_lock[slot_id]);
    return result;
}

int xhci_bulk_transfer(xhci_controller_t *hc, uint8_t slot_id,
                       volatile struct xhci_trb *ring, uint32_t *enqueue, uint8_t *cycle,
                       uint8_t db_target, uint64_t data_phys, uint32_t length, uint8_t dir_in)
{
    (void)dir_in;
    if (hc->command_failed) return -ENODEV;
    if (!hc->dma64 && length &&
        (data_phys >= 0x100000000ULL || length > 0x100000000ULL - data_phys))
        return -EOVERFLOW;
    if (length == 0) return 0;

    if (!xhci_lock_timeout(&hc->ep_lock[slot_id][db_target], 2000000)) return -EBUSY;

    uint32_t idx = *enqueue;
    if (idx >= COUNT_OF_TRB - 1) {
        ring[idx].param   = virt_to_phys((void*)ring);
        ring[idx].status  = 0;
        ring[idx].control = (TRB_LINK << 10) | (1u << 1) | *cycle;
        *enqueue = 0;
        *cycle ^= 1;
        idx = 0;
    }
    uint32_t trb_idx   = idx;
    uint8_t  cur_cyc   = *cycle;

    ring[trb_idx].param   = data_phys;
    ring[trb_idx].status  = length & 0x1FFFF;
    ring[trb_idx].control = (TRB_NORMAL << 10) | (1u << 5) | *cycle;
    (*enqueue)++;

    evtq_drain(&hc->transfer_evt_q[slot_id][db_target]);

    __asm__ volatile("mfence" ::: "memory");
    hc->db_regs[slot_id] = db_target;

    struct xhci_trb evt;
    int result = -2;
    if (xhci_wait_transfer(hc, slot_id, db_target, &evt) != 0) {
        xhci_reset_endpoint(hc, slot_id, db_target);
        xhci_set_tr_dequeue(hc, slot_id, db_target,
                            virt_to_phys((void*)ring) + trb_idx * sizeof(struct xhci_trb),
                            cur_cyc);
        *enqueue = trb_idx;
        *cycle   = cur_cyc;
        evtq_drain(&hc->transfer_evt_q[slot_id][db_target]);
        result = -1;
        goto bulk_out;
    }

    uint8_t code = (evt.status >> 24) & 0xFF;
    if (code == 1 || code == 13) {
        result = 0;
        goto bulk_out;
    }

    if (code == 6) {
        xhci_reset_endpoint(hc, slot_id, db_target);
        xhci_set_tr_dequeue(hc, slot_id, db_target,
                            virt_to_phys((void*)ring) + trb_idx * sizeof(struct xhci_trb),
                            cur_cyc);
        *enqueue = trb_idx;
        *cycle   = cur_cyc;
        evtq_drain(&hc->transfer_evt_q[slot_id][db_target]);
        result = -6;
    }

bulk_out:
    xhci_unlock(&hc->ep_lock[slot_id][db_target]);
    return result;
}

int xhci_bulk_transfer_len(xhci_controller_t *hc, uint8_t slot_id,
                           volatile struct xhci_trb *ring, uint32_t *enqueue, uint8_t *cycle,
                           uint8_t db_target, uint64_t data_phys, uint32_t length, uint8_t dir_in,
                           uint32_t *out_actual)
{
    (void)dir_in;
    if (out_actual) *out_actual = 0;
    if (hc->command_failed) return -ENODEV;
    if (!hc->dma64 && length &&
        (data_phys >= 0x100000000ULL || length > 0x100000000ULL - data_phys))
        return -EOVERFLOW;
    if (length == 0) return 0;

    if (!xhci_lock_timeout(&hc->ep_lock[slot_id][db_target], 2000000)) return -EBUSY;

    uint32_t idx = *enqueue;
    if (idx >= COUNT_OF_TRB - 1) {
        ring[idx].param   = virt_to_phys((void*)ring);
        ring[idx].status  = 0;
        ring[idx].control = (TRB_LINK << 10) | (1u << 1) | *cycle;
        *enqueue = 0;
        *cycle ^= 1;
        idx = 0;
    }
    uint32_t trb_idx   = idx;
    uint8_t  cur_cyc   = *cycle;

    ring[trb_idx].param   = data_phys;
    ring[trb_idx].status  = length & 0x1FFFF;
    ring[trb_idx].control = (TRB_NORMAL << 10) | (1u << 5) | *cycle;
    (*enqueue)++;

    evtq_drain(&hc->transfer_evt_q[slot_id][db_target]);

    __asm__ volatile("mfence" ::: "memory");
    hc->db_regs[slot_id] = db_target;

    struct xhci_trb evt;
    int result = -2;
    if (xhci_wait_transfer(hc, slot_id, db_target, &evt) != 0) {
        xhci_reset_endpoint(hc, slot_id, db_target);
        xhci_set_tr_dequeue(hc, slot_id, db_target,
                            virt_to_phys((void*)ring) + trb_idx * sizeof(struct xhci_trb),
                            cur_cyc);
        *enqueue = trb_idx;
        *cycle   = cur_cyc;
        evtq_drain(&hc->transfer_evt_q[slot_id][db_target]);
        result = -1;
        goto bulk_len_out;
    }

    uint8_t code = (evt.status >> 24) & 0xFF;
    if (code == 1 || code == 13) {
        if (out_actual) {
            uint32_t residual = evt.status & 0xFFFFFF;
            *out_actual = (residual < length) ? (length - residual) : 0;
        }
        result = 0;
        goto bulk_len_out;
    }

    if (code == 6) {
        xhci_reset_endpoint(hc, slot_id, db_target);
        xhci_set_tr_dequeue(hc, slot_id, db_target,
                            virt_to_phys((void*)ring) + trb_idx * sizeof(struct xhci_trb),
                            cur_cyc);
        *enqueue = trb_idx;
        *cycle   = cur_cyc;
        evtq_drain(&hc->transfer_evt_q[slot_id][db_target]);
        result = -6;
    }

bulk_len_out:
    xhci_unlock(&hc->ep_lock[slot_id][db_target]);
    return result;
}

int usb_bot_command(xhci_controller_t *hc, usb_msd_t *msd,
                    uint8_t *cdb, uint8_t cdb_len,
                    uint64_t data_phys, uint32_t data_len, uint8_t dir_in)
{
    uint64_t cbw_phys = msd->cmd_page_phys;
    struct bot_cbw *cbw = (struct bot_cbw *)msd->cmd_page_virt;
    memset(cbw, 0, 512);

    cbw->signature   = BOT_CBW_SIGNATURE;
    cbw->tag         = msd->bot_tag++;
    cbw->data_length = data_len;
    cbw->flags       = dir_in ? 0x80 : 0x00;
    cbw->lun         = 0;
    cbw->cdb_len     = cdb_len;
    memcpy(cbw->cdb, cdb, cdb_len);

    int res = xhci_bulk_transfer(hc, msd->slot_id,
                                  msd->bulk_out_ring, &msd->bulk_out_enqueue, &msd->bulk_out_cycle,
                                  msd->bulk_out_db, cbw_phys, 31, 0);
    if (res) return res;

    if (data_len > 0 && data_phys != 0) {
        uint32_t remaining = data_len;
        uint64_t cur_phys = data_phys;
        while (remaining > 0) {
            uint32_t chunk = remaining > 0x10000 ? 0x10000 : remaining;
            if (dir_in) {
                res = xhci_bulk_transfer(hc, msd->slot_id,
                                          msd->bulk_in_ring, &msd->bulk_in_enqueue, &msd->bulk_in_cycle,
                                          msd->bulk_in_db, cur_phys, chunk, 1);
            } else {
                res = xhci_bulk_transfer(hc, msd->slot_id,
                                          msd->bulk_out_ring, &msd->bulk_out_enqueue, &msd->bulk_out_cycle,
                                          msd->bulk_out_db, cur_phys, chunk, 0);
            }
            if (res) return res;
            cur_phys += chunk;
            remaining -= chunk;
        }
    }

    uint64_t csw_phys = msd->cmd_page_phys + 512;
    struct bot_csw *csw = (struct bot_csw *)((uint8_t *)msd->cmd_page_virt + 512);
    memset(csw, 0, 512);

    res = xhci_bulk_transfer(hc, msd->slot_id,
                              msd->bulk_in_ring, &msd->bulk_in_enqueue, &msd->bulk_in_cycle,
                              msd->bulk_in_db, csw_phys, 13, 1);
    if (res) return res;

    if (csw->signature != BOT_CSW_SIGNATURE) return -3;
    if (csw->status != 0) return -4;
    return 0;
}

int scsi_inquiry(xhci_controller_t *hc, usb_msd_t *msd) {
    uint64_t buf_phys = msd->cmd_page_phys + 1024;
    uint8_t *buf = (uint8_t *)msd->cmd_page_virt + 1024;
    memset(buf, 0, 512);

    uint8_t cdb[6] = { 0x12, 0, 0, 0, 36, 0 };
    int res = usb_bot_command(hc, msd, cdb, 6, buf_phys, 36, 1);
    if (res == 0) {
        console_print_string("[SCSI] Vendor: ");
        for (int i = 8; i < 16; i++)
            if (buf[i] >= 0x20 && buf[i] <= 0x7E) { char c[2] = {(char)buf[i],0}; console_print_string(c); }
        console_print_string(" | Product: ");
        for (int i = 16; i < 32; i++)
            if (buf[i] >= 0x20 && buf[i] <= 0x7E) { char c[2] = {(char)buf[i],0}; console_print_string(c); }
        console_print_string("\n");
    }
    return res;
}

int scsi_read_capacity(xhci_controller_t *hc, usb_msd_t *msd) {
    uint64_t buf_phys = msd->cmd_page_phys + 1024;
    uint8_t *buf = (uint8_t *)msd->cmd_page_virt + 1024;
    memset(buf, 0, 512);

    uint8_t cdb[10] = { 0x25, 0,0,0,0,0,0,0,0,0 };
    int res = usb_bot_command(hc, msd, cdb, 10, buf_phys, 8, 1);
    if (res == 0) {
        uint32_t last_lba = ((uint32_t)buf[0]<<24)|((uint32_t)buf[1]<<16)|((uint32_t)buf[2]<<8)|(uint32_t)buf[3];
        uint32_t blk_size = ((uint32_t)buf[4]<<24)|((uint32_t)buf[5]<<16)|((uint32_t)buf[6]<<8)|(uint32_t)buf[7];
        msd->block_count = last_lba + 1;
        msd->block_size  = blk_size;
        console_print_string("[SCSI] Capacity: ");
        console_print_num(msd->block_count);
        console_print_string(" x ");
        console_print_num(msd->block_size);
        console_print_string(" bytes\n");
    }
    return res;
}

int scsi_read_sectors(xhci_controller_t *hc, usb_msd_t *msd, uint32_t lba, uint16_t count, uint64_t buf_phys) {
    uint8_t cdb[10] = {
        0x28, 0,
        (uint8_t)(lba>>24),(uint8_t)(lba>>16),(uint8_t)(lba>>8),(uint8_t)lba,
        0,
        (uint8_t)(count>>8),(uint8_t)count,
        0
    };
    return usb_bot_command(hc, msd, cdb, 10, buf_phys, (uint32_t)count * msd->block_size, 1);
}

int scsi_write_sectors(xhci_controller_t *hc, usb_msd_t *msd, uint32_t lba, uint16_t count, uint64_t buf_phys) {
    uint8_t cdb[10] = {
        0x2A, 0,
        (uint8_t)(lba>>24),(uint8_t)(lba>>16),(uint8_t)(lba>>8),(uint8_t)lba,
        0,
        (uint8_t)(count>>8),(uint8_t)count,
        0
    };
    return usb_bot_command(hc, msd, cdb, 10, buf_phys, (uint32_t)count * msd->block_size, 0);
}

int xhci_configure_bulk_endpoints(xhci_controller_t *hc, usb_msd_t *msd,
                                   uint8_t ep_out_addr, uint8_t ep_in_addr, uint16_t mps)
{
    uint32_t hccparams1 = xhci_read32(hc->cap_regs, 0x10);
    uint32_t ctx_size = (hccparams1 & (1u << 2)) ? 64 : 32;

    uint8_t dci_out = (ep_out_addr & 0x0F) * 2 + 0;
    uint8_t dci_in  = (ep_in_addr  & 0x0F) * 2 + 1;
    uint8_t max_dci = (dci_out > dci_in) ? dci_out : dci_in;

    uint64_t input_phys = (uint64_t)xhci_dma_alloc_pages(1);
    if (!input_phys) return -ENOMEM;
    uint8_t *input_virt = (uint8_t *)phys_to_virt(input_phys);
    memset(input_virt, 0, 4096);

    uint32_t *icc = (uint32_t *)input_virt;
    icc[1] = (1u << 0) | (1u << dci_out) | (1u << dci_in);

    uint32_t *slot_ctx = (uint32_t *)(input_virt + ctx_size);
    uint32_t *current_dev_ctx = (uint32_t *)phys_to_virt(((uint64_t *)hc->dcbaa)[msd->slot_id]);
    memcpy(slot_ctx, current_dev_ctx, ctx_size);
    slot_ctx[0] = (slot_ctx[0] & ~(0x1Fu << 27)) | ((uint32_t)max_dci << 27);

    uint32_t *ep_out_ctx = (uint32_t *)(input_virt + ctx_size * (dci_out + 1));
    ep_out_ctx[0] = 0;
    ep_out_ctx[1] = ((uint32_t)mps << 16) | (2u << 3) | (3u << 1);
    *(uint64_t *)&ep_out_ctx[2] = msd->bulk_out_ring_phys | 1u;
    ep_out_ctx[4] = mps;

    uint32_t *ep_in_ctx = (uint32_t *)(input_virt + ctx_size * (dci_in + 1));
    ep_in_ctx[0] = 0;
    ep_in_ctx[1] = ((uint32_t)mps << 16) | (6u << 3) | (3u << 1);
    *(uint64_t *)&ep_in_ctx[2] = msd->bulk_in_ring_phys | 1u;
    ep_in_ctx[4] = mps;

    struct xhci_trb evt;
    if (xhci_submit_command(hc, input_phys, 0,
                            (TRB_CONFIGURE_ENDPOINT << 10) |
                            ((uint32_t)msd->slot_id << 24), &evt) != 0) {
        pmm_free_pages((void*)input_phys, 1);
        return -1;
    }
    uint8_t code = (evt.status >> 24) & 0xFF;
    pmm_free_pages((void*)input_phys, 1);
    if (code != 1) return -2;
    return 0;
}

int xhci_configure_endpoints_multi(xhci_controller_t *hc, uint8_t slot_id,
                                   const uint8_t *out_addrs, const uint64_t *out_rings, int out_count,
                                   uint8_t in_addr, uint64_t in_ring, uint16_t mps)
{
    uint32_t hccparams1 = xhci_read32(hc->cap_regs, 0x10);
    uint32_t ctx_size = (hccparams1 & (1u << 2)) ? 64 : 32;

    uint8_t dci_in = (in_addr & 0x0F) * 2 + 1;
    uint8_t max_dci = dci_in;

    uint64_t input_phys = (uint64_t)xhci_dma_alloc_pages(1);
    if (!input_phys) return -ENOMEM;
    uint8_t *input_virt = (uint8_t *)phys_to_virt(input_phys);
    memset(input_virt, 0, 4096);

    uint32_t *icc = (uint32_t *)input_virt;
    icc[1] = (1u << 0) | (1u << dci_in);

    for (int i = 0; i < out_count; i++) {
        uint8_t dci_out = (out_addrs[i] & 0x0F) * 2 + 0;
        icc[1] |= (1u << dci_out);
        if (dci_out > max_dci) max_dci = dci_out;
        uint32_t *ep_ctx = (uint32_t *)(input_virt + ctx_size * (dci_out + 1));
        ep_ctx[0] = 0;
        ep_ctx[1] = ((uint32_t)mps << 16) | (2u << 3) | (3u << 1);
        *(uint64_t *)&ep_ctx[2] = out_rings[i] | 1u;
        ep_ctx[4] = mps;
    }

    uint32_t *slot_ctx = (uint32_t *)(input_virt + ctx_size);
    uint32_t *current_dev_ctx = (uint32_t *)phys_to_virt(((uint64_t *)hc->dcbaa)[slot_id]);
    memcpy(slot_ctx, current_dev_ctx, ctx_size);
    slot_ctx[0] = (slot_ctx[0] & ~(0x1Fu << 27)) | ((uint32_t)max_dci << 27);

    uint32_t *ep_in_ctx = (uint32_t *)(input_virt + ctx_size * (dci_in + 1));
    ep_in_ctx[0] = 0;
    ep_in_ctx[1] = ((uint32_t)mps << 16) | (6u << 3) | (3u << 1);
    *(uint64_t *)&ep_in_ctx[2] = in_ring | 1u;
    ep_in_ctx[4] = mps;

    struct xhci_trb evt;
    if (xhci_submit_command(hc, input_phys, 0,
                            (TRB_CONFIGURE_ENDPOINT << 10) |
                            ((uint32_t)slot_id << 24), &evt) != 0) {
        pmm_free_pages((void*)input_phys, 1);
        return -1;
    }
    uint8_t code = (evt.status >> 24) & 0xFF;
    pmm_free_pages((void*)input_phys, 1);
    if (code != 1) return -2;
    return 0;
}

int usb_msd_block_read(void *dev, uint64_t lba, uint32_t count, void *buffer) {
    usb_msd_t *msd = (usb_msd_t *)dev;
    uint64_t phys = virt_to_phys(buffer);
    return scsi_read_sectors(msd->hc, msd, (uint32_t)lba, (uint16_t)count, phys);
}

int usb_msd_block_write(void *dev, uint64_t lba, uint32_t count, void *buffer) {
    usb_msd_t *msd = (usb_msd_t *)dev;
    uint64_t phys = virt_to_phys(buffer);
    return scsi_write_sectors(msd->hc, msd, (uint32_t)lba, (uint16_t)count, phys);
}

void usb_msd_clear_halt(xhci_controller_t *hc, uint8_t slot_id, uint8_t ep_addr) {
    struct usb_setup_packet sp;
    sp.bmRequestType = 0x02;
    sp.bRequest = 0x01;
    sp.wValue = 0x0000;
    sp.wIndex = ep_addr;
    sp.wLength = 0;

    xhci_control_transfer(hc, slot_id,
                          hc->ep0_ring[slot_id],
                          &hc->ep0_enqueue[slot_id],
                          &hc->ep0_cycle[slot_id],
                          &sp, 0, 0);
}

void init_usb_mass_storage(xhci_controller_t *hc, uint8_t slot_id, uint8_t speed,
                            uint8_t ep_out_addr, uint8_t ep_in_addr, uint16_t mps)
{
    console_print_string("[MSD] Init USB Mass Storage...\n");

    usb_msd_t *msd = (usb_msd_t *)malloc_alloc(sizeof(usb_msd_t));
    if (!msd) return;
    memset(msd, 0, sizeof(usb_msd_t));

    msd->hc = hc;
    msd->slot_id = slot_id;
    msd->speed = speed;
    msd->bot_tag = 1;

    uint8_t dci_out = (ep_out_addr & 0x0F) * 2 + 0;
    uint8_t dci_in  = (ep_in_addr  & 0x0F) * 2 + 1;

    msd->cmd_page_phys = (uint64_t)xhci_dma_alloc_pages(1);
    if (!msd->cmd_page_phys) { malloc_free(msd); return; }
    msd->cmd_page_virt = phys_to_virt(msd->cmd_page_phys);
    memset(msd->cmd_page_virt, 0, 4096);

    msd->bulk_out_ring_phys = (uint64_t)xhci_dma_alloc_pages(1);
    if (!msd->bulk_out_ring_phys) {
        pmm_free_pages((void*)msd->cmd_page_phys, 1);
        malloc_free(msd);
        return;
    }
    msd->bulk_out_ring = (volatile struct xhci_trb *)phys_to_virt(msd->bulk_out_ring_phys);
    memset((void*)msd->bulk_out_ring, 0, 4096);
    msd->bulk_out_enqueue = 0; msd->bulk_out_cycle = 1; msd->bulk_out_db = dci_out;
    msd->bulk_out_ring[COUNT_OF_TRB-1].param   = msd->bulk_out_ring_phys;
    msd->bulk_out_ring[COUNT_OF_TRB-1].control = (TRB_LINK << 10) | (1u << 1) | 1u;

    msd->bulk_in_ring_phys = (uint64_t)xhci_dma_alloc_pages(1);
    if (!msd->bulk_in_ring_phys) {
        pmm_free_pages((void*)msd->cmd_page_phys, 1);
        pmm_free_pages((void*)msd->bulk_out_ring_phys, 1);
        malloc_free(msd);
        return;
    }
    msd->bulk_in_ring = (volatile struct xhci_trb *)phys_to_virt(msd->bulk_in_ring_phys);
    memset((void*)msd->bulk_in_ring, 0, 4096);
    msd->bulk_in_enqueue = 0; msd->bulk_in_cycle = 1; msd->bulk_in_db = dci_in;
    msd->bulk_in_ring[COUNT_OF_TRB-1].param   = msd->bulk_in_ring_phys;
    msd->bulk_in_ring[COUNT_OF_TRB-1].control = (TRB_LINK << 10) | (1u << 1) | 1u;

    if (xhci_configure_bulk_endpoints(hc, msd, ep_out_addr, ep_in_addr, mps) != 0) {
        console_print_string("[MSD] bulk endpoint configuration failed\n");
        pmm_free_pages((void*)msd->cmd_page_phys, 1);
        pmm_free_pages((void*)msd->bulk_out_ring_phys, 1);
        pmm_free_pages((void*)msd->bulk_in_ring_phys, 1);
        malloc_free(msd);
        return;
    }

    cooperative_delay(300000);

    int inquiry_res = -1;
    for (int retry = 0; retry < 5; retry++) {
        inquiry_res = scsi_inquiry(hc, msd);
        if (inquiry_res == 0) break;

        usb_msd_clear_halt(hc, slot_id, ep_out_addr);
        usb_msd_clear_halt(hc, slot_id, ep_in_addr);
        cooperative_delay(500000);
    }

    if (inquiry_res != 0) {
        console_print_string("[MSD] SCSI INQUIRY failed, disk not available\n");
        pmm_free_pages((void*)msd->cmd_page_phys, 1);
        pmm_free_pages((void*)msd->bulk_out_ring_phys, 1);
        pmm_free_pages((void*)msd->bulk_in_ring_phys, 1);
        malloc_free(msd);
        return;
    }

    int cap_res = -1;
    for (int retry = 0; retry < 3; retry++) {
        cap_res = scsi_read_capacity(hc, msd);
        if (cap_res == 0) break;

        usb_msd_clear_halt(hc, slot_id, ep_out_addr);
        usb_msd_clear_halt(hc, slot_id, ep_in_addr);
        cooperative_delay(300000);
    }

    if (cap_res != 0) {
        console_print_string("[MSD] SCSI READ CAPACITY failed, disk not available\n");
        pmm_free_pages((void*)msd->cmd_page_phys, 1);
        pmm_free_pages((void*)msd->bulk_out_ring_phys, 1);
        pmm_free_pages((void*)msd->bulk_in_ring_phys, 1);
        malloc_free(msd);
        return;
    }

    struct block_device *bd = (struct block_device *)malloc_alloc(sizeof(*bd));
    if (!bd) {
        console_print_string("[MSD] out of memory for block device\n");
        pmm_free_pages((void*)msd->cmd_page_phys, 1);
        pmm_free_pages((void*)msd->bulk_out_ring_phys, 1);
        pmm_free_pages((void*)msd->bulk_in_ring_phys, 1);
        malloc_free(msd);
        return;
    }
    memset(bd, 0, sizeof(*bd));
    bd->type                = DISK_TYPE_USB;
    bd->sector_size         = msd->block_size;
    bd->sector_count        = msd->block_count;
    bd->local_device_struct = (void *)msd;
    bd->read                = (block_read_fn)usb_msd_block_read;
    bd->write               = (block_write_fn)usb_msd_block_write;
    bd->flush               = NULL;

    msd->bd = bd;
    register_device(bd);
    console_print_string("[MSD] Registered successfully as ");
    console_print_string(bd->name);
    console_print_string("\n");
}

typedef struct {
    uint32_t usage_page;
    int32_t  logical_min;
    int32_t  logical_max;
    uint32_t report_size;
    uint32_t report_count;
    uint32_t report_id;
} hid_global_state_t;

static int32_t hid_sign_extend(uint32_t value, uint32_t size) {
    if (size == 0) return 0;
    uint32_t bits = size * 8;
    if (bits < 32 && (value & (1u << (bits - 1))))
        return (int32_t)((int64_t)value - (int64_t)(1ULL << bits));
    return (int32_t)value;
}

static int32_t hid_extract_field(const uint8_t *report, uint32_t report_len,
                                 const struct hid_field *field) {
    if (!report || !field || !field->present || field->bit_size == 0) return 0;
    uint32_t bits = field->bit_size;
    if (bits > 32) bits = 32;
    uint64_t value = 0;
    for (uint32_t i = 0; i < bits; i++) {
        uint32_t bit_index = (uint32_t)field->bit_offset + i;
        uint32_t byte_index = bit_index >> 3;
        if (byte_index >= report_len) return 0;
        if (report[byte_index] & (uint8_t)(1u << (bit_index & 7)))
            value |= 1ULL << i;
    }
    if (bits < 32 && (field->logical_min < 0 || field->relative) &&
        (value & (1ULL << (bits - 1))))
        value |= ~((1ULL << bits) - 1ULL);
    return (int32_t)value;
}

static void hid_set_output_value(uint8_t *report, uint32_t report_len,
                                 const struct hid_field *field, uint32_t value) {
    if (!report || !field || !field->present) return;
    for (uint32_t i = 0; i < field->bit_size; i++) {
        uint32_t bit_index = (uint32_t)field->bit_offset + i;
        uint32_t byte_index = bit_index >> 3;
        if (byte_index >= report_len) return;
        if ((value >> i) & 1u)
            report[byte_index] |= (uint8_t)(1u << (bit_index & 7));
        else
            report[byte_index] &= (uint8_t)~(1u << (bit_index & 7));
    }
}

static void hid_parse_report_descriptor(struct usb_hid *hid, const uint8_t *desc, uint32_t len) {
    struct hid_field mouse_buttons = {0};
    struct hid_field mouse_x = {0};
    struct hid_field mouse_y = {0};
    struct hid_field mouse_wheel = {0};
    struct hid_field kbd_modifiers = {0};
    struct hid_field kbd_keys = {0};
    struct hid_field led_num = {0};
    struct hid_field led_caps = {0};
    struct hid_field led_scroll = {0};
    uint8_t mouse_button_count = 0;
    uint8_t kbd_key_count = 0;
    uint8_t mouse_id = 0;
    uint8_t kbd_modifiers_id = 0;
    uint8_t kbd_keys_id = 0;
    uint8_t led_id = 0;
    bool mouse_id_known = false;
    bool led_id_known = false;
    hid_global_state_t state = {0};
    hid_global_state_t stack[4];
    uint32_t stack_depth = 0;
    uint32_t usages[32];
    uint32_t usage_count = 0;
    uint32_t usage_min = 0;
    uint32_t usage_max = 0;
    bool have_usage_min = false;
    bool have_usage_max = false;
    uint16_t input_bits[256] = {0};
    uint16_t output_bits[256] = {0};
    uint16_t feature_bits[256] = {0};

    hid->mouse_layout_valid = false;
    hid->kbd_layout_valid = false;
    hid->desc_led_layout_valid = false;
    hid->has_input_report_id = false;
    hid->input_report_id = 0;

    for (uint32_t pos = 0; pos < len;) {
        if (desc[pos] == 0xFE) {
            if (pos + 2 >= len) break;
            pos += (uint32_t)desc[pos + 1] + 3;
            continue;
        }

        uint8_t prefix = desc[pos++];
        uint32_t size_code = prefix & 0x03;
        uint32_t size = size_code == 3 ? 4 : size_code;
        if (pos + size > len) break;
        uint32_t value = 0;
        for (uint32_t i = 0; i < size; i++)
            value |= (uint32_t)desc[pos + i] << (i * 8);
        pos += size;

        uint32_t type = (prefix >> 2) & 0x03;
        uint32_t tag = (prefix >> 4) & 0x0F;

        if (type == 1) {
            if (tag == 0) {
                state.usage_page = value;
            } else if (tag == 1) {
                state.logical_min = hid_sign_extend(value, size);
            } else if (tag == 2) {
                state.logical_max = state.logical_min < 0 ? hid_sign_extend(value, size) : (int32_t)value;
            } else if (tag == 7) {
                state.report_size = value;
            } else if (tag == 8) {
                state.report_id = value & 0xFF;
            } else if (tag == 9) {
                state.report_count = value;
            } else if (tag == 10) {
                if (stack_depth < 4) stack[stack_depth++] = state;
            } else if (tag == 11) {
                if (stack_depth > 0) state = stack[--stack_depth];
            }
            continue;
        }

        if (type == 2) {
            if (tag == 0) {
                if (usage_count < 32) usages[usage_count++] = value;
            } else if (tag == 1) {
                usage_min = value;
                have_usage_min = true;
            } else if (tag == 2) {
                usage_max = value;
                have_usage_max = true;
            }
            continue;
        }

        if (type != 0) continue;

        if (tag == 8 || tag == 9 || tag == 11) {
            uint16_t *bits = tag == 8 ? input_bits : tag == 9 ? output_bits : feature_bits;
            uint32_t report_id = state.report_id & 0xFF;
            uint32_t base_offset = bits[report_id];
            bool is_constant = (value & 0x01) != 0;
            bool is_variable = (value & 0x02) != 0;
            bool is_relative = (value & 0x04) != 0;

            if (tag == 8 && !is_constant && report_id != 0 && !hid->has_input_report_id) {
                hid->has_input_report_id = true;
                hid->input_report_id = (uint8_t)report_id;
            }

            if (!is_constant) {
                for (uint32_t idx = 0; idx < state.report_count; idx++) {
                    uint32_t field_page = state.usage_page;
                    uint32_t usage = 0;
                    bool have_usage = false;
                    if (have_usage_min) {
                        usage = usage_min + idx;
                        have_usage = !have_usage_max || usage <= usage_max;
                    } else if (idx < usage_count) {
                        usage = usages[idx];
                        have_usage = true;
                    } else if (usage_count > 0) {
                        usage = usages[usage_count - 1];
                        have_usage = true;
                    }
                    if (have_usage && usage > 0xFFFF) {
                        field_page = usage >> 16;
                        usage &= 0xFFFF;
                    }

                    struct hid_field field;
                    field.bit_offset = (uint16_t)(base_offset + idx * state.report_size);
                    field.bit_size = (uint16_t)state.report_size;
                    field.logical_min = state.logical_min;
                    field.logical_max = state.logical_max;
                    field.present = state.report_size != 0;
                    field.relative = is_relative;

                    if (tag == 8 && hid->protocol == 2 && is_variable && have_usage) {
                        bool report_match = !mouse_id_known || report_id == mouse_id;
                        if (report_match && field_page == 0x09 && idx == 0 && usage == 1) {
                            if (!mouse_id_known) {
                                mouse_id = (uint8_t)report_id;
                                mouse_id_known = true;
                            }
                            mouse_buttons = field;
                            mouse_button_count = state.report_count > 8 ? 8 : (uint8_t)state.report_count;
                        } else if (report_match && field_page == 0x01 && is_relative &&
                                   (usage == 0x30 || usage == 0x31 || usage == 0x38)) {
                            if (!mouse_id_known) {
                                mouse_id = (uint8_t)report_id;
                                mouse_id_known = true;
                            }
                            if (report_id == mouse_id) {
                                if (usage == 0x30) mouse_x = field;
                                else if (usage == 0x31) mouse_y = field;
                                else mouse_wheel = field;
                            }
                        }
                    }

                    if (tag == 8 && hid->protocol == 1 && have_usage && field_page == 0x07) {
                        if (is_variable && state.report_size == 1 && usage >= 0xE0 && usage <= 0xE7) {
                            if (!kbd_modifiers.present) {
                                kbd_modifiers = field;
                                kbd_modifiers_id = (uint8_t)report_id;
                            }
                        } else if (!is_variable && idx == 0 && state.report_size >= 4 && state.report_size <= 16) {
                            kbd_keys = field;
                            kbd_key_count = state.report_count > 16 ? 16 : (uint8_t)state.report_count;
                            kbd_keys_id = (uint8_t)report_id;
                        }
                    }

                    if (tag == 9 && hid->protocol == 1 && is_variable && have_usage && field_page == 0x08) {
                        if (!led_id_known) {
                            led_id = (uint8_t)report_id;
                            led_id_known = true;
                        }
                        if (report_id == led_id) {
                            if (usage == 1) led_num = field;
                            else if (usage == 2) led_caps = field;
                            else if (usage == 3) led_scroll = field;
                        }
                    }
                }
            }

            uint32_t total_bits = base_offset + state.report_size * state.report_count;
            bits[report_id] = total_bits > 0xFFFF ? 0xFFFF : (uint16_t)total_bits;
        }

        usage_count = 0;
        have_usage_min = false;
        have_usage_max = false;
    }

    if (mouse_buttons.present && mouse_x.present && mouse_y.present) {
        hid->mouse_buttons = mouse_buttons;
        hid->mouse_button_count = mouse_button_count;
        hid->mouse_x = mouse_x;
        hid->mouse_y = mouse_y;
        hid->mouse_wheel = mouse_wheel;
        hid->mouse_report_id = mouse_id;
        hid->mouse_report_bits = input_bits[mouse_id];
        hid->mouse_layout_valid = true;
    }

    if (kbd_modifiers.present && kbd_keys.present && kbd_modifiers_id == kbd_keys_id) {
        hid->kbd_modifiers = kbd_modifiers;
        hid->kbd_keys = kbd_keys;
        hid->kbd_key_count = kbd_key_count;
        hid->kbd_report_id = kbd_keys_id;
        hid->kbd_report_bits = input_bits[kbd_keys_id];
        hid->kbd_layout_valid = true;
    }

    if (led_num.present || led_caps.present || led_scroll.present) {
        hid->led_num = led_num;
        hid->led_caps = led_caps;
        hid->led_scroll = led_scroll;
        hid->desc_led_report_id = led_id;
        hid->desc_led_report_len = (uint16_t)(((output_bits[led_id] + 7) / 8) + (led_id ? 1 : 0));
        if (hid->desc_led_report_len == 0) hid->desc_led_report_len = led_id ? 2 : 1;
        hid->desc_led_layout_valid = true;
    }
}

static void handle_keyboard_report(struct usb_hid *hid, const uint8_t *r, uint32_t report_len) {
    if (!r || report_len == 0 || report_len > 64) return;
    if (report_len > hid->report_size) report_len = hid->report_size;

    uint8_t cur_keys[16] = {0};
    uint32_t cur_key_count = 0;
    uint8_t modifiers = 0;

    bool use_layout = hid->kbd_layout_valid &&
        (hid->report_protocol_active || hid->kbd_report_id == 0);

    if (use_layout) {
        uint32_t expected = (uint32_t)((hid->kbd_report_bits + 7) / 8) +
                            (hid->kbd_report_id ? 1 : 0);
        uint32_t offset = 0;
        if (hid->kbd_report_id) {
            if (report_len < 1 || r[0] != hid->kbd_report_id) return;
            offset = 1;
        }
        if (report_len >= expected) {
            const uint8_t *payload = r + offset;
            uint32_t payload_len = report_len - offset;
            for (uint32_t i = 0; i < 8; i++) {
                struct hid_field field = hid->kbd_modifiers;
                field.bit_offset = (uint16_t)(field.bit_offset + i * field.bit_size);
                if (hid_extract_field(payload, payload_len, &field) != 0)
                    modifiers |= (uint8_t)(1u << i);
            }
            cur_key_count = hid->kbd_key_count;
            if (cur_key_count > 16) cur_key_count = 16;
            for (uint32_t i = 0; i < cur_key_count; i++) {
                struct hid_field field = hid->kbd_keys;
                field.bit_offset = (uint16_t)(field.bit_offset + i * field.bit_size);
                int32_t usage = hid_extract_field(payload, payload_len, &field);
                cur_keys[i] = usage > 0 && usage <= 0xFF ? (uint8_t)usage : 0;
            }
        }
    } else {
        uint32_t offset = 0;
        if (hid->report_protocol_active && hid->has_input_report_id &&
            report_len >= 9 && r[0] == hid->input_report_id)
            offset = 1;
        if (report_len >= offset + 8) {
            modifiers = r[offset];
            cur_key_count = 6;
            memcpy(cur_keys, &r[offset + 2], 6);
        }
    }

    static const uint16_t mod_codes[8] = {
        KEY_LCTRL, KEY_LSHIFT, KEY_LALT, KEY_LGUI,
        KEY_RCTRL, KEY_RSHIFT, KEY_RALT, KEY_RGUI
    };

    uint8_t cur_codes[32];
    uint8_t cur_count = 0;

    for (uint32_t i = 0; i < 8; i++) {
        if (!(modifiers & (uint8_t)(1u << i))) continue;
        uint8_t kc = (uint8_t)mod_codes[i];
        bool seen = false;
        for (uint32_t j = 0; j < cur_count; j++)
            if (cur_codes[j] == kc) { seen = true; break; }
        if (!seen && cur_count < sizeof(cur_codes)) cur_codes[cur_count++] = kc;
    }

    for (uint32_t i = 0; i < cur_key_count; i++) {
        uint16_t kc = hid_usage_to_keycode(cur_keys[i]);
        if (kc == 0 || kc >= KEY_MAX) continue;
        bool seen = false;
        for (uint32_t j = 0; j < cur_count; j++)
            if (cur_codes[j] == (uint8_t)kc) { seen = true; break; }
        if (!seen && cur_count < sizeof(cur_codes))
            cur_codes[cur_count++] = (uint8_t)kc;
    }

    static const uint8_t hid_mod_codes[8] = {
        KEY_LCTRL, KEY_LSHIFT, KEY_LALT, KEY_LGUI,
        KEY_RCTRL, KEY_RSHIFT, KEY_RALT, KEY_RGUI
    };
    for (uint32_t i = 0; i < 8; i++) {
        if (!(modifiers & (uint8_t)(1u << i)))
            inout_set_key_state(hid_mod_codes[i], false);
    }

    for (uint32_t i = 0; i < hid->active_keycode_count; i++) {
        uint8_t kc = hid->active_keycodes[i];
        bool still = false;
        for (uint32_t j = 0; j < cur_count; j++)
            if (cur_codes[j] == kc) { still = true; break; }
        if (!still) inout_set_key_state(kc, false);
    }
    for (uint32_t i = 0; i < cur_count; i++)
        inout_set_key_state(cur_codes[i], true);

    memcpy(hid->active_keycodes, cur_codes, cur_count);
    hid->active_keycode_count = cur_count;

    memset(hid->prev_keys, 0, sizeof(hid->prev_keys));
    if (cur_key_count > sizeof(hid->prev_keys))
        cur_key_count = sizeof(hid->prev_keys);
    memcpy(hid->prev_keys, cur_keys, cur_key_count);
    hid->prev_key_count = (uint8_t)cur_key_count;
    hid->prev_modifiers = modifiers;
}

static void handle_mouse_report(struct usb_hid *hid, const uint8_t *r, uint32_t report_len) {
    if (!r || report_len == 0 || report_len > 64) return;
    if (report_len > hid->report_size) report_len = hid->report_size;

    int dx = 0;
    int dy = 0;
    int dz = 0;
    uint8_t buttons = 0;
    bool parsed = false;

    bool use_layout = hid->mouse_layout_valid &&
        (hid->report_protocol_active || hid->mouse_report_id == 0);

    if (use_layout) {
        uint32_t expected = (uint32_t)((hid->mouse_report_bits + 7) / 8) +
                            (hid->mouse_report_id ? 1 : 0);
        uint32_t offset = 0;
        if (hid->mouse_report_id) {
            if (report_len < 1 || r[0] != hid->mouse_report_id) return;
            offset = 1;
        }
        if (report_len < expected) return;

        const uint8_t *payload = r + offset;
        uint32_t payload_len = report_len - offset;
        uint32_t button_bits = 0;
        for (uint32_t i = 0; i < hid->mouse_button_count && i < 8; i++) {
            struct hid_field field = hid->mouse_buttons;
            field.bit_offset = (uint16_t)(field.bit_offset + i * field.bit_size);
            if (hid_extract_field(payload, payload_len, &field) != 0)
                button_bits |= 1u << i;
        }
        buttons = (uint8_t)button_bits;
        dx = hid_extract_field(payload, payload_len, &hid->mouse_x);
        dy = hid_extract_field(payload, payload_len, &hid->mouse_y);
        dz = hid_extract_field(payload, payload_len, &hid->mouse_wheel);
        parsed = true;
    }

    if (!parsed) {
        uint32_t offset = 0;
        if (hid->report_protocol_active && hid->has_input_report_id &&
            r[0] == hid->input_report_id)
            offset = 1;
        if (report_len < offset + 3) return;
        if (r[offset] & 0xC0) return;
        buttons = (uint8_t)(r[offset] & 0x07);
        dx = (int8_t)r[offset + 1];
        dy = (int8_t)r[offset + 2];
        dz = report_len > offset + 3 ? (int8_t)r[offset + 3] : 0;
    }

    if (dx > 2048 || dx < -2048 || dy > 2048 || dy < -2048) return;
    if (dx > 127) dx = 127; else if (dx < -127) dx = -127;
    if (dy > 127) dy = 127; else if (dy < -127) dy = -127;
    if (dz > 127) dz = 127; else if (dz < -127) dz = -127;

    uint8_t prev = hid->prev_buttons;

    if (dx != 0 || dy != 0 || dz != 0) {
        inout_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = INOUT_EVENT_MOUSE;
        ev.data.mouse.mouse_state = INOUT_MOUSE_MOVE;
        ev.data.mouse.x = dx;
        ev.data.mouse.y = dy;
        ev.data.mouse.z = dz;
        inout_add_event(ev);
    }

    struct { uint8_t mask; int btn; } mp[3] = {
        { 0x01, INOUT_MOUSE_LEFTBTN   },
        { 0x02, INOUT_MOUSE_RIGHTBTN  },
        { 0x04, INOUT_MOUSE_CENTERBTN },
    };
    for (int i = 0; i < 3; i++) {
        uint8_t cur_b  = buttons & mp[i].mask;
        uint8_t prev_b = prev    & mp[i].mask;
        if (cur_b != prev_b) {
            inout_event_t ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = INOUT_EVENT_MOUSE;
            ev.data.mouse.mouse_state = cur_b ? INOUT_MOUSE_DOWN : INOUT_MOUSE_UP;
            ev.data.mouse.mouse_btn   = mp[i].btn;
            ev.data.mouse.x = 0;
            ev.data.mouse.y = 0;
            ev.data.mouse.z = 0;
            inout_add_event(ev);
        }
    }

    hid->prev_buttons = buttons;
}

static int usb_hid_send_led_report(struct usb_hid *hid, uint8_t leds,
                                   uint8_t report_id, uint16_t report_len, bool use_fields) {
    if (report_len == 0 || report_len > 64) return -1;
    uint8_t *led_buf = (uint8_t *)phys_to_virt(hid->report_phys + 1024);
    memset(led_buf, 0, 64);

    uint32_t payload_offset = 0;
    if (report_id) {
        led_buf[0] = report_id;
        payload_offset = 1;
    }
    if (report_len <= payload_offset) return -1;

    if (use_fields) {
        hid_set_output_value(led_buf + payload_offset, report_len - payload_offset,
                             &hid->led_num, (leds & 0x01) ? 1 : 0);
        hid_set_output_value(led_buf + payload_offset, report_len - payload_offset,
                             &hid->led_caps, (leds & 0x02) ? 1 : 0);
        hid_set_output_value(led_buf + payload_offset, report_len - payload_offset,
                             &hid->led_scroll, (leds & 0x04) ? 1 : 0);
    } else {
        led_buf[payload_offset] = leds;
    }

    struct usb_setup_packet sp;
    memset(&sp, 0, sizeof(sp));
    sp.bmRequestType = 0x21;
    sp.bRequest = 0x09;
    sp.wValue = (uint16_t)(0x0200 | report_id);
    sp.wIndex = hid->iface_num;
    sp.wLength = report_len;
    return xhci_control_transfer(hid->hc, hid->slot_id,
                                 hid->hc->ep0_ring[hid->slot_id],
                                 &hid->hc->ep0_enqueue[hid->slot_id],
                                 &hid->hc->ep0_cycle[hid->slot_id],
                                 &sp, hid->report_phys + 1024, report_len);
}

static bool usb_hid_send_current_leds(struct usb_hid *hid, uint8_t leds) {
    if (hid->led_layout_known) {
        bool use_fields = hid->desc_led_layout_valid &&
                          hid->led_report_id == hid->desc_led_report_id &&
                          hid->led_report_len == hid->desc_led_report_len;
        int res = usb_hid_send_led_report(hid, leds, hid->led_report_id,
                                          hid->led_report_len, use_fields);
        if (res == 0) {
            hid->led_failure_count = 0;
            return true;
        }

        hid->led_failure_count++;
        if (hid->led_failure_count >= 3) {
            hid->led_failure_count = 0;
            if (hid->desc_led_layout_valid &&
                (hid->led_report_id != hid->desc_led_report_id ||
                 hid->led_report_len != hid->desc_led_report_len)) {
                hid->led_report_id = hid->desc_led_report_id;
                hid->led_report_len = hid->desc_led_report_len;
            } else if (hid->led_report_id != 0 || hid->led_report_len != 1) {
                hid->led_report_id = 0;
                hid->led_report_len = 1;
            } else {
                hid->led_layout_known = false;
                hid->led_probe_stage = 1;
            }
        }
        return false;
    }

    if (hid->led_probe_stage == 0) {
        int res = usb_hid_send_led_report(hid, leds, 0, 1, false);
        if (res == 0) {
            hid->led_report_id = 0;
            hid->led_report_len = 1;
            hid->led_layout_known = true;
            hid->led_failure_count = 0;
            return true;
        }
        hid->led_probe_stage = 1;
        return false;
    }

    int res = usb_hid_send_led_report(hid, leds, 1, 2, false);
    if (res == 0) {
        hid->led_report_id = 1;
        hid->led_report_len = 2;
        hid->led_layout_known = true;
        hid->led_failure_count = 0;
        return true;
    }

    hid->led_probe_stage = 0;
    if (hid->desc_led_layout_valid) {
        hid->led_report_id = hid->desc_led_report_id;
        hid->led_report_len = hid->desc_led_report_len;
        hid->led_layout_known = true;
    }
    return false;
}

static void usb_hid_task(void *arg) {
    struct usb_hid *hid = (struct usb_hid *)arg;
    xhci_controller_t *hc = hid->hc;
    bool transfer_queued = false;
    uint8_t snap[64];

    for (;;) {
        if (hid->disconnected) {
            hid->stopped = 1;
            for (;;) {
                task_sleep(get_current_process());
                schedule_yield_safe();
            }
        }

        if (hid->halted) {
            hid->halted = 0;
            uint32_t now = (uint32_t)get_monotonic_ms();
            if (now - hid->halt_recovery_ms > 50 && hid->ep_addr)
                usb_msd_clear_halt(hc, hid->slot_id, hid->ep_addr);
            hid->halt_recovery_ms = now;

            if (xhci_lock_timeout(&hid->report_lock, 200000)) {
                hid->transfer_pending = 0;
                hid->report_qh = 0;
                hid->report_qt = 0;
                hid->report_ready = 0;
                xhci_unlock(&hid->report_lock);
            }

            xhci_reset_endpoint(hc, hid->slot_id, hid->dci);

            hid->enqueue = 0;
            hid->cycle = 1;
            hid->ring[COUNT_OF_TRB - 1].param   = hid->ring_phys;
            hid->ring[COUNT_OF_TRB - 1].status  = 0;
            hid->ring[COUNT_OF_TRB - 1].control = (TRB_LINK << 10) | (1u << 1) | 1u;
            xhci_set_tr_dequeue(hc, hid->slot_id, hid->dci, hid->ring_phys, 1);

            hid->report_len = 0;
            transfer_queued = false;
        }

        if (hid->protocol == 1) {
            if (hid->led_retry_delay > 0) {
                hid->led_retry_delay--;
            } else if (hid->led_pending) {
                uint8_t desired = hid->led_value;
                hid->led_pending = 0;
                bool sent = usb_hid_send_current_leds(hid, desired);
                if (sent) {
                    hid->led_sent_value = desired;
                    hid->led_sent_valid = true;
                }
                if (hid->led_value != desired) {
                    hid->led_pending = 1;
                } else if (!sent && (!hid->led_sent_valid || hid->led_sent_value != desired)) {
                    hid->led_pending = 1;
                    hid->led_retry_delay = 1000;
                }
            }
        }

        uint8_t snap_len = 0;
        if (xhci_lock_timeout(&hid->report_lock, 200000)) {
            if (!hid->transfer_pending) transfer_queued = false;
            if (hid->report_qt != hid->report_qh) {
                snap_len = hid->report_data_len[hid->report_qt];
                if (snap_len > 64) snap_len = 64;
                memcpy(snap, hid->report_data[hid->report_qt], snap_len);
                hid->report_qt = (uint8_t)((hid->report_qt + 1) % HID_REPORT_QUEUE);
                if (hid->report_qt == hid->report_qh) hid->report_ready = 0;
            }
            xhci_unlock(&hid->report_lock);
        }

        if (snap_len) {
            transfer_queued = false;
            if (hid->protocol == 1) handle_keyboard_report(hid, snap, snap_len);
            else if (hid->protocol == 2) handle_mouse_report(hid, snap, snap_len);
        }

        if (!transfer_queued &&
            xhci_lock_timeout(&hc->ep_lock[hid->slot_id][hid->dci], 200000)) {
            uint32_t idx = hid->enqueue;
            if (idx >= COUNT_OF_TRB - 1) {
                hid->ring[idx].param   = hid->ring_phys;
                hid->ring[idx].status  = 0;
                hid->ring[idx].control = (TRB_LINK << 10) | (1u << 1) | hid->cycle;
                hid->enqueue = 0;
                hid->cycle ^= 1;
                idx = 0;
            }

            memset(hid->report, 0, hid->report_size);
            hid->report_len = 0;
            hid->ring[idx].param   = hid->report_phys;
            hid->ring[idx].status  = hid->report_size;
            hid->ring[idx].control = (TRB_NORMAL << 10) | (1u << 5) | hid->cycle;

            if (xhci_lock_timeout(&hid->report_lock, 200000)) {
                hid->pending_trb_phys = hid->ring_phys + (uint64_t)idx * sizeof(struct xhci_trb);
                hid->transfer_pending = 1;
                xhci_unlock(&hid->report_lock);

                hid->enqueue++;
                transfer_queued = true;

                __asm__ volatile("mfence" ::: "memory");
                hc->db_regs[hid->slot_id] = hid->dci;
            }

            xhci_unlock(&hc->ep_lock[hid->slot_id][hid->dci]);
        }
        schedule_yield_safe();
    }
}

static bool xhci_configure_hid_endpoint(xhci_controller_t *hc, struct usb_hid *hid) {
    uint32_t hccparams1 = xhci_read32(hc->cap_regs, 0x10);
    uint32_t ctx_size = (hccparams1 & (1u << 2)) ? 64 : 32;

    uint8_t dci = hid->dci;
    uint8_t max_dci = dci;

    uint64_t input_phys = (uint64_t)xhci_dma_alloc_pages(1);
    if (!input_phys) return -ENOMEM;
    uint8_t *input_virt = (uint8_t *)phys_to_virt(input_phys);
    memset(input_virt, 0, 4096);

    uint32_t *icc = (uint32_t *)input_virt;
    icc[1] = (1u << 0) | (1u << dci);

    uint32_t *slot_ctx = (uint32_t *)(input_virt + ctx_size);
    uint32_t *cur = (uint32_t *)phys_to_virt(((uint64_t *)hc->dcbaa)[hid->slot_id]);
    memcpy(slot_ctx, cur, ctx_size);
    slot_ctx[0] = (slot_ctx[0] & ~(0x1Fu << 27)) | ((uint32_t)max_dci << 27);

    uint8_t xhci_interval;
    if (hid->speed >= 3) {
        xhci_interval = hid->bInterval;
        if (xhci_interval < 1) xhci_interval = 1;
        if (xhci_interval > 15) xhci_interval = 15;
    } else {
        uint32_t bi = hid->bInterval ? hid->bInterval : 10;
        xhci_interval = 3;
        while (xhci_interval < 10 && (1u << (xhci_interval - 3)) < bi)
            xhci_interval++;
    }

    uint32_t *ep_ctx = (uint32_t *)(input_virt + ctx_size * (dci + 1));
    ep_ctx[0] = ((uint32_t)xhci_interval << 16);
    ep_ctx[1] = ((uint32_t)hid->mps << 16) | (7u << 3) | (3u << 1);
    *(uint64_t *)&ep_ctx[2] = hid->ring_phys | 1u;
    ep_ctx[4] = hid->mps;

    struct xhci_trb evt;
    if (xhci_submit_command(hc, input_phys, 0,
                            (TRB_CONFIGURE_ENDPOINT << 10) |
                            ((uint32_t)hid->slot_id << 24), &evt) != 0) {
        pmm_free_pages((void*)input_phys, 1);
        return false;
    }
    uint8_t code = (evt.status >> 24) & 0xFF;
    pmm_free_pages((void*)input_phys, 1);
    if (code != 1) return false;
    return true;
}

static void init_usb_hid(xhci_controller_t *hc, uint8_t slot_id, uint8_t speed,
                         uint8_t iface_num, uint8_t protocol,
                         uint8_t ep_in_addr, uint16_t mps, uint8_t interval,
                         uint16_t report_desc_len)
{
    struct usb_hid *hid = (struct usb_hid *)malloc_alloc(sizeof(struct usb_hid));
    if (!hid) return;
    memset(hid, 0, sizeof(struct usb_hid));

    hid->hc = hc;
    hid->slot_id = slot_id;
    hid->speed = speed;
    hid->iface_num = iface_num;
    hid->dci = (ep_in_addr & 0x0F) * 2 + 1;
    hid->ep_addr = ep_in_addr;
    hid->mps = mps;
    hid->bInterval = interval;
    hid->report_size = (mps > 0 && mps <= 64) ? mps : 8;

    if (protocol == 1) {
        hid->protocol = 1;
    } else if (protocol == 2) {
        hid->protocol = 2;
    } else {
        if (mps >= 8) hid->protocol = 1;
        else          hid->protocol = 2;
    }

    hid->ring_phys = (uint64_t)xhci_dma_alloc_pages(1);
    if (!hid->ring_phys) { malloc_free(hid); return; }
    hid->ring = (volatile struct xhci_trb *)phys_to_virt(hid->ring_phys);
    memset((void*)hid->ring, 0, 4096);
    hid->enqueue = 0; hid->cycle = 1;
    hid->ring[COUNT_OF_TRB-1].param   = hid->ring_phys;
    hid->ring[COUNT_OF_TRB-1].control = (TRB_LINK << 10) | (1u << 1) | 1u;

    hid->report_phys = (uint64_t)xhci_dma_alloc_pages(1);
    if (!hid->report_phys) {
        pmm_free_pages((void*)hid->ring_phys, 1);
        malloc_free(hid);
        return;
    }
    hid->report = (uint8_t *)phys_to_virt(hid->report_phys);
    memset(hid->report, 0, 4096);

    hid->halted = 0;
    hid->report_ready = 0;
    hid->report_len = 0;
    hid->report_qh = 0;
    hid->report_qt = 0;
    hid->transfer_pending = 0;
    hid->report_lock = 0;
    hid->disconnected = 0;
    hid->stopped = 0;
    hid->active_keycode_count = 0;
    memset(hid->active_keycodes, 0, sizeof(hid->active_keycodes));
    hid->led_probe_stage = 0;

    if (!xhci_configure_hid_endpoint(hc, hid)) {
        pmm_free_pages((void*)hid->ring_phys, 1);
        pmm_free_pages((void*)hid->report_phys, 1);
        malloc_free(hid);
        return;
    }

    if (report_desc_len > 0) {
        uint16_t fetch_len = report_desc_len > 4096 ? 4096 : report_desc_len;
        uint32_t desc_pages = (fetch_len + 4095) / 4096;
        uint64_t desc_phys = (uint64_t)xhci_dma_alloc_pages(desc_pages);
        if (desc_phys) {
            uint8_t *desc_buf = (uint8_t *)phys_to_virt(desc_phys);
            memset(desc_buf, 0, desc_pages * 4096);

            struct usb_setup_packet get_desc;
            memset(&get_desc, 0, sizeof(get_desc));
            get_desc.bmRequestType = 0x81;
            get_desc.bRequest = 0x06;
            get_desc.wValue = 0x2200;
            get_desc.wIndex = iface_num;
            get_desc.wLength = fetch_len;

            int dres = xhci_control_transfer(hc, slot_id,
                                             hc->ep0_ring[slot_id],
                                             &hc->ep0_enqueue[slot_id],
                                             &hc->ep0_cycle[slot_id],
                                             &get_desc, desc_phys, fetch_len);
            if (dres == 0)
                hid_parse_report_descriptor(hid, desc_buf, fetch_len);
            pmm_free_pages((void *)desc_phys, desc_pages);
        }
    }

    if (hid->protocol == 2 && hid->mouse_layout_valid) {
        uint32_t needed = (uint32_t)((hid->mouse_report_bits + 7) / 8) + (hid->mouse_report_id ? 1 : 0);
        if (needed > hid->report_size && needed <= 64)
            hid->report_size = (uint8_t)needed;
    }
    if (hid->protocol == 1 && hid->kbd_layout_valid) {
        uint32_t needed = (uint32_t)((hid->kbd_report_bits + 7) / 8) + (hid->kbd_report_id ? 1 : 0);
        if (needed > hid->report_size && needed <= 64)
            hid->report_size = (uint8_t)needed;
    }

    int desired_protocol = hid->protocol == 2 && hid->mouse_layout_valid ? 1 : 0;
    struct usb_setup_packet sp;
    memset(&sp, 0, sizeof(sp));
    sp.bmRequestType = 0x21;
    sp.bRequest = 0x0B;
    sp.wValue = (uint16_t)desired_protocol;
    sp.wIndex = iface_num;
    sp.wLength = 0;

    int pret = xhci_control_transfer(hc, slot_id,
                          hc->ep0_ring[slot_id],
                          &hc->ep0_enqueue[slot_id],
                          &hc->ep0_cycle[slot_id],
                          &sp, 0, 0);
    if (desired_protocol == 1) {
        hid->report_protocol_active = pret == 0;
        if (pret != 0) {
            sp.wValue = 0;
            int bret = xhci_control_transfer(hc, slot_id,
                                             hc->ep0_ring[slot_id],
                                             &hc->ep0_enqueue[slot_id],
                                             &hc->ep0_cycle[slot_id],
                                             &sp, 0, 0);
            hid->boot_protocol_active = bret == 0;
            hid->report_protocol_active = !hid->boot_protocol_active;
        }
    } else {
        hid->boot_protocol_active = pret == 0;
        hid->report_protocol_active = pret != 0;
    }

    if (hid->protocol == 1) {
        if (hid->boot_protocol_active) {
            hid->led_report_id = 0;
            hid->led_report_len = 1;
            hid->led_layout_known = true;
        } else if (hid->desc_led_layout_valid) {
            hid->led_report_id = hid->desc_led_report_id;
            hid->led_report_len = hid->desc_led_report_len;
            hid->led_layout_known = true;
        }
    }

    hc->hid_table[slot_id][hid->dci] = hid;

    if (hid->protocol == 1) {
        update_keyboard_leds();
    }

    char name[16];
    if (hid->protocol == 1) { name[0]='u';name[1]='s';name[2]='b';name[3]='-';name[4]='k';name[5]='b';name[6]='d';name[7]=0; }
    else                    { name[0]='u';name[1]='s';name[2]='b';name[3]='-';name[4]='m';name[5]='s';name[6]=0; }

    create_task_kernel_arg((void *)usb_hid_task, (uint64_t)hid, name);
    console_print_string("[HID] Registered protocol=");
    console_print_num(hid->protocol);
    console_print_string("\n");
}

void xhci_poll_all(void) {
    for (int i = 0; i < g_xhci_count; i++) {
        if (g_xhci_list[i]) {
            xhci_poll(g_xhci_list[i]);
        }
    }
}

void xhci_set_keyboard_leds(uint8_t leds) {
    for (int i = 0; i < g_xhci_count; i++) {
        xhci_controller_t *hc = g_xhci_list[i];
        if (!hc) continue;

        for (int slot = 1; slot < MAX_XHCI_SLOTS; slot++) {
            for (int dci = 0; dci < 32; dci++) {
                struct usb_hid *hid = hc->hid_table[slot][dci];
                if (hid && hid->protocol == 1) {
                    hid->led_value = leds;
                    hid->led_pending = 1;
                }
            }
        }
    }
}

/* Подстраховка на случай, если Port Status Change Event потерялся:
   например адаптер/мышь воткнули до окончания инициализации контроллера
   или CSC остался не подтверждённым. Раз в 200 мс сверяем состояние
   портов с таблицей слотов. */

static bool xhci_port_try(xhci_controller_t *hc, uint8_t port, uint64_t now) {
    if (port < 1 || port > 32) return false;
    if (now < hc->port_next_try[port]) return false;
    hc->port_next_try[port] = now + 1000;
    return true;
}

static void xhci_rescan_ports(xhci_controller_t *hc) {
    uint32_t hcsparams1 = xhci_read32(hc->cap_regs, 0x04);
    uint8_t max_ports = (hcsparams1 >> 24) & 0xFF;
    if (max_ports == 0 || max_ports > 32) max_ports = 32;
    volatile uint8_t *port_base = hc->op_regs + 0x400;
    uint64_t now = get_monotonic_ms();

    for (uint8_t port = 1; port <= max_ports; port++) {
        volatile uint8_t *reg = port_base + (port - 1) * 0x10;
        uint32_t portsc = xhci_read32(reg, 0x00);
        uint8_t slot = xhci_find_slot_by_port(hc, port);

        if (portsc & XHCI_PORTSC_CSC) {
            if (xhci_port_try(hc, port, now)) handle_port_connect(hc, port);
        } else if ((portsc & XHCI_PORTSC_CCS) && !slot) {
            if (xhci_port_try(hc, port, now)) handle_port_connect(hc, port);
        } else if (!(portsc & XHCI_PORTSC_CCS) && slot) {
            handle_port_disconnect(hc, port);
        }
    }
}

static void xhci_hotplug_task(void *arg) {
    xhci_controller_t *hc = (xhci_controller_t *)arg;
    uint64_t last_rescan = 0;
    while (!xhci_boot_done) schedule_yield_safe();
    xhci_enum_begin();
    listing_all_ports(hc);
    xhci_enum_end();

    for (;;) {
        if (hc->command_failed) {
            schedule_yield_safe();
            continue;
        }
        if (hc->port_change_head != hc->port_change_tail) {
            uint8_t port = hc->port_change_queue[hc->port_change_head];
            hc->port_change_head = (hc->port_change_head + 1) % 16;
            if (xhci_port_try(hc, port, get_monotonic_ms()))
                handle_port_connect(hc, port);
        }

        uint64_t now = get_monotonic_ms();
        if (now - last_rescan >= 200) {
            last_rescan = now;
            xhci_rescan_ports(hc);
        }
        schedule_yield_safe();
    }
}

static void xhci_pool_task(void *arg) {
    xhci_controller_t *hc = (xhci_controller_t *)arg;
    for (;;) {
        xhci_poll(hc);
        schedule_yield_safe();
    }
}

void parse_and_configure_device(xhci_controller_t *hc, uint8_t slot_id, uint8_t speed, uint16_t vid, uint16_t pid) {
    console_print_string("[USB] device VID=");
    console_print_hex(vid);
    console_print_string(" PID=");
    console_print_hex(pid);
    console_print_string("\n");

    uint64_t temp_phys = (uint64_t)xhci_dma_alloc_pages(1);
    if (!temp_phys) return;
    struct usb_configuration_descriptor *cfg = (struct usb_configuration_descriptor *)phys_to_virt(temp_phys);
    memset(cfg, 0, 4096);

    struct usb_setup_packet setup;
    setup.bmRequestType = 0x80;
    setup.bRequest = 0x06;
    setup.wValue = 0x0200;
    setup.wIndex = 0;
    setup.wLength = 9;

    if (xhci_control_transfer(hc, slot_id, hc->ep0_ring[slot_id], &hc->ep0_enqueue[slot_id], &hc->ep0_cycle[slot_id], &setup, temp_phys, setup.wLength) != 0) {
        pmm_free_pages((void*)temp_phys, 1);
        return;
    }

    uint16_t total_length = cfg->wTotalLength;
    uint8_t config_value = cfg->bConfigurationValue;
    if (total_length < 9 || total_length > 16384) {
        pmm_free_pages((void*)temp_phys, 1);
        return;
    }
    uint32_t full_pages = (total_length + 4095) / 4096;

    uint64_t full_phys = (uint64_t)xhci_dma_alloc_pages(full_pages);
    if (!full_phys) {
        pmm_free_pages((void*)temp_phys, 1);
        return;
    }
    uint8_t *buffer = (uint8_t *)phys_to_virt(full_phys);
    memset(buffer, 0, full_pages * 4096);

    setup.wLength = total_length;
    if (xhci_control_transfer(hc, slot_id, hc->ep0_ring[slot_id], &hc->ep0_enqueue[slot_id], &hc->ep0_cycle[slot_id], &setup, full_phys, setup.wLength) != 0) {
        pmm_free_pages((void*)temp_phys, 1);
        pmm_free_pages((void*)full_phys, full_pages);
        return;
    }

    struct usb_setup_packet set_cfg;
    set_cfg.bmRequestType = 0x00;
    set_cfg.bRequest = 0x09;
    set_cfg.wValue = config_value;
    set_cfg.wIndex = 0;
    set_cfg.wLength = 0;
    xhci_control_transfer(hc, slot_id, hc->ep0_ring[slot_id], &hc->ep0_enqueue[slot_id], &hc->ep0_cycle[slot_id], &set_cfg, 0, 0);

    uint8_t  cur_iface_class = 0, cur_iface_sub = 0, cur_iface_proto = 0, cur_iface_num = 0;

    // Переменные для MSD и HID
    uint8_t  is_msd = 0, is_hid = 0;
    uint8_t  msd_ep_out = 0, msd_ep_in = 0; uint16_t msd_mps = 512;
    uint8_t  hid_ep_in = 0; uint16_t hid_mps = 8; uint8_t hid_interval = 0;
    uint8_t  hid_proto = 0;
    uint16_t hid_report_desc_len = 0;
    bool msd_done = false;
    bool is_hub = false;

    // Переменные для USB NET / Wi-Fi
    uint8_t  is_net = 0;
    uint8_t  net_ep_out = 0, net_ep_in = 0, net_ep_int = 0;
    uint16_t net_mps = 512;
    uint8_t  net_ep_outs[4];
    uint8_t  net_ep_out_count = 0;

    /* Для Wi-Fi адаптеров не полагаемся на класс интерфейса: у RTL8192EU
       это Vendor Specific (0xFF), но драйвер известен по VID:PID.
       Собираем ВСЕ bulk-эндпоинты конфигурации (в т.ч. из alt setting'ов). */
    bool wifi_device = wifi_match_driver(vid, pid) != NULL;
    uint8_t  all_bulk_in = 0, all_bulk_out[4];
    uint8_t  all_bulk_out_count = 0;
    uint16_t all_bulk_mps = 512;

    uint32_t offset = 0;
    while (offset + 2 <= total_length) {
        uint8_t len  = buffer[offset];
        uint8_t type = buffer[offset + 1];
        if (len < 2 || offset + len > total_length) break;

        if (type == 0x04) {
            // Инициализируем предыдущие собранные MSD/HID
            if (is_msd && msd_ep_out && msd_ep_in && !msd_done) {
                init_usb_mass_storage(hc, slot_id, speed, msd_ep_out, msd_ep_in, msd_mps);
                msd_done = true;
                is_msd = 0; msd_ep_out = 0; msd_ep_in = 0;
            } else if (is_hid && hid_ep_in) {
                init_usb_hid(hc, slot_id, speed, cur_iface_num, hid_proto, hid_ep_in, hid_mps, hid_interval, hid_report_desc_len);
                is_hid = 0; hid_ep_in = 0; hid_report_desc_len = 0;
            } else {
                is_msd = 0; msd_ep_out = 0; msd_ep_in = 0;
                is_hid = 0; hid_ep_in = 0; hid_report_desc_len = 0;
            }

            struct usb_interface_descriptor *id = (struct usb_interface_descriptor *)&buffer[offset];
            cur_iface_class = id->bInterfaceClass;
            cur_iface_sub   = id->bInterfaceSubClass;
            cur_iface_proto = id->bInterfaceProtocol;
            cur_iface_num   = id->bInterfaceNumber;

            if (cur_iface_class == 0x08) is_msd = 1;
            if (cur_iface_class == 0x03) {
                is_hid = 1;
                hid_proto = cur_iface_proto;
            }
            if (cur_iface_class == 0x09) is_hub = true;
            // Детект сети/Wi-Fi: CDC-ECM (0x02 или 0x0A) ИЛИ Vendor-Specific
            if (cur_iface_class == 0x02 || cur_iface_class == 0x0A ||
                (cur_iface_class == 0xFF && wifi_device)) {
                is_net = 1;
            }

        } else if (type == 0x21 && is_hid && len >= 9) {
            hid_report_desc_len = (uint16_t)buffer[offset + 7] |
                                  ((uint16_t)buffer[offset + 8] << 8);
        } else if (type == 0x05 && len >= 7) {
            struct usb_endpoint_descriptor *ep = (struct usb_endpoint_descriptor *)&buffer[offset];
            uint8_t attr = ep->bmAttributes & 0x03;
            uint8_t addr = ep->bEndpointAddress;

            if (attr == 2) {
                /* Общий список bulk-эндпоинтов (для Wi-Fi). */
                if (wifi_device) {
                    if (addr & 0x80) {
                        if (!all_bulk_in) {
                            all_bulk_in = addr;
                            all_bulk_mps = ep->wMaxPacketSize;
                        }
                    } else {
                        bool seen = false;
                        for (int k = 0; k < all_bulk_out_count; k++)
                            if (all_bulk_out[k] == addr) { seen = true; break; }
                        if (!seen && all_bulk_out_count < 4)
                            all_bulk_out[all_bulk_out_count++] = addr;
                    }
                }
                if (is_msd) {
                    if (addr & 0x80) msd_ep_in  = addr;
                    else             msd_ep_out = addr;
                    msd_mps = ep->wMaxPacketSize;
                } else if (is_net) {
                    if (addr & 0x80) {
                        net_ep_in  = addr;
                        net_mps = ep->wMaxPacketSize;
                    } else {
                        net_ep_out = addr;
                        bool seen = false;
                        for (int k = 0; k < net_ep_out_count; k++)
                            if (net_ep_outs[k] == addr) { seen = true; break; }
                        if (!seen && net_ep_out_count < 4)
                            net_ep_outs[net_ep_out_count++] = addr;
                    }
                }
            } else if (attr == 3) {
                if (is_hid && (addr & 0x80)) {
                    hid_ep_in   = addr;
                    hid_mps     = ep->wMaxPacketSize;
                    hid_interval= ep->bInterval;
                } else if (is_net && (addr & 0x80)) {
                    net_ep_int = addr;
                }
            }
        }
        offset += len;
    }

    // Хвосты после цикла
    if (is_msd && msd_ep_out && msd_ep_in && !msd_done) {
        init_usb_mass_storage(hc, slot_id, speed, msd_ep_out, msd_ep_in, msd_mps);
        msd_done = true;
    } else if (is_hid && hid_ep_in) {
        init_usb_hid(hc, slot_id, speed, cur_iface_num, hid_proto, hid_ep_in, hid_mps, hid_interval, hid_report_desc_len);
    }

    if (is_hub) {
        console_print_string("[USB] HUB detected VID=");
        console_print_hex(vid);
        console_print_string(" PID=");
        console_print_hex(pid);
        console_print_string(": devices behind an external hub are NOT enumerated; "
                             "plug the adapter into a motherboard USB port\n");
    }

    /* Wi-Fi: только регистрация в подсистеме (FW потом). */
    if (wifi_device) {
        uint8_t  ep_in  = all_bulk_in  ? all_bulk_in  : net_ep_in;
        uint8_t *outs   = all_bulk_out_count ? all_bulk_out : net_ep_outs;
        uint8_t  outs_n = all_bulk_out_count ? all_bulk_out_count : net_ep_out_count;
        uint16_t mps    = all_bulk_in ? all_bulk_mps : net_mps;

        /* Если нашли эндпоинты только в vendor-интерфейсе — берём их. */
        if (outs_n == 0 && net_ep_out_count != 0) {
            outs   = net_ep_outs;
            outs_n = net_ep_out_count;
        }
        /* RTL8192EU: если дескриптор конфигурации не прочитался полностью,
           используем стандартную раскладку эндпоинтов. */
        if (!ep_in) ep_in = 0x81;
        if (outs_n == 0) {
            outs   = net_ep_outs;
            outs[0] = 0x02;
            outs[1] = 0x03;
            outs[2] = 0x04;
            outs_n  = 3;
        }

        console_print_string("[Wi-Fi] probing adapter, bulk IN=");
        console_print_hex(ep_in);
        console_print_string(" OUT=");
        console_print_num(outs_n);
        console_print_string("\n");

        wifi_probe_register_eps(hc, slot_id, speed, vid, pid,
                                ep_in, outs, outs_n,
                                net_ep_int, mps ? mps : 512);
    }
    // В противном случае обычный проводной USB-Ethernet
    else if (is_net && net_ep_out && net_ep_in) {
        init_usb_net(hc, slot_id, speed, vid, pid, net_ep_in, net_ep_out, net_ep_int, net_mps);
    }

    pmm_free_pages((void*)temp_phys, 1);
    pmm_free_pages((void*)full_phys, full_pages);
}

int xhci_evaluate_ep0_mps(xhci_controller_t *hc, uint8_t slot_id, uint16_t mps) {
    uint32_t hccparams1 = xhci_read32(hc->cap_regs, 0x10);
    uint32_t ctx_size = (hccparams1 & (1u << 2)) ? 64 : 32;

    uint64_t input_phys = (uint64_t)xhci_dma_alloc_pages(1);
    if (!input_phys) return -ENOMEM;
    uint8_t *input_virt = (uint8_t *)phys_to_virt(input_phys);
    memset(input_virt, 0, 4096);

    uint32_t *icc = (uint32_t *)input_virt;
    icc[0] = 0;
    icc[1] = (1u << 1);

    uint32_t *ep0_ctx = (uint32_t *)(input_virt + ctx_size * 2);
    ep0_ctx[1] = ((uint32_t)mps << 16);

    struct xhci_trb evt;
    if (xhci_submit_command(hc, input_phys, 0,
                            (TRB_EVALUATE_CONTEXT << 10) |
                            ((uint32_t)slot_id << 24), &evt) != 0) {
        pmm_free_pages((void*)input_phys, 1);
        return -1;
    }
    uint8_t code = (evt.status >> 24) & 0xFF;
    pmm_free_pages((void*)input_phys, 1);
    return (code == 1) ? 0 : -2;
}

/* Подтвердить изменение состояния порта (write-1-to-clear). */
static void xhci_ack_port_changes(volatile uint8_t *portsc_reg) {
    uint32_t portsc = xhci_read32(portsc_reg, 0x00);
    xhci_write32(portsc_reg, 0x00,
                 (portsc & XHCI_PORTSC_PRESERVE_MASK) | XHCI_PORTSC_CHANGE_MASK);
}

static bool xhci_reset_port(xhci_controller_t *hc, uint8_t port) {
    volatile uint8_t *reg = hc->op_regs + 0x400 + (port - 1) * 0x10;
    uint32_t status = xhci_read32(reg, 0);
    uint32_t state = (status >> 5) & 0xF;
    bool warm = hc->port_major[port] >= 3 && (state == 6 || state == 8);
    uint32_t reset = warm ? (1u << 31) : XHCI_PORTSC_PR;
    xhci_write32(reg, 0, (status & XHCI_PORTSC_PRESERVE_MASK) | reset);
    if (!xhci_wait_bits(reg, 0, reset, 0, 1000) ||
        !xhci_wait_bits(reg, 0, XHCI_PORTSC_PED | XHCI_PORTSC_CCS,
                        XHCI_PORTSC_PED | XHCI_PORTSC_CCS, 1000)) return false;
    xhci_ack_port_changes(reg);
    uint64_t start = hpet_read_counter();
    uint64_t ticks = hpet_ticks_for_ms(10);
    for (uint32_t i = 0; i < 100000; i++) {
        if (ticks && hpet_read_counter() - start >= ticks) break;
        __asm__ volatile("pause");
    }
    return true;
}

/* Выкидываем все события старого устройства из очередей слота:
   при повторном использовании номера слота они иначе попадут
   новому устройству (в том числе в control transfer'ы). */
static void xhci_drain_slot_events(xhci_controller_t *hc, uint8_t slot_id) {
    if (slot_id == 0 || slot_id > hc->max_slots || slot_id >= MAX_XHCI_SLOTS) return;
    for (int dci = 0; dci < 32; dci++)
        evtq_drain(&hc->transfer_evt_q[slot_id][dci]);
}

static uint8_t xhci_find_slot_by_port(xhci_controller_t *hc, uint8_t port) {
    for (int s = 1; s < MAX_XHCI_SLOTS; s++) {
        uint64_t dev_ctx_phys = ((uint64_t *)hc->dcbaa)[s];
        if (dev_ctx_phys == 0) continue;

        uint32_t *slot_ctx = (uint32_t *)phys_to_virt(dev_ctx_phys);
        uint8_t slot_port = (slot_ctx[1] >> 16) & 0xFF;
        if (slot_port == port) return (uint8_t)s;
    }
    return 0;
}

/* Паркуем все HID-задачи отключённого устройства и только потом
   освобождаем их память. Раньше usb_hid_task продолжал вечно работать
   с уже освобождённым hid, портить кольцо нового устройства и сбрасывать
   его endpoint'ы — из-за этого вторая мышь "не доходила" до nano-X. */
static void xhci_disconnect_hid_devices(xhci_controller_t *hc, uint8_t slot_id) {
    struct usb_hid *hids[32];
    int n = 0;
    bool keyboard_was_disconnected = false;

    for (int dci = 0; dci < 32; dci++) {
        struct usb_hid *hid = hc->hid_table[slot_id][dci];
        if (!hid) continue;

        /* Клавиатуру отключили, пока был зажат Shift: репорт «все клавиши
           отпущены» уже не придёт. Явно снимаем каждый ключ этого
           устройства, иначе модификатор останется включённым навсегда. */
        if (hid->protocol == 1) {
            for (uint32_t i = 0; i < hid->active_keycode_count; i++)
                inout_set_key_state(hid->active_keycodes[i], false);
            hid->active_keycode_count = 0;
            keyboard_was_disconnected = true;
        }

        hid->disconnected = 1;
        hc->hid_table[slot_id][dci] = NULL;
        hids[n++] = hid;
    }
    if (n == 0) return;

    /* Один ресинк на всё отключение, а не на каждый интерфейс. */
    if (keyboard_was_disconnected) inout_keyboard_request_resync();

    for (uint32_t wait = 0; wait < 300000; wait++) {
        bool stopped = true;
        for (int i = 0; i < n; i++) {
            if (!hids[i]->stopped) { stopped = false; break; }
        }
        if (stopped) break;
        if ((wait & 0x3F) == 0) schedule_yield_safe();
        __asm__ volatile("pause");
    }

    for (int i = 0; i < n; i++) {
        if (!hids[i]->stopped) continue;
        hids[i]->report = NULL;
        hids[i]->transfer_pending = 0;
        pmm_free_pages((void*)hids[i]->ring_phys, 1);
        pmm_free_pages((void*)hids[i]->report_phys, 1);
        malloc_free(hids[i]);
    }
}

static void xhci_destroy_slot(xhci_controller_t *hc, uint8_t slot_id) {
    /* Даём завершиться control transfer, который ещё мог идти к старому
       устройству (иначе ep0_lock останется захваченным навсегда). */
    for (uint32_t i = 0; i < 200000 && hc->ep0_lock[slot_id]; i++) {
        if ((i & 0x3F) == 0) schedule_yield_safe();
        __asm__ volatile("pause");
    }

    /* Чистим флэшки */
    for (int i = 0; i < MAX_BLOCK_DEVICES; i++) {
        struct block_device *bd = &block_devices[i];
        if (bd->type == DISK_TYPE_USB && bd->local_device_struct != NULL) {
            usb_msd_t *msd = (usb_msd_t *)bd->local_device_struct;
            if (msd->hc == hc && msd->slot_id == slot_id) {
                char dev_name[32];
                memset(dev_name, 0, sizeof(dev_name));
                for (uint32_t k = 0; k < sizeof(dev_name) - 1 && bd->name[k]; k++)
                    dev_name[k] = bd->name[k];
                partitions_device_remove(dev_name);
                unregister_device(dev_name);
                if (msd->bd) malloc_free(msd->bd);
                pmm_free_pages((void*)msd->cmd_page_phys, 1);
                pmm_free_pages((void*)msd->bulk_out_ring_phys, 1);
                pmm_free_pages((void*)msd->bulk_in_ring_phys, 1);
                malloc_free(msd);
                bd->local_device_struct = NULL;
                break;
            }
        }
    }

    xhci_disconnect_hid_devices(hc, slot_id);
    wifi_probe_unregister(hc, slot_id);
    usb_net_disconnect(hc, slot_id);

    xhci_drain_slot_events(hc, slot_id);

    struct xhci_trb evt;
    xhci_submit_command(hc, 0, 0,
                        (TRB_DISABLE_SLOT << 10) | ((uint32_t)slot_id << 24), &evt);

    /* Если control transfer всё ещё не отпустил ep0_lock, страницы rings
       пока трогать нельзя — иначе получим use-after-free. Лучше потерять
       страницу, чем уронить ядро. */
    bool ep0_busy = (hc->ep0_lock[slot_id] != 0);
    if (!ep0_busy) {
        /* Освобождаем именно ту страницу, что выделяли под ep0 ring.
           Раньше адрес брали из TR Dequeue Pointer в контексте устройства —
           он указывает в СЕРЕДИНУ кольца, и pmm освобождал не ту страницу. */
        if (hc->ep0_ring[slot_id]) {
            pmm_free_pages((void*)virt_to_phys((void*)hc->ep0_ring[slot_id]), 1);
            hc->ep0_ring[slot_id] = NULL;
        }
        hc->ep0_enqueue[slot_id] = 0;
        hc->ep0_cycle[slot_id] = 0;
    }

    uint64_t dev_ctx_phys = ((uint64_t *)hc->dcbaa)[slot_id];
    if (dev_ctx_phys) pmm_free_pages((void*)dev_ctx_phys, 1);
    ((uint64_t *)hc->dcbaa)[slot_id] = 0;

    if (!ep0_busy) hc->ep0_lock[slot_id] = 0;
    xhci_drain_slot_events(hc, slot_id);
}

void handle_port_disconnect(xhci_controller_t *hc, uint8_t port) {
    volatile uint8_t *port_base = hc->op_regs + 0x400;
    volatile uint8_t *portsc_reg = port_base + (port - 1) * 0x10;

    xhci_ack_port_changes(portsc_reg);

    for (int s = 1; s < MAX_XHCI_SLOTS; s++) {
        uint64_t dev_ctx_phys = ((uint64_t *)hc->dcbaa)[s];
        if (dev_ctx_phys == 0) continue;
        uint32_t *slot_ctx = (uint32_t *)phys_to_virt(dev_ctx_phys);
        uint8_t slot_port = (slot_ctx[1] >> 16) & 0xFF;
        if (slot_port == port) xhci_destroy_slot(hc, (uint8_t)s);
    }
}

static void usb_enum_fail(xhci_controller_t *hc, uint8_t port, const char *stage) {
    if (port < 1 || port > 32) return;
    uint32_t bit = 1u << (port - 1);
    if (hc->enum_fail_reported & bit) return;
    hc->enum_fail_reported |= bit;
    console_print_string("[USB] ");
    console_print_num(hc->bus);
    console_print_string(":");
    console_print_num(hc->slot);
    console_print_string(":");
    console_print_num(hc->func);
    console_print_string(" port ");
    console_print_num(port);
    console_print_string(" failed: ");
    console_print_string(stage);
    console_print_string("\n");
}

void handle_port_connect(xhci_controller_t *hc, uint8_t port) {
    if (hc->command_failed || port < 1 || port > 32) return;
    volatile uint8_t *port_base = hc->op_regs + 0x400;
    volatile uint8_t *portsc_reg = port_base + (port - 1) * 0x10;

    uint32_t portsc = xhci_read32(portsc_reg, 0x00);
    xhci_ack_port_changes(portsc_reg);

    if (!(portsc & XHCI_PORTSC_CCS)) {
        handle_port_disconnect(hc, port);
        return;
    }

    /* Порт уже проинициализирован: без нового CSC это дубликат события,
       повторно передёргивать устройство не нужно. */
    if (xhci_find_slot_by_port(hc, port) != 0) {
        if (!(portsc & XHCI_PORTSC_CSC)) return;
        /* Потерянное событие disconnect (или быстрая замена мыши):
           старый слот всё ещё висит на этом порту — убираем его. */
        handle_port_disconnect(hc, port);
    }

    if (!(portsc & XHCI_PORTSC_PED) && !xhci_reset_port(hc, port)) {
        console_print_string("[USB] port reset failed, PORTSC=");
        console_print_hex(xhci_read32(portsc_reg, 0));
        console_print_string("\n");
        usb_enum_fail(hc, port, "port reset/enable timeout");
        return;
    }

    portsc = xhci_read32(portsc_reg, 0x00);
    uint8_t speed = (portsc >> 10) & 0xF;
    if (!(portsc & XHCI_PORTSC_CCS)) {
        usb_enum_fail(hc, port, "device gone after port reset");
        return;
    }

    struct xhci_trb evt;
    if (xhci_submit_command(hc, 0, 0, (TRB_ENABLE_SLOT << 10) | ((uint32_t)hc->port_slot_type[port] << 16), &evt) != 0) {
        usb_enum_fail(hc, port, "enable slot command timed out");
        return;
    }

    uint8_t code = (evt.status >> 24) & 0xFF;
    uint8_t slot_id = (evt.control >> 24) & 0xFF;
    if (code != 1) {
        usb_enum_fail(hc, port, "enable slot rejected");
        return;
    }
    if (slot_id == 0 || slot_id > hc->max_slots || slot_id >= MAX_XHCI_SLOTS) {
        usb_enum_fail(hc, port, "invalid slot id");
        return;
    }

    /* Слот мог остаться после аварийного отключения — чистим все его очереди. */
    xhci_drain_slot_events(hc, slot_id);
    hc->ep0_lock[slot_id] = 0;

    uint32_t hccparams1 = xhci_read32(hc->cap_regs, 0x10);
    uint32_t ctx_size = (hccparams1 & (1u << 2)) ? 64 : 32;

    uint64_t dev_ctx_phys = (uint64_t)xhci_dma_alloc_pages(1);
    if (!dev_ctx_phys) { xhci_destroy_slot(hc, slot_id); return; }
    memset(phys_to_virt(dev_ctx_phys), 0, 4096);
    ((uint64_t *)hc->dcbaa)[slot_id] = dev_ctx_phys;

    uint64_t ep0_ring_phys = (uint64_t)xhci_dma_alloc_pages(1);
    if (!ep0_ring_phys) { xhci_destroy_slot(hc, slot_id); return; }
    volatile struct xhci_trb *ep0_ring = (volatile struct xhci_trb *)phys_to_virt(ep0_ring_phys);
    memset((void*)ep0_ring, 0, 4096);
    ep0_ring[COUNT_OF_TRB-1].param   = ep0_ring_phys;
    ep0_ring[COUNT_OF_TRB-1].control = (TRB_LINK << 10) | (1u << 1) | 1u;

    hc->ep0_ring[slot_id] = ep0_ring;
    hc->ep0_enqueue[slot_id] = 0;
    hc->ep0_cycle[slot_id] = 1;

    uint64_t input_ctx_phys = (uint64_t)xhci_dma_alloc_pages(1);
    if (!input_ctx_phys) { xhci_destroy_slot(hc, slot_id); return; }
    uint8_t *input_ctx_virt = (uint8_t *)phys_to_virt(input_ctx_phys);
    memset(input_ctx_virt, 0, 4096);

    uint32_t *icc = (uint32_t *)input_ctx_virt;
    uint32_t *slot_ctx = (uint32_t *)(input_ctx_virt + ctx_size);
    uint32_t *ep0_ctx  = (uint32_t *)(input_ctx_virt + ctx_size * 2);

    icc[1] = (1u << 0) | (1u << 1);
    slot_ctx[0] = (1u << 27) | ((uint32_t)(speed & 0xF) << 20);
    slot_ctx[1] = ((uint32_t)port << 16);

    uint16_t mps = (speed == 3) ? 64 : (speed >= 4 ? 512 : 8);

    ep0_ctx[1] = ((uint32_t)mps << 16) | (4u << 3) | (3u << 1);
    *(uint64_t *)&ep0_ctx[2] = ep0_ring_phys | 1u;
    ep0_ctx[4] = 8;

    if (xhci_submit_command(hc, input_ctx_phys, 0,
                            (TRB_ADDRESS_DEVICE << 10) |
                            ((uint32_t)slot_id << 24), &evt) != 0) {
        pmm_free_pages((void*)input_ctx_phys, 1);
        usb_enum_fail(hc, port, "address device command timed out");
        xhci_destroy_slot(hc, slot_id);
        return;
    }
    code = (evt.status >> 24) & 0xFF;
    pmm_free_pages((void*)input_ctx_phys, 1);
    if (code != 1) {
        console_print_string("[USB] Address Device completion code: ");
        console_print_num(code);
        console_print_string("\n");
        usb_enum_fail(hc, port, "address device rejected");
        xhci_destroy_slot(hc, slot_id);
        return;
    }

    uint64_t desc_phys = (uint64_t)xhci_dma_alloc_pages(1);
    if (!desc_phys) { xhci_destroy_slot(hc, slot_id); return; }
    struct usb_device_descriptor *desc = (struct usb_device_descriptor *)phys_to_virt(desc_phys);
    memset(desc, 0, 4096);

    struct usb_setup_packet setup;
    setup.bmRequestType = 0x80;
    setup.bRequest = 0x06;
    setup.wValue = 0x0100;
    setup.wIndex = 0;
    setup.wLength = 8;

    if (xhci_control_transfer(hc, slot_id, hc->ep0_ring[slot_id], &hc->ep0_enqueue[slot_id], &hc->ep0_cycle[slot_id], &setup, desc_phys, 8) != 0) {
        pmm_free_pages((void*)desc_phys, 1);
        usb_enum_fail(hc, port, "short device descriptor failed");
        xhci_destroy_slot(hc, slot_id);
        return;
    }


    uint16_t real_mps = desc->bMaxPacketSize0;
    if (speed >= 4) real_mps = desc->bMaxPacketSize0 == 9 ? 512 : 0;
    if (real_mps != 8 && real_mps != 16 && real_mps != 32 &&
        real_mps != 64 && real_mps != 512) {
        pmm_free_pages((void*)desc_phys, 1);
        xhci_destroy_slot(hc, slot_id);
        usb_enum_fail(hc, port, "invalid EP0 max packet size");
        return;
    }
    if (real_mps > 0 && real_mps != mps) {
        xhci_evaluate_ep0_mps(hc, slot_id, real_mps);
        delay_loop(2000000);
    }

    memset(desc, 0, 4096);
    setup.wLength = 18;
    if (xhci_control_transfer(hc, slot_id, hc->ep0_ring[slot_id], &hc->ep0_enqueue[slot_id], &hc->ep0_cycle[slot_id], &setup, desc_phys, 18) != 0) {
        pmm_free_pages((void*)desc_phys, 1);
        usb_enum_fail(hc, port, "full device descriptor failed");
        xhci_destroy_slot(hc, slot_id);
        return;
    }

    // Передаём их в парсер дескрипторов конфигурации
    parse_and_configure_device(hc, slot_id, speed, desc->idVendor, desc->idProduct);
    pmm_free_pages((void*)desc_phys, 1);
}

void listing_all_ports(xhci_controller_t *hc) {
    hc->enum_fail_reported = 0;
    uint32_t hcsparams1 = xhci_read32(hc->cap_regs, 0x04);
    uint8_t max_ports = (hcsparams1 >> 24) & 0xFF;
    if (max_ports == 0 || max_ports > 32) max_ports = 32;
    volatile uint8_t *port_base = hc->op_regs + 0x400;

    for (uint8_t port = 1; port <= max_ports; port++) {
        volatile uint8_t *portsc_reg = port_base + (port - 1) * 0x10;
        uint32_t portsc = xhci_read32(portsc_reg, 0x00);
        if (!(portsc & XHCI_PORTSC_PP)) {
            portsc &= ~(0x7F << 17);
            xhci_write32(portsc_reg, 0x00, portsc | XHCI_PORTSC_PP);
        }
    }
    console_print_string("[XHCI] root ports (");
    console_print_num(max_ports);
    console_print_string(")\n");

    console_print_string("[XHCI] scanning ports now; late connections handled by hotplug\n");

    uint32_t connected = 0;
    for (uint8_t port = 1; port <= max_ports; port++) {
        volatile uint8_t *portsc_reg = port_base + (port - 1) * 0x10;
        uint32_t portsc = xhci_read32(portsc_reg, 0x00);
        console_print_string("[XHCI] controller ");
        console_print_num(hc->bus);
        console_print_string(":");
        console_print_num(hc->slot);
        console_print_string(":");
        console_print_num(hc->func);
        console_print_string(" port ");
        console_print_num(port);
        console_print_string(" USB major=");
        console_print_num(hc->port_major[port]);
        console_print_string(" PORTSC=");
        console_print_hex(portsc);
        console_print_string("\n");
        if (portsc & XHCI_PORTSC_CCS) {
            connected++;
            console_print_string("[XHCI] port ");
            console_print_num(port);
            console_print_string(" connected\n");
            handle_port_connect(hc, port);
        }
    }
    console_print_string("[XHCI] devices connected on this controller: ");
    console_print_num(connected);
    console_print_string("\n");
    if (!connected)
        console_print_string("[XHCI] no ports report a device; check BIOS xHCI/EHCI hand-off settings\n");
}

void xhci_init(pci_device_t *dev) {
    console_print_string("[XHCI] probing controller ");
    console_print_num(dev->bus);
    console_print_string(":");
    console_print_num(dev->slot);
    console_print_string(":");
    console_print_num(dev->func);
    console_print_string("\n");

    xhci_controller_t *hc = (xhci_controller_t *)malloc_alloc(sizeof(xhci_controller_t));
    if (!hc) return;
    memset(hc, 0, sizeof(*hc));

    uint64_t bar0_phys = (dev->bars[0] & 0xFFFFFFF0);
    if ((dev->bars[0] & 0x6) == 0x4) bar0_phys |= ((uint64_t)dev->bars[1] << 32);
    if (!bar0_phys || (dev->bars[0] & 1)) {
        console_print_string("[XHCI] invalid BAR0, skipping controller\n");
        malloc_free(hc);
        return;
    }
    uint64_t bar0_virt = (uint64_t)phys_to_virt(bar0_phys);
    hc->cap_regs = (volatile uint8_t *)bar0_virt;

    hc->bus = dev->bus;
    hc->slot = dev->slot;
    hc->func = dev->func;

    uint32_t pci_cmd = pci_read_config(dev->bus, dev->slot, dev->func, 0x04) & 0xFFFF;
    pci_cmd |= (1u << 1) | (1u << 2) | (1u << 10);
    pci_write_config(dev->bus, dev->slot, dev->func, 0x04, pci_cmd);

    for (uint64_t off = 0; off < 0x100000; off += 0x1000)
        vmm_map_page(get_kernel_pml4(), bar0_phys + off, bar0_virt + off,
                     PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_PWT);

    uint8_t caplength = xhci_read8(hc->cap_regs, 0x00);
    uint16_t hciversion = *(volatile uint16_t *)(hc->cap_regs + 0x02);
    if (caplength < 0x20 || caplength > 0x80 || hciversion == 0 || hciversion == 0xFFFF) {
        console_print_string("[XHCI] invalid capability registers, skipping controller\n");
        malloc_free(hc);
        return;
    }
    hc->op_regs = hc->cap_regs + caplength;

    uint32_t rtsoff = xhci_read32(hc->cap_regs, 0x18) & ~0x1F;
    if (rtsoff > 0x100000 - 0x40) { malloc_free(hc); return; }
    hc->rt_regs = hc->cap_regs + rtsoff;

    uint32_t dboff = xhci_read32(hc->cap_regs, 0x14) & ~0x3;
    if (dboff > 0x100000 - 1024) { malloc_free(hc); return; }
    hc->db_regs = (volatile uint32_t *)(hc->cap_regs + dboff);

    uint32_t hccparams1 = xhci_read32(hc->cap_regs, 0x10);
    hc->dma64 = (hccparams1 & 1) != 0;
    console_print_string("[XHCI] version=");
    console_print_hex(hciversion);
    console_print_string(" PCI ID=");
    console_print_hex(pci_read_config(dev->bus, dev->slot, dev->func, 0));
    console_print_string(" DMA bits=");
    console_print_num(hc->dma64 ? 64 : 32);
    console_print_string(" context bytes=");
    console_print_num((hccparams1 & 4) ? 64 : 32);
    console_print_string("\n");
    if (!xhci_extended_caps(hc, hccparams1)) {
        console_print_string("[XHCI] malformed extended capabilities, skipping\n");
        malloc_free(hc);
        return;
    }
    xhci_intel_route_ports(dev);
    if (!wait_controller_ready(hc->op_regs)) {
        console_print_string("[XHCI] CNR timeout before reset\n");
        malloc_free(hc);
        return;
    }
    xhci_write32(hc->op_regs, 0, xhci_read32(hc->op_regs, 0) & ~5u);
    if (!wait_hchalt(hc->op_regs, true)) {
        console_print_string("[XHCI] halt timeout before reset\n");
        malloc_free(hc);
        return;
    }
    xhci_write32(hc->op_regs, 0, 1u << 1);
    if (!xhci_wait_bits(hc->op_regs, 0, 1u << 1, 0, 1000) ||
        !wait_controller_ready(hc->op_regs)) {
        console_print_string("[XHCI] reset/CNR timeout, skipping controller\n");
        malloc_free(hc);
        return;
    }
    if (!(xhci_read32(hc->op_regs, 0x08) & 1)) {
        console_print_string("[XHCI] controller does not support 4KiB pages\n");
        malloc_free(hc);
        return;
    }

    uint32_t hcsparams1 = xhci_read32(hc->cap_regs, 0x04);
    uint8_t max_slots = hcsparams1 & 0xFF;
    if (!max_slots) { malloc_free(hc); return; }
    if (max_slots >= MAX_XHCI_SLOTS) max_slots = MAX_XHCI_SLOTS - 1;
    hc->max_slots = max_slots;
    xhci_write32(hc->op_regs, 0x38, max_slots);

    uint64_t dcbaa_bytes = MAX_XHCI_SLOTS * sizeof(uint64_t);
    hc->dcbaa_phys = (uint64_t)xhci_dma_alloc_pages((dcbaa_bytes + 4095) / 4096);
    if (!hc->dcbaa_phys) { malloc_free(hc); return; }
    hc->dcbaa = (uint64_t*)phys_to_virt(hc->dcbaa_phys);
    memset(hc->dcbaa, 0, (dcbaa_bytes + 4095) / 4096 * 4096);

    uint32_t hcsparams2 = xhci_read32(hc->cap_regs, 0x08);
    uint32_t max_scratchpads = (((hcsparams2 >> 21) & 0x1F) << 5) | ((hcsparams2 >> 27) & 0x1F);
    if (max_scratchpads > 0) {
        uint64_t sp_pages = (max_scratchpads * sizeof(uint64_t) + 4095) / 4096;
        uint64_t sp_phys = (uint64_t)xhci_dma_alloc_pages(sp_pages);
        if (!sp_phys) return;
        uint64_t *sp_array = (uint64_t*)phys_to_virt(sp_phys);
        memset(sp_array, 0, sp_pages * 4096);
        for (uint32_t i = 0; i < max_scratchpads; i++) {
            void *sp_page = (void*)xhci_dma_alloc_pages(1);
            if (!sp_page) return;
            sp_array[i] = (uint64_t)sp_page;
            memset(phys_to_virt((uint64_t)sp_page), 0, 4096);
        }
        hc->dcbaa[0] = virt_to_phys(sp_array);
    }
    xhci_write64(hc->op_regs, 0x30, hc->dcbaa_phys);

    hc->cmd_ring_phys = (uint64_t)xhci_dma_alloc_pages(1);
    if (!hc->cmd_ring_phys) return;
    hc->cmd_ring = (struct xhci_trb*)phys_to_virt(hc->cmd_ring_phys);
    memset(hc->cmd_ring, 0, 4096);
    hc->cmd_enqueue = 0; hc->cmd_cycle = 1;
    hc->cmd_ring[COUNT_OF_TRB-1].param   = hc->cmd_ring_phys;
    hc->cmd_ring[COUNT_OF_TRB-1].control = (TRB_LINK << 10) | (1u << 1) | 1u;
    xhci_write64(hc->op_regs, 0x18, hc->cmd_ring_phys | 1u);

    hc->evt_ring_phys = (uint64_t)xhci_dma_alloc_pages(1);
    if (!hc->evt_ring_phys) return;
    hc->evt_ring = (struct xhci_trb*)phys_to_virt(hc->evt_ring_phys);
    memset(hc->evt_ring, 0, 4096);
    hc->evt_dequeue = 0; hc->evt_cycle = 1;

    hc->erst_phys = (uint64_t)xhci_dma_alloc_pages(1);
    if (!hc->erst_phys) return;
    hc->erst = (struct xhci_erst_entry*)phys_to_virt(hc->erst_phys);
    memset(hc->erst, 0, 4096);
    hc->erst[0].base = hc->evt_ring_phys;
    hc->erst[0].size = COUNT_OF_TRB;

    xhci_write32(hc->rt_regs, 0x20 + 0x08, 1);
    xhci_write64(hc->rt_regs, 0x20 + 0x10, hc->erst_phys);
    xhci_write64(hc->rt_regs, 0x20 + 0x18, hc->evt_ring_phys | (1u << 3));

    xhci_write32(hc->rt_regs, 0x20 + 0x00, 0);

    uint32_t usbcmd = xhci_read32(hc->op_regs, 0x00);
    usbcmd &= ~(1u << 2);
    usbcmd |= (1u << 0);
    xhci_write32(hc->op_regs, 0x00, usbcmd);
    if (!wait_hchalt(hc->op_regs, false)) {
        console_print_string("[XHCI] controller failed to start\n");
        return;
    }

    struct xhci_trb evt;
    if (xhci_submit_command(hc, 0, 0, TRB_CMD_NOOP << 10, &evt) != 0 ||
        ((evt.status >> 24) & 0xFF) != 1) {
        console_print_string("[XHCI] command ring self-test failed, enumeration disabled\n");
        return;
    }
    if (xhci_register_controller(hc) < 0) {
        xhci_write32(hc->op_regs, 0, 0);
        return;
    }

    create_task_kernel_arg((void *)xhci_hotplug_task, (uint64_t)hc, "xhci-hotplug");
    create_task_kernel_arg((void *)xhci_pool_task, (uint64_t)hc, "xhci-pool");

    console_print_string("[XHCI] Controller Initialized; device scan deferred to scheduler\n");
}