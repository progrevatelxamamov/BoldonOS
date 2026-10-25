#ifndef IOAPIC_H
#define IOAPIC_H

#include <stdint.h>
#include <stdbool.h>

#define IOAPIC_MAX_CHIPS 8
#define IOAPIC_MAX_GSI   256

typedef struct {
    volatile uint32_t *base;
    uint32_t gsi_base;
    uint32_t num_pins;
    uint32_t id;
} ioapic_chip_t;

int  ioapic_register(uint64_t mmio_phys, uint32_t gsi_base);

void ioapic_register_override(uint8_t irq, uint32_t gsi,
                              uint8_t polarity, uint8_t trigger);

int  ioapic_set_irq(uint8_t irq, uint8_t vector, uint8_t apic_id);
int  ioapic_mask_irq(uint8_t irq);
int  ioapic_unmask_irq(uint8_t irq);

void ioapic_init_legacy(uint8_t bsp_apic_id, uint8_t vector_base);

uint64_t ioapic_read_rte(uint8_t irq);
void     ioapic_write_rte(uint8_t irq, uint64_t rte);

#endif