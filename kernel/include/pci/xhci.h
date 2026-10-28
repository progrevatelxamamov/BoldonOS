#ifndef XHCI_H
#define XHCI_H

#include <stdint.h>
#include <pci/pci.h>
#include <inout/inout.h>
#include <usb_data.h>

#define COUNT_OF_TRB 64
#define MAX_XHCI_SLOTS 64
#define MAX_XHCI_CONTROLLERS 8
#define EVT_QUEUE_SIZE 64
#define HID_REPORT_QUEUE 8

struct xhci_controller;
struct usb_hid;

struct xhci_trb {
    uint64_t param;
    uint32_t status;
    uint32_t control;
} __attribute__((packed));

struct xhci_erst_entry {
    uint64_t base;
    uint32_t size;
    uint32_t reserved;
} __attribute__((packed));

struct evt_queue {
    struct xhci_trb  buf[EVT_QUEUE_SIZE];
    volatile uint32_t head;
    volatile uint32_t tail;
};

typedef struct {
    volatile uint8_t *cap_regs;
    volatile uint8_t *op_regs;
    volatile uint8_t *rt_regs;
    volatile uint32_t *db_regs;
    struct xhci_trb *cmd_ring;
    uint64_t cmd_ring_phys;
    uint32_t cmd_enqueue;
    uint8_t  cmd_cycle;
    struct xhci_trb *evt_ring;
    uint64_t evt_ring_phys;
    struct xhci_erst_entry *erst;
    uint64_t erst_phys;
    uint32_t evt_dequeue;
    uint8_t  evt_cycle;
    uint64_t *dcbaa;
    uint64_t dcbaa_phys;
    volatile uint32_t evt_lock;
    volatile uint32_t cmd_lock;
    /* Сериализует всю команду (постановку в кольцо + ожидание события).
       Без этого несколько задач берут чужие command completion events. */
    volatile uint32_t cmd_exec_lock;
    volatile uint32_t ep_lock[MAX_XHCI_SLOTS][32];
    volatile uint32_t ep0_lock[MAX_XHCI_SLOTS];
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    struct evt_queue  cmd_evt_q;
    struct evt_queue  transfer_evt_q[MAX_XHCI_SLOTS][32];
    volatile uint8_t port_change_queue[16];
    uint64_t port_next_try[33];
    uint32_t enum_fail_reported;
    volatile uint8_t port_change_head;
    volatile uint8_t port_change_tail;
    struct usb_hid *hid_table[MAX_XHCI_SLOTS][32];
    volatile struct xhci_trb *ep0_ring[MAX_XHCI_SLOTS];
    uint32_t ep0_enqueue[MAX_XHCI_SLOTS];
    uint8_t  ep0_cycle[MAX_XHCI_SLOTS];
} xhci_controller_t;

struct usb_setup_packet {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed));

struct usb_device_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} __attribute__((packed));

struct usb_configuration_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} __attribute__((packed));

struct usb_interface_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} __attribute__((packed));

struct usb_endpoint_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} __attribute__((packed));

#define BOT_CBW_SIGNATURE  0x43425355
#define BOT_CSW_SIGNATURE  0x53425355

struct bot_cbw {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_length;
    uint8_t  flags;
    uint8_t  lun;
    uint8_t  cdb_len;
    uint8_t  cdb[16];
} __attribute__((packed));

struct bot_csw {
    uint32_t signature;
    uint32_t tag;
    uint32_t residue;
    uint8_t  status;
} __attribute__((packed));

typedef struct {
    xhci_controller_t *hc;
    uint32_t bot_tag;
    uint8_t  slot_id;
    uint8_t  speed;
    uint64_t cmd_page_phys;
    void    *cmd_page_virt;
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
    uint32_t block_count;
    uint32_t block_size;
    struct block_device *bd;
} usb_msd_t;

struct hid_field {
    uint16_t bit_offset;
    uint16_t bit_size;
    int32_t  logical_min;
    int32_t  logical_max;
    bool     present;
    bool     relative;
};

