#include <stdint.h>
#include <stddef.h>
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
#include <scheduler/scheduler.h>

// Заголовки lwIP
#include "lwip/opt.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/tcpip.h"
#include "lwip/snmp.h"
#include "lwip/pbuf.h"
#include "lwip/dhcp.h"

// Экспортируем нужные функции из xhci.c
extern int xhci_configure_bulk_endpoints(xhci_controller_t *hc, usb_msd_t *msd, uint8_t ep_out_addr, uint8_t ep_in_addr, uint16_t mps);
extern int xhci_bulk_transfer(xhci_controller_t *hc, uint8_t slot_id, volatile struct xhci_trb *ring, uint32_t *enqueue, uint8_t *cycle, uint8_t db_target, uint64_t data_phys, uint32_t length, uint8_t dir_in);
extern int xhci_control_transfer(xhci_controller_t *hc, uint8_t slot_id, volatile struct xhci_trb *ep0_ring, uint32_t *ep0_enqueue, uint8_t *ep0_cycle, struct usb_setup_packet *setup, uint64_t data_buf_phys, uint16_t transfer_len);

#define USB_NET_TYPE_CDC     1
#define USB_NET_TYPE_ASIX    2
#define USB_NET_TYPE_REALTEK 3

typedef struct usb_net_device {
    xhci_controller_t *hc;
    uint8_t slot_id;
    uint8_t type;
    volatile bool active;

    uint8_t mac_addr[6];
    struct netif netif;

    // XHCI Bulk Rings для приёма/передачи
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

    struct usb_net_device *next;
} usb_net_device_t;

// Глобальный список всех воткнутых адаптеров
static usb_net_device_t *g_net_devices = NULL;

static inline uint64_t virt_to_phys_net(void *virt) {
    uint64_t v = (uint64_t)virt;
    uint64_t hhdm = get_bootinfo().hhdm->offset;
    if (v >= hhdm) return v - hhdm;
    return vmm_get_phys(get_kernel_pml4(), v & ~0xFFFULL) | (v & 0xFFFULL);
}

static inline void* phys_to_virt_net(uint64_t phys) {
    return (void*)(phys + get_bootinfo().hhdm->offset);
}

static inline bool interrupts_enabled_net(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0" : "=r"(flags));
    return (flags & (1ULL << 9)) != 0;
}

static inline void schedule_yield_safe_net(void) {
    if (interrupts_enabled_net()) schedule_yield();
}

// Функция передачи пакета (вызывается lwIP)
static err_t usb_net_tx(struct netif *netif, struct pbuf *p) {
    usb_net_device_t *dev = (usb_net_device_t *)netif->state;
    if (!dev || !dev->active) return ERR_CONN;

    // Выделяем 1 физическую страницу под отправляемый пакет (хватит для MTU 1500)
    uint64_t phys = (uint64_t)pmm_alloc_pages(1);
    void *virt = phys_to_virt_net(phys);

    // lwIP хранит данные в цепочке pbuf, собираем их в один плоский буфер
    pbuf_copy_partial(p, virt, p->tot_len, 0);

    // Отправляем пакет железяке
    int res = xhci_bulk_transfer(dev->hc, dev->slot_id,
                                 dev->bulk_out_ring, &dev->bulk_out_enqueue, &dev->bulk_out_cycle,
                                 dev->bulk_out_db, phys, p->tot_len, 0);

    pmm_free_pages((void*)phys, 1);
    return res == 0 ? ERR_OK : ERR_IF;
}

// Настройка интерфейса lwIP
static err_t usb_net_netif_init(struct netif *netif) {
    usb_net_device_t *dev = (usb_net_device_t *)netif->state;
    
    netif->name[0] = 'u';
    netif->name[1] = 'n'; // "un" = USB Network
    netif->output = etharp_output;
    netif->linkoutput = usb_net_tx;
    
    netif->hwaddr_len = 6;
    memcpy(netif->hwaddr, dev->mac_addr, 6);
    
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP | NETIF_FLAG_UP;
    
    return ERR_OK;
}

