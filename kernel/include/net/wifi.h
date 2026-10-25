#ifndef WIFI_H
#define WIFI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pci/xhci.h>

#define WIFI_MAX_SSID_LEN  32
#define WIFI_MAX_DEVICES   8
#define WIFI_MAX_FW_PATH   128

typedef enum {
    WIFI_STATE_NONE = 0,
    WIFI_STATE_PROBED,       /* xHCI нашёл, в реестре, FW ещё нет */
    WIFI_STATE_FW_LOADED,
    WIFI_STATE_INIT,
    WIFI_STATE_DISCONNECTED,
    WIFI_STATE_SCANNING,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_ERROR
} wifi_state_t;

struct wifi_device;

/* Ops — заполняет КОНКРЕТНЫЙ драйвер (rtl8192eu и т.д.) при регистрации */
typedef struct {
    /* Поздний init: уже можно читать диск. fw_data может быть NULL если сам грузит */
    int  (*init)(struct wifi_device *dev, const uint8_t *fw, size_t fw_len);
    int  (*start)(struct wifi_device *dev);          /* RX task, radio on */
    int  (*stop)(struct wifi_device *dev);
    int  (*scan)(struct wifi_device *dev);
    int  (*connect)(struct wifi_device *dev, const char *ssid, const char *pass);
    int  (*disconnect)(struct wifi_device *dev);
    int  (*send_frame)(struct wifi_device *dev, const void *data, size_t len);
} wifi_ops_t;

/* Описание чипа, которое драйвер регистрирует в таблице match */
typedef struct {
    uint16_t vid;
    uint16_t pid;
    const char *name;
    const char *fw_path;     /* например "C:/firmware/rtl8192eufw.bin" */
    const wifi_ops_t *ops;
} wifi_driver_info_t;

typedef struct wifi_device {
    char name[16];
    uint16_t vid, pid;
    wifi_state_t state;

    /* USB binding (заполняет xHCI при probe) */
    xhci_controller_t *hc;
    uint8_t slot_id;
    uint8_t speed;
    uint8_t ep_in;
    uint8_t ep_out;
    uint8_t ep_int;
    uint16_t mps;

    uint8_t mac_addr[6];
    bool mac_valid;

    const wifi_ops_t *ops;
    char fw_path[WIFI_MAX_FW_PATH];
    void *priv;              /* rtl8192eu_t* и т.п. — создаёт драйвер в init */

    struct wifi_device *next;
} wifi_device_t;

/* --- Реестр драйверов (статические match-таблицы) --- */
void wifi_register_driver(const wifi_driver_info_t *info);
const wifi_driver_info_t *wifi_match_driver(uint16_t vid, uint16_t pid);

/* --- Реестр ЖЕЛЕЗА (то, что нашёл xHCI) --- */
/* Только регистрация, БЕЗ firmware. Можно звать из xHCI. */
int  wifi_probe_register(xhci_controller_t *hc, uint8_t slot_id, uint8_t speed,
                         uint16_t vid, uint16_t pid,
                         uint8_t ep_in, uint8_t ep_out, uint8_t ep_int, uint16_t mps);

void wifi_probe_unregister(xhci_controller_t *hc, uint8_t slot_id);

/* --- Поздний старт: после дисков/VFS --- */
void wifi_subsystem_init(void);     /* пустой/лог, реестр драйверов */
void wifi_boot_all(void);           /* синхронно: FW + init + start */
void wifi_start_boot_task(void);    /* create_task → wifi_boot_all */

/* RX путь драйвер → lwIP (когда поднимем netif) */
void wifi_rx_ethernet_frame(wifi_device_t *dev, const void *buf, size_t len);

uint8_t *firmware_load_file(const char *path, size_t *out_size);

#endif