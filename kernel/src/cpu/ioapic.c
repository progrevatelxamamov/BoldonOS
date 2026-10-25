#include <cpu/ioapic.h>
#include <cpu/apic.h>
#include <mm/vmm.h>
#include <bootloader.h>
#include <kernel.h>
#include <gconsole.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

static ioapic_chip_t chips[IOAPIC_MAX_CHIPS];
static int           chips_count = 0;

typedef struct {
    uint32_t gsi;
    uint8_t  polarity;   /* 0 = active high, 1 = active low */
    uint8_t  trigger;    /* 0 = edge,        1 = level      */
    bool     valid;
} irq_mapping_t;

static irq_mapping_t irq_map[16];

#define IOAPIC_IOREGSEL 0x00
#define IOAPIC_IOWIN    0x10

static inline uint32_t ioapic_read(ioapic_chip_t *c, uint32_t reg) {
    c->base[IOAPIC_IOREGSEL / 4] = reg;
    __asm__ volatile("" ::: "memory");
    return c->base[IOAPIC_IOWIN / 4];
}

static inline void ioapic_write(ioapic_chip_t *c, uint32_t reg, uint32_t val) {
    c->base[IOAPIC_IOREGSEL / 4] = reg;
    __asm__ volatile("" ::: "memory");
    c->base[IOAPIC_IOWIN / 4] = val;
    __asm__ volatile("" ::: "memory");
}

#define IOAPIC_REG_ID      0x00
#define IOAPIC_REG_VER     0x01
#define IOAPIC_REG_ARB     0x02
#define IOAPIC_RTE_LOW(n)  (0x10 + 2 * (n))
#define IOAPIC_RTE_HIGH(n) (0x11 + 2 * (n))

static ioapic_chip_t *ioapic_find_by_gsi(uint32_t gsi, uint32_t *pin_out) {
    for (int i = 0; i < chips_count; i++) {
        ioapic_chip_t *c = &chips[i];
        if (gsi >= c->gsi_base && gsi < c->gsi_base + c->num_pins) {
            if (pin_out) *pin_out = gsi - c->gsi_base;
            return c;
        }
    }
    return NULL;
}

int ioapic_register(uint64_t mmio_phys, uint32_t gsi_base) {
    if (chips_count >= IOAPIC_MAX_CHIPS) return -1;

    uint64_t hhdm = get_bootinfo().hhdm->offset;
    uint64_t virt = mmio_phys + hhdm;

    vmm_map_page(get_kernel_pml4(),
                 mmio_phys,
                 virt,
                 PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_PWT);

    ioapic_chip_t *c = &chips[chips_count];
    c->base     = (volatile uint32_t *)virt;
    c->gsi_base = gsi_base;
    c->id       = (ioapic_read(c, IOAPIC_REG_ID) >> 24) & 0xF;

    uint32_t ver = ioapic_read(c, IOAPIC_REG_VER);
    c->num_pins  = ((ver >> 16) & 0xFF) + 1;

    chips_count++;
    return chips_count - 1;
}

void ioapic_register_override(uint8_t irq, uint32_t gsi,
                              uint8_t polarity, uint8_t trigger)
{
    if (irq >= 16) return;

    uint8_t pol = (polarity == 3) ? 1 : 0;
    uint8_t trg = (trigger  == 3) ? 1 : 0;

    irq_map[irq].gsi      = gsi;
    irq_map[irq].polarity = pol;
    irq_map[irq].trigger  = trg;
    irq_map[irq].valid    = true;
}

static uint64_t build_rte(uint8_t vector, uint8_t apic_id,
                          uint8_t polarity, uint8_t trigger,
                          bool masked)
{
    uint64_t rte = 0;

    rte |= (uint64_t)vector;

    rte |= ((uint64_t)polarity & 1) << 13;
    rte |= ((uint64_t)trigger  & 1) << 15;
    if (masked)
        rte |= (uint64_t)1 << 16;

    rte |= ((uint64_t)apic_id & 0xFF) << 56;

    return rte;
}

uint64_t ioapic_read_rte(uint8_t irq) {
    uint32_t gsi = irq_map[irq].valid ? irq_map[irq].gsi : irq;
    uint32_t pin;
    ioapic_chip_t *c = ioapic_find_by_gsi(gsi, &pin);
    if (!c) return 0;

    uint64_t low  = ioapic_read(c, IOAPIC_RTE_LOW(pin));
    uint64_t high = ioapic_read(c, IOAPIC_RTE_HIGH(pin));
    return (high << 32) | low;
}

void ioapic_write_rte(uint8_t irq, uint64_t rte) {
    uint32_t gsi = irq_map[irq].valid ? irq_map[irq].gsi : irq;
    uint32_t pin;
    ioapic_chip_t *c = ioapic_find_by_gsi(gsi, &pin);
    if (!c) return;

    ioapic_write(c, IOAPIC_RTE_HIGH(pin), (uint32_t)(rte >> 32));
    ioapic_write(c, IOAPIC_RTE_LOW(pin),  (uint32_t)(rte & 0xFFFFFFFFu));
}

int ioapic_set_irq(uint8_t irq, uint8_t vector, uint8_t apic_id) {
    if (irq >= 16) return -1;

    uint32_t gsi = irq_map[irq].valid ? irq_map[irq].gsi : irq;
    uint8_t  pol = irq_map[irq].valid ? irq_map[irq].polarity : 0;
    uint8_t  trg = irq_map[irq].valid ? irq_map[irq].trigger  : 0;

    uint32_t pin;
    ioapic_chip_t *c = ioapic_find_by_gsi(gsi, &pin);
    if (!c) return -1;

    uint64_t rte = build_rte(vector, apic_id, pol, trg, /*masked=*/false);
    ioapic_write_rte(irq, rte);
    return 0;
}

int ioapic_mask_irq(uint8_t irq) {
    if (irq >= 16) return -1;
    uint64_t rte = ioapic_read_rte(irq);
    rte |= (uint64_t)1 << 16;
    ioapic_write_rte(irq, rte);
    return 0;
}

int ioapic_unmask_irq(uint8_t irq) {
    if (irq >= 16) return -1;
    uint64_t rte = ioapic_read_rte(irq);
    rte &= ~((uint64_t)1 << 16);
    ioapic_write_rte(irq, rte);
    return 0;
}

void ioapic_init_legacy(uint8_t bsp_apic_id, uint8_t vector_base) {
    for (uint8_t irq = 0; irq < 16; irq++) {
        uint32_t gsi = irq_map[irq].valid ? irq_map[irq].gsi : irq;
        uint32_t pin;
        ioapic_chip_t *c = ioapic_find_by_gsi(gsi, &pin);
        if (!c) continue;

        uint64_t rte = build_rte(vector_base + irq, bsp_apic_id,
                                 /*pol=*/0, /*trg=*/0, /*masked=*/true);
        ioapic_write_rte(irq, rte);
    }

    ioapic_unmask_irq(0);
    ioapic_unmask_irq(1);
    ioapic_unmask_irq(12);
    /* ioapic_unmask_irq(4); */
}