// Эвристика: угадываем реальную длину Ethernet-фрейма по IP-заголовкам
// (Т.к. стандартный CDC-ECM отдает сырой кадр, а XHCI скрывает размер при коротком пакете)
static uint32_t deduce_packet_len(uint8_t *buf, uint32_t max_len) {
    uint16_t eth_type = (buf[12] << 8) | buf[13];
    if (eth_type == 0x0800) { // IPv4
        uint16_t ip_len = (buf[16] << 8) | buf[17];
        return 14 + ip_len;
    } else if (eth_type == 0x0806) { // ARP
        return 14 + 28;
    } else if (eth_type == 0x86DD) { // IPv6
        uint16_t ip_len = (buf[18] << 8) | buf[19];
        return 14 + 40 + ip_len;
    }
    return max_len;
}

// Задача планировщика: поллинг входящих пакетов для КОНКРЕТНОГО устройства
static void usb_net_rx_task(void *arg) {
    usb_net_device_t *dev = (usb_net_device_t *)arg;

    while (dev->active) {
        uint64_t phys = (uint64_t)pmm_alloc_pages(1);
        uint8_t *virt = (uint8_t *)phys_to_virt_net(phys);

        // Ждем пакет размером до 2048 байт
        int res = xhci_bulk_transfer(dev->hc, dev->slot_id,
                                     dev->bulk_in_ring, &dev->bulk_in_enqueue, &dev->bulk_in_cycle,
                                     dev->bulk_in_db, phys, 2048, 1);

        if (res == 0 && dev->active) {
            uint32_t pkt_len = 0;
            uint8_t *payload = virt;

            // Обработка вендорских заголовков Realtek / ASIX (они пишут размер перед кадром)
            if (dev->type == USB_NET_TYPE_REALTEK) {
                pkt_len = virt[0] | ((virt[1] & 0x7F) << 8); // Младшие 15 бит RX Desc
                payload = virt + 4;
            } else if (dev->type == USB_NET_TYPE_ASIX) {
                pkt_len = virt[0] | (virt[1] << 8);
                payload = virt + 4;
            } else {
                // CDC ECM
                pkt_len = deduce_packet_len(virt, 2048);
            }

            if (pkt_len > 0 && pkt_len <= 1514) {
                struct pbuf *p = pbuf_alloc(PBUF_RAW, pkt_len, PBUF_POOL);
                if (p) {
                    pbuf_take(p, payload, pkt_len);
                    if (dev->netif.input(p, &dev->netif) != ERR_OK) {
                        pbuf_free(p);
                    }
                }
            }
        }

        pmm_free_pages((void*)phys, 1);

        // Уступаем CPU, чтобы не зависнуть
        schedule_yield_safe_net();
    }
}