struct usb_hid {
    xhci_controller_t *hc;
    uint8_t  slot_id;
    uint8_t  speed;
    uint8_t  iface_num;
    uint8_t  protocol;
    uint8_t  dci;
    uint16_t mps;
    uint8_t  bInterval;
    volatile struct xhci_trb *ring;
    uint64_t ring_phys;
    uint32_t enqueue;
    uint8_t  cycle;
    uint64_t report_phys;
    uint8_t *report;
    uint8_t  report_size;
    volatile uint16_t report_len;
    uint8_t  prev_keys[16];
    uint8_t  prev_key_count;
    uint8_t  prev_modifiers;
    /* Авторитетный набор клавиш ЭТОГО устройства (внутренние keycodes).
       Сверяется на каждом репорте, а не только по фронтам: потерянное
       событие отпускания больше не оставляет Shift «залипшим». */
    uint8_t  active_keycodes[32];
    uint8_t  active_keycode_count;
    uint8_t  prev_buttons;
    volatile uint8_t pending;
    volatile uint8_t halted;
    volatile uint8_t report_ready;
    uint8_t  ep_addr;
    volatile uint32_t report_lock;
    uint64_t pending_trb_phys;
    volatile uint8_t transfer_pending;
    volatile uint8_t report_qh;
    volatile uint8_t report_qt;
    uint8_t  report_data[HID_REPORT_QUEUE][64];
    uint8_t  report_data_len[HID_REPORT_QUEUE];
    uint32_t halt_recovery_ms;
    /* Hotplug: устройство отключено, задача должна освободить ресурсы и уснуть. */
    volatile uint8_t disconnected;
    volatile uint8_t stopped;
    bool     boot_protocol_active;
    bool     report_protocol_active;
    bool     has_input_report_id;
    uint8_t  input_report_id;
    struct hid_field mouse_buttons;
    uint8_t  mouse_button_count;
    struct hid_field mouse_x;
    struct hid_field mouse_y;
    struct hid_field mouse_wheel;
    uint8_t  mouse_report_id;
    uint16_t mouse_report_bits;
    bool     mouse_layout_valid;
    struct hid_field kbd_modifiers;
    struct hid_field kbd_keys;
    uint8_t  kbd_key_count;
    uint8_t  kbd_report_id;
    uint16_t kbd_report_bits;
    bool     kbd_layout_valid;
    struct hid_field led_num;
    struct hid_field led_caps;
    struct hid_field led_scroll;
    uint8_t  desc_led_report_id;
    uint16_t desc_led_report_len;
    bool     desc_led_layout_valid;
    uint8_t  led_report_id;
    uint16_t led_report_len;
    bool     led_layout_known;
    uint8_t  led_probe_stage;
    volatile uint8_t led_pending;
    volatile uint8_t led_value;
    uint8_t  led_sent_value;
    bool     led_sent_valid;
    uint32_t led_retry_delay;
    uint8_t  led_failure_count;
};

#define TRB_NORMAL               1
#define TRB_SETUP_STAGE          2
#define TRB_DATA_STAGE           3
#define TRB_STATUS_STAGE         4
#define TRB_LINK                 6
#define TRB_CONFIGURE_ENDPOINT   12
#define TRB_EVALUATE_CONTEXT     13
#define TRB_RESET_ENDPOINT       14
#define TRB_SET_TR_DEQUEUE       16
#define TRB_CMD_NOOP             23
#define TRB_ENABLE_SLOT          9
#define TRB_DISABLE_SLOT         10
#define TRB_ADDRESS_DEVICE       11
#define TRB_TRANSFER_EVENT       32
#define TRB_COMMAND_COMPLETION   33
#define TRB_PORT_STATUS_CHANGE   34

#define XHCI_PORTSC_CCS        (1u << 0)
#define XHCI_PORTSC_PED        (1u << 1)
#define XHCI_PORTSC_PR         (1u << 4)
#define XHCI_PORTSC_PP         (1u << 9)
#define XHCI_PORTSC_CSC        (1u << 17)
#define XHCI_PORTSC_PEC        (1u << 18)
#define XHCI_PORTSC_OCC        (1u << 20)
#define XHCI_PORTSC_PRC        (1u << 21)
#define XHCI_PORTSC_PRESERVE_MASK (XHCI_PORTSC_PP)
/* Write-1-to-clear биты PORTSC. Их обязательно сбрасывать и на
   отключении, иначе CSC остаётся в 1 и контроллер не пришлёт новое
   Port Status Change Event при следующем подключении. */
#define XHCI_PORTSC_CHANGE_MASK (XHCI_PORTSC_CSC | XHCI_PORTSC_PEC | \
                                 XHCI_PORTSC_OCC | XHCI_PORTSC_PRC)

static const char *usb_get_vendor_name(uint16_t vid) {
    int low = 0;
    int high = (int)usb_vendor_count - 1;
    while (low <= high) {
        int mid = (low + high) / 2;
        uint16_t mid_vid = usb_vendor_db[mid].vid;
        if (mid_vid == vid) return &usb_string_pool[usb_vendor_db[mid].offset];
        if (mid_vid < vid) low = mid + 1;
        else high = mid - 1;
    }
    return "Unknown Vendor";
}

static const char *usb_get_product_name(uint16_t vid, uint16_t pid) {
    uint32_t target_vid_pid = ((uint32_t)vid << 16) | pid;
    int low = 0;
    int high = (int)usb_product_count - 1;
    while (low <= high) {
        int mid = (low + high) / 2;
        uint32_t mid_vid_pid = usb_product_db[mid].vid_pid;
        if (mid_vid_pid == target_vid_pid) return &usb_string_pool[usb_product_db[mid].offset];
        if (mid_vid_pid < target_vid_pid) low = mid + 1;
        else high = mid - 1;
    }
    return "Generic USB Device";
}

