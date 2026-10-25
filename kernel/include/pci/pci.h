#ifndef PCI_H
#define PCI_H

#include <stdint.h>

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC
#define PCI_CAP_ID_MSIX 0x11

struct pci_msix_cap {
    uint8_t  cap_id;
    uint8_t  next;
    uint16_t msg_ctrl;
    uint32_t table_offset_bir;
    uint32_t pba_offset_bir;
} __attribute__((packed));

typedef struct pci_device {
    uint8_t bus;
    uint8_t slot;
    uint8_t func;

    uint8_t revision;
    uint8_t prog_if;
    uint8_t subclass;
    uint8_t base_class;

    uint32_t bars[14];
} pci_device_t;

uint32_t pci_read_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_write_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t data);
void pci_init(void);

uint8_t pci_find_capability(uint8_t bus, uint8_t slot, uint8_t func, uint8_t cap_id);

#endif