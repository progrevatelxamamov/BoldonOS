#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Standard SDT Header shared by all ACPI tables
struct SDT_header {
    char     Signature[4];
    uint32_t Length;
    uint8_t  Revision;
    uint8_t  Checksum;
    char     OEMID[6];
    char     OEMTableID[8];
    uint32_t OEMRevision;
    uint32_t CreatorID;
    uint32_t CreatorRevision;
} __attribute__((packed));

// ACPI 1.0 RSDP
struct RSDP_t {
    char     Signature[8];
    uint8_t  Checksum;
    char     OEMID[6];
    uint8_t  Revision;
    uint32_t RsdtAddress;
} __attribute__((packed));

// ACPI 2.0+ Extended RSDP
struct XSDP_t {
    struct RSDP_t rsdp;
    uint32_t      Length;
    uint64_t      XsdtAddress;
    uint8_t       ExtendedChecksum;
    uint8_t       Reserved[3];
} __attribute__((packed));

// 32-bit Root System Description Table (ACPI 1.0)
struct RSDT {
    struct SDT_header header;
    uint32_t          entries[];
} __attribute__((packed));

// 64-bit Extended System Description Table (ACPI 2.0+)
struct XSDT {
    struct SDT_header header;
    uint64_t          entries[];
} __attribute__((packed));

struct MADT {
    struct SDT_header header;
    uint32_t LocalAPICAddress;
    uint32_t Flags;
} __attribute__((packed));

struct HPET_Address {
    uint8_t  space_id;      // 0 = System Memory (MMIO)
    uint8_t  bit_width;
    uint8_t  bit_offset;
    uint8_t  access_width;
    uint64_t address;         // Физический адрес MMIO HPET
} __attribute__((packed));

struct HPET_Table {
    struct SDT_header header;
    uint8_t  hardware_rev_id;
    uint8_t  comparator_count : 5;
    uint8_t  counter_size : 1;
    uint8_t  reserved : 1;
    uint8_t  legacy_replacement : 1;
    uint16_t pci_vendor_id;
    struct HPET_Address address;
    uint8_t  hpet_number;
    uint16_t minimum_tick;
    uint8_t  page_protection;
} __attribute__((packed));

struct MADT_entry {
    uint8_t type;
    uint8_t length;
} __attribute__((packed));

/* Типы записей */
enum MADT_entry_type {
    MADT_TYPE_LAPIC          = 0,   /* Processor Local APIC */
    MADT_TYPE_IOAPIC         = 1,   /* I/O APIC */
    MADT_TYPE_ISO            = 2,   /* Interrupt Source Override */
    MADT_TYPE_NMI_SOURCE     = 3,   /* Non-maskable Interrupt Source */
    MADT_TYPE_LAPIC_NMI      = 4,   /* Local APIC NMI */
    MADT_TYPE_LAPIC_OVERRIDE = 5,   /* Local APIC Address Override */
    MADT_TYPE_IOSAPIC        = 6,   /* I/O SAPIC (IA-64) */
    MADT_TYPE_LSAPIC         = 7,   /* Local SAPIC (IA-64) */
    MADT_TYPE_PLATFORM_INT   = 8,   /* Platform Interrupt Source */
    MADT_TYPE_LX2APIC        = 9,   /* Processor Local x2APIC */
    MADT_TYPE_LX2APIC_NMI    = 10,  /* Local x2APIC NMI */
    MADT_TYPE_GICC           = 11,  /* GIC CPU Interface (ARM) */
    MADT_TYPE_GICD           = 12,  /* GIC Distributor (ARM) */
    MADT_TYPE_GIC_MSI        = 13,  /* GIC MSI Frame (ARM) */
    MADT_TYPE_GICR           = 14,  /* GIC Redistributor (ARM) */
    MADT_TYPE_GIC_ITS        = 15,  /* GIC ITS (ARM) */
};