static uint16_t hid_usage_to_keycode(uint8_t usage) {
    switch (usage) {
        case 0x04: return KEY_A;
        case 0x05: return KEY_B;
        case 0x06: return KEY_C;
        case 0x07: return KEY_D;
        case 0x08: return KEY_E;
        case 0x09: return KEY_F;
        case 0x0A: return KEY_G;
        case 0x0B: return KEY_H;
        case 0x0C: return KEY_I;
        case 0x0D: return KEY_J;
        case 0x0E: return KEY_K;
        case 0x0F: return KEY_L;
        case 0x10: return KEY_M;
        case 0x11: return KEY_N;
        case 0x12: return KEY_O;
        case 0x13: return KEY_P;
        case 0x14: return KEY_Q;
        case 0x15: return KEY_R;
        case 0x16: return KEY_S;
        case 0x17: return KEY_T;
        case 0x18: return KEY_U;
        case 0x19: return KEY_V;
        case 0x1A: return KEY_W;
        case 0x1B: return KEY_X;
        case 0x1C: return KEY_Y;
        case 0x1D: return KEY_Z;
        case 0x1E: return KEY_1;
        case 0x1F: return KEY_2;
        case 0x20: return KEY_3;
        case 0x21: return KEY_4;
        case 0x22: return KEY_5;
        case 0x23: return KEY_6;
        case 0x24: return KEY_7;
        case 0x25: return KEY_8;
        case 0x26: return KEY_9;
        case 0x27: return KEY_0;
        case 0x28: return KEY_ENTER;
        case 0x29: return KEY_ESC;
        case 0x2A: return KEY_BACKSPACE;
        case 0x2B: return KEY_TAB;
        case 0x2C: return KEY_SPACE;
        case 0x2D: return KEY_MINUS;
        case 0x2E: return KEY_EQUALS;
        case 0x2F: return KEY_LBRACKET;
        case 0x30: return KEY_RBRACKET;
        case 0x31: return KEY_BACKSLASH;
        case 0x33: return KEY_SEMICOLON;
        case 0x34: return KEY_APOSTROPHE;
        case 0x35: return KEY_GRAVE;
        case 0x36: return KEY_COMMA;
        case 0x37: return KEY_DOT;
        case 0x38: return KEY_SLASH;
        case 0x39: return KEY_CAPSLOCK;
        case 0x3A: return KEY_F1;
        case 0x3B: return KEY_F2;
        case 0x3C: return KEY_F3;
        case 0x3D: return KEY_F4;
        case 0x3E: return KEY_F5;
        case 0x3F: return KEY_F6;
        case 0x40: return KEY_F7;
        case 0x41: return KEY_F8;
        case 0x42: return KEY_F9;
        case 0x43: return KEY_F10;
        case 0x44: return KEY_F11;
        case 0x45: return KEY_F12;
        case 0x49: return KEY_INSERT;
        case 0x4A: return KEY_HOME;
        case 0x4B: return KEY_PGUP;
        case 0x4C: return KEY_DELETE;
        case 0x4D: return KEY_END;
        case 0x4E: return KEY_PGDN;
        case 0x4F: return KEY_RIGHT;
        case 0x50: return KEY_LEFT;
        case 0x51: return KEY_DOWN;
        case 0x52: return KEY_UP;
        case 0x53: return KEY_NUMLOCK;
        case 0x54: return KEY_KP_SLASH;
        case 0x55: return KEY_KP_STAR;
        case 0x56: return KEY_KP_MINUS;
        case 0x57: return KEY_KP_PLUS;
        case 0x58: return KEY_KP_ENTER;
        case 0x59: return KEY_KP_1;
        case 0x5A: return KEY_KP_2;
        case 0x5B: return KEY_KP_3;
        case 0x5C: return KEY_KP_4;
        case 0x5D: return KEY_KP_5;
        case 0x5E: return KEY_KP_6;
        case 0x5F: return KEY_KP_7;
        case 0x60: return KEY_KP_8;
        case 0x61: return KEY_KP_9;
        case 0x62: return KEY_KP_0;
        case 0x63: return KEY_KP_DOT;
        default: return 0;
    }
}

void xhci_init(pci_device_t *dev);
void xhci_set_keyboard_leds(uint8_t leds);
void xhci_poll_all(void);

int init_usb_net(xhci_controller_t *hc, uint8_t slot_id, uint8_t speed, uint16_t vid, uint16_t pid, 
                 uint8_t ep_in, uint8_t ep_out, uint8_t ep_int, uint16_t mps);
void usb_net_disconnect(xhci_controller_t *hc, uint8_t slot_id);

#endif