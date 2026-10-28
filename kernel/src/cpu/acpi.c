#include <cpu/acpi.h>
#include <cpu/apic.h>
#include <stdbool.h>
#include <stdint.h>
#include <bootloader.h>
#include <kernel.h>
#include <gconsole.h>
#include <memory.h>
#include <mm/vmm.h>
#include <cpu/pic.h>
#include <cpu/ioapic.h>

static struct RSDT *rsdt = NULL;
static struct XSDT *xsdt = NULL;
static uint64_t hhdm_offset = 0;

static inline void *phys_to_virt(uint64_t phys_addr) {
    return (void *)(phys_addr + hhdm_offset);
}

static bool acpi_checksum(const void *ptr, size_t size) {
    uint8_t sum = 0;
    const uint8_t *bytes = (const uint8_t *)ptr;
    for (size_t i = 0; i < size; i++) {
        sum += bytes[i];
    }
    return sum == 0;
}

static volatile uint64_t *hpet_base = NULL;
static uint32_t hpet_clock_period_fs = 0;

uint8_t lapic_ids[32];
int lapic_count = 0;

#define HPET_REG(off) (*(volatile uint64_t *)((uint64_t)hpet_base + (off)))

void hpet_init(void) {
    struct HPET_Table *hpet_sdt = (struct HPET_Table *)acpi_find_table("HPET");
    if (!hpet_sdt) {
        console_print_string("[HPET] Table not found!\n");
        return;
    }

    uint64_t phys_addr = hpet_sdt->address.address;
    uint64_t virt_addr = phys_addr + get_bootinfo().hhdm->offset;

    vmm_map_page(get_kernel_pml4(), 
                 phys_addr, 
                 virt_addr, 
                 PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_PWT);

    hpet_base = (volatile uint64_t *)virt_addr;

    uint64_t caps = HPET_REG(0x00);
    hpet_clock_period_fs = (uint32_t)(caps >> 32);

    HPET_REG(0x10) |= 1ULL;

    console_print_string("[HPET] Initialized! Period: ");
    console_print_hex(hpet_clock_period_fs);
    console_print_string(" fs\n");
}

uint64_t hpet_read_counter(void) {
    return hpet_base ? HPET_REG(0xF0) : 0;
}

uint64_t hpet_ticks_for_ms(uint32_t ms) {
    if (!hpet_base || !hpet_clock_period_fs) return 0;
    return ((uint64_t)ms * 1000000000000ULL) / hpet_clock_period_fs;
}

void hpet_sleep_ms(uint64_t ms) {
    if (!hpet_base || hpet_clock_period_fs == 0) return;

    uint64_t ticks_to_wait = (ms * 1000000000000ULL) / hpet_clock_period_fs;
    uint64_t target = hpet_read_counter() + ticks_to_wait;

    while (hpet_read_counter() < target) {
        __asm__ volatile("pause");
    }
}