struct MADT_lapic {
    struct MADT_entry entry;      /* type = 0, length = 8 */
    uint8_t  acpi_processor_id;   /* ACPI Processor UID */
    uint8_t  apic_id;             /* LAPIC ID */
    uint32_t flags;               /* bit 0: enabled
                                     bit 1: online capable (ACPI 6.3+) */
} __attribute__((packed));

#define MADT_LAPIC_ENABLED         (1U << 0)
#define MADT_LAPIC_ONLINE_CAPABLE  (1U << 1)

struct MADT_ioapic {
    struct MADT_entry entry;      /* type = 1, length = 12 */
    uint8_t  ioapic_id;
    uint8_t  reserved;
    uint32_t ioapic_address;
    uint32_t gsi_base;
} __attribute__((packed));

struct MADT_iso {
    struct MADT_entry entry;      /* type = 2, length = 10 */
    uint8_t  bus;
    uint8_t  irq_source;
    uint32_t gsi;
    uint16_t flags;
} __attribute__((packed));

#define MADT_ISO_POLARITY_MASK   0x3
#define MADT_ISO_POLARITY_HIGH   0x1
#define MADT_ISO_POLARITY_LOW    0x3
#define MADT_ISO_TRIGGER_MASK    0xC
#define MADT_ISO_TRIGGER_EDGE    0x4
#define MADT_ISO_TRIGGER_LEVEL   0xC

struct MADT_nmi_source {
    struct MADT_entry entry;      /* type = 3, length = 8 */
    uint16_t flags;               /* polarity + trigger (ISO) */
    uint32_t gsi;                 /* GSI, NMI */
} __attribute__((packed));

struct MADT_lapic_nmi {
    struct MADT_entry entry;      /* type = 4, length = 6 */
    uint8_t  acpi_processor_id;   /* 0xFF = all processors */
    uint16_t flags;               /* polarity + trigger */
    uint8_t  lint;                /* LINT0 (0) or LINT1 (1) */
} __attribute__((packed));

struct MADT_lapic_override {
    struct MADT_entry entry;      /* type = 5, length = 12 */
    uint16_t reserved;
    uint64_t lapic_address;       /* 64 bit phys address LAPIC */
} __attribute__((packed));

struct MADT_platform_int {
    struct MADT_entry entry;      /* type = 8, length = 16 */
    uint16_t flags;
    uint8_t  type;                /* 0 = PMI, 1 = INIT, 2.. — reserved */
    uint8_t  processor_id;
    uint8_t  processor_eid;
    uint8_t  io_sapic_vector;
    uint32_t gsi;
    uint32_t platform_int_flags;
} __attribute__((packed));

struct MADT_lx2apic {
    struct MADT_entry entry;      /* type = 9, length = 16 */
    uint16_t reserved;
    uint32_t x2apic_id;           /* 32-битный x2APIC ID */
    uint32_t flags;               /* как у MADT_lapic */
    uint32_t acpi_processor_id;   /* ACPI Processor UID */
} __attribute__((packed));

struct MADT_lx2apic_nmi {
    struct MADT_entry entry;      /* type = 10, length = 12 */
    uint16_t flags;
    uint32_t acpi_processor_id;
    uint8_t  lint;
    uint8_t  reserved[3];
} __attribute__((packed));

struct MADT_gicc {
    struct MADT_entry entry;      /* type = 11 */
    uint8_t  reserved;
    uint32_t cpu_interface_number;
    uint32_t acpi_processor_id;
    uint32_t flags;
    uint32_t parking_protocol_version;
    uint32_t performance_interrupt_gsiv;
    uint64_t parked_address;
    uint64_t physical_base_address;
    uint64_t gicv;
    uint64_t gich;
    uint32_t vgic_maintenance_interrupt;
    uint64_t gicr_base_address;
    uint64_t mpidr;
    uint8_t  efficiency_class;
    uint8_t  reserved2;
    uint16_t spe_overflow_interrupt;
} __attribute__((packed));

void acpi_init(void);
void *acpi_find_table(const char *signature);

uint64_t hpet_ticks_for_ms(uint32_t ms);
uint64_t hpet_read_counter(void);
void hpet_sleep_ms(uint64_t ms);

#endif