// Основная инициализация сетевого свистка (вызывается из xhci.c)
int init_usb_net(xhci_controller_t *hc, uint8_t slot_id, uint8_t speed, uint16_t vid, uint16_t pid, 
                 uint8_t ep_in, uint8_t ep_out, uint8_t ep_int, uint16_t mps) 
{
    console_print_string("[USB-NET] Initializing adapter...\n");

    usb_net_device_t *dev = (usb_net_device_t *)malloc_alloc(sizeof(usb_net_device_t));
    memset(dev, 0, sizeof(*dev));

    dev->hc = hc;
    dev->slot_id = slot_id;
    dev->active = true;

    // Определяем тип адаптера
    if (vid == 0x0BDA) dev->type = USB_NET_TYPE_REALTEK;
    else if (vid == 0x0B95 || vid == 0x07AA) dev->type = USB_NET_TYPE_ASIX;
    else dev->type = USB_NET_TYPE_CDC;

    uint8_t dci_out = (ep_out & 0x0F) * 2 + 0;
    uint8_t dci_in  = (ep_in  & 0x0F) * 2 + 1;
    dev->bulk_out_db = dci_out;
    dev->bulk_in_db  = dci_in;

    // Кольцо OUT (Передача)
    dev->bulk_out_ring_phys = (uint64_t)pmm_alloc_pages(1);
    dev->bulk_out_ring = (volatile struct xhci_trb *)phys_to_virt_net(dev->bulk_out_ring_phys);
    memset((void*)dev->bulk_out_ring, 0, 4096);
    dev->bulk_out_enqueue = 0; dev->bulk_out_cycle = 1;
    dev->bulk_out_ring[COUNT_OF_TRB-1].param   = dev->bulk_out_ring_phys;
    dev->bulk_out_ring[COUNT_OF_TRB-1].control = (TRB_LINK << 10) | (1u << 1) | 1u;

    // Кольцо IN (Приём)
    dev->bulk_in_ring_phys = (uint64_t)pmm_alloc_pages(1);
    dev->bulk_in_ring = (volatile struct xhci_trb *)phys_to_virt_net(dev->bulk_in_ring_phys);
    memset((void*)dev->bulk_in_ring, 0, 4096);
    dev->bulk_in_enqueue = 0; dev->bulk_in_cycle = 1;
    dev->bulk_in_ring[COUNT_OF_TRB-1].param   = dev->bulk_in_ring_phys;
    dev->bulk_in_ring[COUNT_OF_TRB-1].control = (TRB_LINK << 10) | (1u << 1) | 1u;

    // ХИТРЫЙ ТРЮК: юзаем функцию из xhci.c, подкидывая ей dummy MSD структуру с нашими кольцами!
    usb_msd_t dummy_msd;
    memset(&dummy_msd, 0, sizeof(dummy_msd));
    dummy_msd.slot_id = slot_id;
    dummy_msd.bulk_out_ring_phys = dev->bulk_out_ring_phys;
    dummy_msd.bulk_in_ring_phys = dev->bulk_in_ring_phys;

    if (xhci_configure_bulk_endpoints(hc, &dummy_msd, ep_out, ep_in, mps) != 0) {
        console_print_string("[USB-NET] Failed to configure endpoints!\n");
        return -1;
    }

    // Буфер для Control Transfers
    uint64_t tmp_phys = (uint64_t)pmm_alloc_pages(1);
    void *tmp_virt = phys_to_virt_net(tmp_phys);

    // Достаем MAC-адрес и включаем питание RX/TX
    if (dev->type == USB_NET_TYPE_REALTEK) {
        // Чтение MAC-адреса Realtek
        struct usb_setup_packet sp = { .bmRequestType = 0xC0, .bRequest = 0x05, .wValue = 0x011E, .wIndex = 0x0100, .wLength = 6 };
        xhci_control_transfer(hc, slot_id, hc->ep0_ring[slot_id], &hc->ep0_enqueue[slot_id], &hc->ep0_cycle[slot_id], &sp, tmp_phys, 6);
        memcpy(dev->mac_addr, tmp_virt, 6);

        // Включаем RX/TX в Realtek
        sp.bmRequestType = 0x40; sp.bRequest = 0x05; sp.wValue = 0x0100; sp.wIndex = 0x0100; sp.wLength = 1;
        ((uint8_t*)tmp_virt)[0] = 0x0C; 
        xhci_control_transfer(hc, slot_id, hc->ep0_ring[slot_id], &hc->ep0_enqueue[slot_id], &hc->ep0_cycle[slot_id], &sp, tmp_phys, 1);
    } 
    else if (dev->type == USB_NET_TYPE_ASIX) {
        // Чтение MAC-адреса ASIX
        struct usb_setup_packet sp = { .bmRequestType = 0xC0, .bRequest = 0x13, .wValue = 0, .wIndex = 0, .wLength = 6 };
        xhci_control_transfer(hc, slot_id, hc->ep0_ring[slot_id], &hc->ep0_enqueue[slot_id], &hc->ep0_cycle[slot_id], &sp, tmp_phys, 6);
        memcpy(dev->mac_addr, tmp_virt, 6);

        // Включаем RX в ASIX (Broadcast + Multicast + Unicast)
        sp.bmRequestType = 0x40; sp.bRequest = 0x10; sp.wValue = 0x0088; sp.wIndex = 0; sp.wLength = 0;
        xhci_control_transfer(hc, slot_id, hc->ep0_ring[slot_id], &hc->ep0_enqueue[slot_id], &hc->ep0_cycle[slot_id], &sp, 0, 0);
    } 
    else {
        // CDC ECM (стандартный). Если сложно парсить String Descriptor, берем локальный сгенерённый MAC
        dev->mac_addr[0] = 0x02; dev->mac_addr[1] = 0x88; dev->mac_addr[2] = 0xEE;
        dev->mac_addr[3] = dev->slot_id; dev->mac_addr[4] = 0x11; dev->mac_addr[5] = 0x22;

        // Дергаем Alt Interface, чтобы завести CDC ECM
        struct usb_setup_packet sp = { .bmRequestType = 0x01, .bRequest = 0x0B, .wValue = 1, .wIndex = 1, .wLength = 0 };
        xhci_control_transfer(hc, slot_id, hc->ep0_ring[slot_id], &hc->ep0_enqueue[slot_id], &hc->ep0_cycle[slot_id], &sp, 0, 0);
        sp.wIndex = 2; // Пробуем оба индекса на удачу, если не парсили дескриптор
        xhci_control_transfer(hc, slot_id, hc->ep0_ring[slot_id], &hc->ep0_enqueue[slot_id], &hc->ep0_cycle[slot_id], &sp, 0, 0);
    }

    pmm_free_pages((void*)tmp_phys, 1);

    // Добавляем интерфейс в стек lwIP!
    ip4_addr_t ipaddr, netmask, gw;
    IP4_ADDR(&ipaddr, 10, 0, 2, dev->slot_id + 100); // Пример: 10.0.2.1xx
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gw, 10, 0, 2, 2);

    netif_add(&dev->netif, &ipaddr, &netmask, &gw, dev, usb_net_netif_init, tcpip_input);
    netif_set_default(&dev->netif);
    netif_set_up(&dev->netif);

    // Добавляем в глобальный список
    dev->next = g_net_devices;
    g_net_devices = dev;

    console_print_string("[USB-NET] Device added! MAC: ");
    for(int i=0; i<6; i++) { console_print_hex(dev->mac_addr[i]); console_print_string(" "); }
    console_print_string("\n");

    // Самое важное: Запускаем СВОЙ ЛИЧНЫЙ ПОТОК для этого устройства!
    char task_name[16];
    task_name[0]='n'; task_name[1]='e'; task_name[2]='t'; task_name[3]='-'; task_name[4]='r'; task_name[5]='x'; task_name[6]=0;
    create_task_kernel_arg((void *)usb_net_rx_task, (uint64_t)dev, task_name);

    dhcp_start(&dev->netif);
    console_print_string("[USB-NET] DHCP client started for ");
    console_print_string(dev->netif.name);
    console_print_string("\n");

    return 0;
}

// Хук выдергивания устройства из порта
void usb_net_disconnect(xhci_controller_t *hc, uint8_t slot_id) {
    usb_net_device_t **prev = &g_net_devices;
    usb_net_device_t *curr = g_net_devices;

    while (curr) {
        if (curr->hc == hc && curr->slot_id == slot_id) {
            // Выключаем флаг active — поток usb_net_rx_task сам выйдет из цикла и убьется
            curr->active = false;
            
            // Удаляем из lwIP
            netif_remove(&curr->netif);

            // Убираем из списка
            *prev = curr->next;
            
            // Память освободится чуть позже, когда таск умрет (здесь мы просто отпускаем страницы)
            pmm_free_pages((void*)curr->bulk_out_ring_phys, 1);
            pmm_free_pages((void*)curr->bulk_in_ring_phys, 1);
            
            console_print_string("[USB-NET] Device disconnected.\n");
            return;
        }
        prev = &curr->next;
        curr = curr->next;
    }
}