void madt_init(void) {
    void *head = acpi_find_table("APIC");
    if (!head) { return; }

    struct MADT *madt = (struct MADT *)head;

    if (madt->LocalAPICAddress == 0) { return; }

    uint64_t lapic_phys = madt->LocalAPICAddress;
    uint64_t lapic_virt = lapic_phys + get_bootinfo().hhdm->offset;

    vmm_map_page(get_kernel_pml4(),
                 lapic_phys,
                 lapic_virt,
                 PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_PWT);

    volatile uint32_t *lapic = (volatile uint32_t *)lapic_virt;

    int rc = enable_apic((uint32_t *)lapic, 32, 1000);
    if (rc < 0) {
        console_print_string("[ACPI] APIC init failed, PIC stays on\n");
        return;
    }

    uint8_t *p   = (uint8_t *)madt + sizeof(struct MADT);
    uint8_t *end = (uint8_t *)madt + madt->header.Length;

    while (p + sizeof(struct MADT_entry) <= end) {
        struct MADT_entry *e = (struct MADT_entry *)p;
        if (e->length < sizeof(struct MADT_entry)) break;
        if (p + e->length > end) break;

        switch (e->type) {
        case MADT_TYPE_IOAPIC: {
            struct MADT_ioapic *io = (struct MADT_ioapic *)e;
            ioapic_register(io->ioapic_address, io->gsi_base);
            break;
        }
        case MADT_TYPE_ISO: {
            struct MADT_iso *iso = (struct MADT_iso *)e;
            uint8_t pol = (iso->flags & MADT_ISO_POLARITY_MASK) == MADT_ISO_POLARITY_LOW ? 1 : 0;
            uint8_t trg = (iso->flags & MADT_ISO_TRIGGER_MASK)  == MADT_ISO_TRIGGER_LEVEL  ? 1 : 0;
            ioapic_register_override(iso->irq_source, iso->gsi, pol, trg);
            break;
        }
        case MADT_TYPE_LAPIC_NMI: {
            break;
        }
        case MADT_TYPE_LAPIC_OVERRIDE: {
            struct MADT_lapic_override *lo = (struct MADT_lapic_override *)e;
            lapic_phys = lo->lapic_address;
            break;
        }
        case MADT_TYPE_LAPIC: {
            struct MADT_lapic *la = (struct MADT_lapic *)e;
            if ((la->flags & 1) && lapic_count < 32) {
                lapic_ids[lapic_count++] = la->apic_id;
            }
            break;
        }
        default:
            break;
        }
        p += e->length;
    }

    uint32_t cpuid_eax, cpuid_ebx, cpuid_ecx, cpuid_edx;
    cpuid(1, &cpuid_eax, &cpuid_ebx, &cpuid_ecx, &cpuid_edx);
    ioapic_init_legacy((uint8_t)(cpuid_ebx >> 24), 32);
    PIC_disable();
}

void acpi_init(void) {
    hhdm_offset = get_bootinfo().hhdm->offset;

    // Limine gives the RSDP address directly as a higher-half virtual address
    struct RSDP_t *rsdp = (struct RSDP_t *)get_bootinfo().acpi->address;

    if (!rsdp || memcmp(rsdp->Signature, "RSD PTR ", 8) != 0) {
        console_print_string("[ACPI] Invalid RSDP signature!\n");
        return;
    }

    // Verify ACPI 1.0 Checksum
    if (!acpi_checksum(rsdp, sizeof(struct RSDP_t))) {
        console_print_string("[ACPI] RSDP checksum failed!\n");
        return;
    }

    // Check if ACPI 2.0+ (XSDT available)
    if (rsdp->Revision >= 2) {
        struct XSDP_t *xsdp = (struct XSDP_t *)rsdp;
        if (acpi_checksum(xsdp, xsdp->Length)) {
            xsdt = (struct XSDT *)phys_to_virt(xsdp->XsdtAddress);
            console_print_string("[ACPI] Using 64-bit XSDT at ");
            console_print_hex((uint64_t)xsdt);
            console_print_string("\n");
        }
    }

    // Fallback to 32-bit RSDT if XSDT is unavailable
    if (!xsdt && rsdp->RsdtAddress != 0) {
        rsdt = (struct RSDT *)phys_to_virt(rsdp->RsdtAddress);
        console_print_string("[ACPI] Using 32-bit RSDT at ");
        console_print_hex((uint64_t)rsdt);
        console_print_string("\n");
    }

    hpet_init();
    madt_init();
}

// Function to find any table (e.g., "APIC" for MADT, "FACP" for FADT, "HPET")
void *acpi_find_table(const char *signature) {
    if (xsdt != NULL) {
        size_t entries_count = (xsdt->header.Length - sizeof(struct SDT_header)) / sizeof(uint64_t);
        for (size_t i = 0; i < entries_count; i++) {
            struct SDT_header *header = (struct SDT_header *)phys_to_virt(xsdt->entries[i]);
            if (memcmp(header->Signature, signature, 4) == 0) {
                if (acpi_checksum(header, header->Length)) {
                    return (void *)header;
                }
            }
        }
    }
    if (rsdt != NULL) {
        size_t entries_count = (rsdt->header.Length - sizeof(struct SDT_header)) / sizeof(uint32_t);
        for (size_t i = 0; i < entries_count; i++) {
            struct SDT_header *header = (struct SDT_header *)phys_to_virt(rsdt->entries[i]);
            if (memcmp(header->Signature, signature, 4) == 0) {
                if (acpi_checksum(header, header->Length)) {
                    return (void *)header;
                }
            }
        }
    }

    return NULL; // Table not found
}