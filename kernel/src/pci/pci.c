#include <stdint.h>
#include <stddef.h>
#include <io.h>
#include <pci/pci.h>
#include <pci/nvme.h>
#include <pci/xhci.h>
#include <mm/malloc.h>

uint64_t pci_count = 0;
pci_device_t *devices[1024];

uint32_t pci_read_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address;
    uint32_t lbus  = (uint32_t)bus;
    uint32_t lslot = (uint32_t)slot;
    uint32_t lfunc = (uint32_t)func;

    address = (uint32_t)((lbus << 16) | (lslot << 11) |
              (lfunc << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));

    outl(PCI_CONFIG_ADDRESS, address);

    return inl(PCI_CONFIG_DATA);
}

void pci_write_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t data) {
    uint32_t address;
    uint32_t lbus  = (uint32_t)bus;
    uint32_t lslot = (uint32_t)slot;
    uint32_t lfunc = (uint32_t)func;
    
    address = (uint32_t)((lbus << 16) | (lslot << 11) |
              (lfunc << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));

    outl(PCI_CONFIG_ADDRESS, address);

    outl(PCI_CONFIG_DATA, data);
}

uint8_t pci_get_base_class(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t reg = pci_read_config(bus, slot, func, 0x08);
    return (reg >> 24) & 0xFF;
}

uint8_t pci_get_sub_class(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t reg = pci_read_config(bus, slot, func, 0x08);
    return (reg >> 16) & 0xFF;
}

uint8_t pci_get_prog_if(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t reg = pci_read_config(bus, slot, func, 0x08);
    return (reg >> 8) & 0xFF;
}

uint8_t pci_get_revision(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t reg = pci_read_config(bus, slot, func, 0x08);
    return reg & 0xFF;
}

uint8_t pci_get_secondary_bus(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t reg = pci_read_config(bus, slot, func, 0x18);
    return (reg >> 8) & 0xFF;  // байт по смещению 0x19
}

void scan_bus(uint8_t bus) {
    for (uint16_t device = 0; device < 32; device++) {
        uint32_t id = pci_read_config(bus, device, 0, 0);
        if ((id & 0xFFFF) == 0xFFFF) continue;

        uint32_t header_reg = pci_read_config(bus, device, 0, 0x0C);
        uint8_t header_type = (header_reg >> 16) & 0xFF;
        uint16_t functions = 1;
        if (header_type & 0x80) {
            functions = 8;
        }

        for (uint64_t func = 0; func < functions; func++) {
            uint32_t id = pci_read_config(bus, device, func, 0);
            if ((id & 0xFFFF) == 0xFFFF) continue;

            uint8_t base = pci_get_base_class(bus, device, func);
            uint8_t sub = pci_get_sub_class(bus, device, func);

            if (base == 0x06 && sub == 0x04) {
                uint8_t secondary = pci_get_secondary_bus(bus, device, func);
                if (secondary > bus && secondary != 0) {
                    scan_bus(secondary);
                }
            }

            pci_device_t *dev = (pci_device_t*)malloc_alloc(sizeof(pci_device_t));
            dev->base_class = base;
            dev->subclass = sub;
            dev->bus = bus;
            dev->func = func;
            dev->prog_if = pci_get_prog_if(bus, device, func);
            dev->slot = device;

            for (int i = 0; i < 6; i++) {
                uint32_t bar = pci_read_config(bus, device, func, 0x10 + i * 4);
                dev->bars[i] = bar;
            }

            devices[pci_count] = dev;
            pci_count++;
        } 
    }
}

void pci_init(void) {
    scan_bus(0);

    for (uint64_t dev = 0; dev < 1024; dev++) {
        if (devices[dev] != NULL) {
            if (devices[dev]->base_class == 0x01 && devices[dev]->subclass == 0x08) {
                nvme_init(devices[dev]);
            }

            if (devices[dev]->base_class == 0x0C && devices[dev]->subclass == 0x03) {
                if (devices[dev]->prog_if == 0x30) { // xhci
                    xhci_init(devices[dev]);
                }
            }
        }
    }
}

uint8_t pci_find_capability(uint8_t bus, uint8_t slot, uint8_t func, uint8_t cap_id) {
    uint32_t status = pci_read_config(bus, slot, func, 0x04);
    if (!(status & (1u << 4))) return 0;

    uint8_t ptr = pci_read_config(bus, slot, func, 0x34) & 0xFC;

    for (int guard = 0; guard < 48; guard++) {
        if (ptr == 0) return 0;

        uint32_t hdr = pci_read_config(bus, slot, func, ptr);
        uint8_t  id  = hdr & 0xFF;
        uint8_t  next = (hdr >> 8) & 0xFC;

        if (id == cap_id) return ptr;
        ptr = next;
    }
    return 0;
}