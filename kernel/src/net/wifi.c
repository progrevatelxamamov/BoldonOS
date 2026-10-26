#include <net/wifi.h>
#include <fs/vfs.h>
#include <mm/malloc.h>
#include <mm/pmm.h>
#include <memory.h>
#include <gconsole.h>
#include <scheduler/scheduler.h>
#include <cpu/time.h>
#include <string.h>

#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/tcpip.h"
#include "lwip/dhcp.h"
#include "lwip/pbuf.h"

#define MAX_WIFI_DRIVERS 16

static const wifi_driver_info_t *g_drivers[MAX_WIFI_DRIVERS];
static int g_driver_count = 0;

static wifi_device_t *g_devices = NULL;
static volatile bool g_wifi_booted = false;

void wifi_register_driver(const wifi_driver_info_t *info) {
    if (!info || g_driver_count >= MAX_WIFI_DRIVERS) return;
    g_drivers[g_driver_count++] = info;
    console_print_string("[Wi-Fi] Driver registered: ");
    console_print_string(info->name ? info->name : "?");
    console_print_string("\n");
}

const wifi_driver_info_t *wifi_match_driver(uint16_t vid, uint16_t pid) {
    for (int i = 0; i < g_driver_count; i++) {
        if (g_drivers[i]->vid == vid && g_drivers[i]->pid == pid)
            return g_drivers[i];
    }
    return NULL;
}

void wifi_subsystem_init(void) {
    /* Драйверы сами зовут wifi_register_driver() из своих wifi_driver_init()
       или мы зовём rtl8192eu_driver_register() отсюда */
    extern void rtl8192eu_driver_register(void);
    rtl8192eu_driver_register();

    console_print_string("[Wi-Fi] Subsystem ready (deferred boot).\n");
}

/* ========== PROBE ONLY (из xHCI, диски могут быть ещё не готовы) ========== */

int wifi_probe_register(xhci_controller_t *hc, uint8_t slot_id, uint8_t speed,
                        uint16_t vid, uint16_t pid,
                        uint8_t ep_in, uint8_t ep_out, uint8_t ep_int, uint16_t mps)
{
    uint8_t outs[1];
    outs[0] = ep_out;
    return wifi_probe_register_eps(hc, slot_id, speed, vid, pid,
                                   ep_in, outs, 1, ep_int, mps);
}

int wifi_probe_register_eps(xhci_controller_t *hc, uint8_t slot_id, uint8_t speed,
                            uint16_t vid, uint16_t pid,
                            uint8_t ep_in, const uint8_t *ep_outs, uint8_t ep_out_count,
                            uint8_t ep_int, uint16_t mps)
{
    const wifi_driver_info_t *drv = wifi_match_driver(vid, pid);
    if (!drv) {
        console_print_string("[Wi-Fi] No driver for VID:PID\n");
        return -1;
    }

    /* Уже есть? */
    for (wifi_device_t *d = g_devices; d; d = d->next) {
        if (d->hc == hc && d->slot_id == slot_id)
            return 0;
    }

    wifi_device_t *dev = malloc_alloc(sizeof(wifi_device_t));
    if (!dev) return -1;
    memset(dev, 0, sizeof(*dev));

    strncpy(dev->name, "wlan0", 15);
    /* Если несколько — wlan1, wlan2... */
    int idx = 0;
    for (wifi_device_t *d = g_devices; d; d = d->next) idx++;
    if (idx > 0) {
        dev->name[4] = '0' + (idx % 10);
        dev->name[5] = 0;
    }

    dev->vid = vid;
    dev->pid = pid;
    dev->hc = hc;
    dev->slot_id = slot_id;
    dev->speed = speed;
    dev->ep_in = ep_in;
    dev->ep_out = ep_out_count ? ep_outs[0] : 0;
    dev->ep_out_count = ep_out_count;
    for (int i = 0; i < ep_out_count && i < 4; i++)
        dev->ep_out_q[i] = ep_outs[i];
    dev->ep_int = ep_int;
    dev->mps = mps;
    dev->ops = drv->ops;
    dev->state = WIFI_STATE_PROBED;
    if (drv->fw_path)
        strncpy(dev->fw_path, drv->fw_path, WIFI_MAX_FW_PATH - 1);

    dev->next = g_devices;
    g_devices = dev;

    console_print_string("[Wi-Fi] Probed ");
    console_print_string(drv->name);
    console_print_string(" -> ");
    console_print_string(dev->name);
    console_print_string(" (FW deferred)\n");
    return 0;
}

void wifi_probe_unregister(xhci_controller_t *hc, uint8_t slot_id) {
    wifi_device_t **pp = &g_devices;
    while (*pp) {
        wifi_device_t *d = *pp;
        if (d->hc == hc && d->slot_id == slot_id) {
            if (d->ops && d->ops->stop)
                d->ops->stop(d);
            *pp = d->next;
            if (d->priv) malloc_free(d->priv);
            malloc_free(d);
            console_print_string("[Wi-Fi] Device unregistered\n");
            return;
        }
        pp = &(*pp)->next;
    }
}

/* ========== FIRMWARE LOADER (только когда VFS жив) ========== */

uint8_t *firmware_load_file(const char *path, size_t *out_size) {
    if (out_size) *out_size = 0;
    if (!path || !path[0]) return NULL;

    console_print_string("[FW] Loading ");
    console_print_string(path);
    console_print_string("\n");

    vfs_file_t *f = vfs_open(path, VFS_O_READ);
    if (!f) {
        console_print_string("[FW] not found\n");
        return NULL;
    }

    vfs_stat_t st;
    if (vfs_stat(path, &st) < 0 || st.size == 0) {
        vfs_close(f);
        return NULL;
    }

    uint8_t *buf = malloc_alloc((size_t)st.size);
    if (!buf) {
        vfs_close(f);
        return NULL;
    }

    int n = vfs_read(f, buf, st.size);
    vfs_close(f);
    if (n <= 0 || (uint64_t)n != st.size) {
        malloc_free(buf);
        console_print_string("[FW] short read\n");
        return NULL;
    }

    if (out_size) *out_size = (size_t)st.size;
    console_print_string("[FW] OK, bytes=");
    console_print_num((uint64_t)st.size);
    console_print_string("\n");
    return buf;
}

/* ========== lwIP bridge (после start) ========== */

static err_t wifi_linkoutput(struct netif *netif, struct pbuf *p) {
    wifi_device_t *dev = (wifi_device_t *)netif->state;
    if (!dev || dev->state != WIFI_STATE_CONNECTED || !dev->ops || !dev->ops->send_frame)
        return ERR_CONN;

    uint8_t tmp[1514];
    if (p->tot_len > sizeof(tmp)) return ERR_BUF;
    pbuf_copy_partial(p, tmp, p->tot_len, 0);
    return dev->ops->send_frame(dev, tmp, p->tot_len) == 0 ? ERR_OK : ERR_IF;
}

static err_t wifi_netif_init_cb(struct netif *netif) {
    wifi_device_t *dev = (wifi_device_t *)netif->state;
    netif->name[0] = 'w';
    netif->name[1] = 'l';
    netif->output = etharp_output;
    netif->linkoutput = wifi_linkoutput;
    netif->hwaddr_len = 6;
    memcpy(netif->hwaddr, dev->mac_addr, 6);
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    return ERR_OK;
}

void wifi_rx_ethernet_frame(wifi_device_t *dev, const void *buf, size_t len) {
    if (!dev || !buf || len == 0 || len > 1514) return;
    /* netif добавим в start — пока заглушка если нет */
    (void)dev; (void)buf; (void)len;
}

/* ========== BOOT ALL (после дисков) ========== */

static void wifi_msleep(uint32_t ms) {
    uint64_t end = get_monotonic_ms() + ms;
    while (get_monotonic_ms() < end)
        schedule_yield();
}

void wifi_boot_all(void) {
    if (g_wifi_booted) return;

    console_print_string("[Wi-Fi] Boot: loading modules & firmware...\n");

    for (wifi_device_t *dev = g_devices; dev; dev = dev->next) {
        if (dev->state != WIFI_STATE_PROBED) continue;
        if (!dev->ops || !dev->ops->init) {
            dev->state = WIFI_STATE_ERROR;
            continue;
        }

        size_t fw_len = 0;
        uint8_t *fw = NULL;
        if (dev->fw_path[0])
            fw = firmware_load_file(dev->fw_path, &fw_len);

        if (dev->fw_path[0] && !fw)
            continue;

        int r = dev->ops->init(dev, fw, fw_len);
        if (fw) malloc_free(fw);

        if (r != 0) {
            console_print_string("[Wi-Fi] init failed: ");
            console_print_string(dev->name);
            console_print_string("\n");
            dev->state = WIFI_STATE_ERROR;
            continue;
        }
        dev->state = WIFI_STATE_INIT;

        if (dev->ops->start) {
            r = dev->ops->start(dev);
            if (r != 0) {
                dev->state = WIFI_STATE_ERROR;
                continue;
            }
        }

        dev->state = WIFI_STATE_DISCONNECTED;
        console_print_string("[Wi-Fi] ");
        console_print_string(dev->name);
        console_print_string(" ready (disconnected)\n");
    }

    console_print_string("[Wi-Fi] Boot done.\n");
}

static void wifi_boot_task(void *arg) {
    (void)arg;
    for (int attempt = 0; attempt < 120; attempt++) {
        wifi_boot_all();
        bool pending = false;
        for (wifi_device_t *dev = g_devices; dev; dev = dev->next) {
            if (dev->state == WIFI_STATE_PROBED) pending = true;
        }
        if (!pending) break;
        wifi_msleep(500);
    }
    for (;;) schedule_yield();
}

void wifi_start_boot_task(void) {
    if (g_wifi_booted) return;
    g_wifi_booted = true;
    create_task_kernel_arg((void *)wifi_boot_task, 0, "wifi-boot");
}