#ifndef GDT_H
#define GDT_H

#include <stdint.h>

struct tss_entry {
    uint32_t reserved0;
    uint64_t rsp0;      // Stack pointer for Ring 0
    uint64_t rsp1;      // Stack pointer for Ring 1 (unused)
    uint64_t rsp2;      // Stack pointer for Ring 2 (unused)
    uint64_t reserved1;
    uint64_t ist1;      // Interrupt Stack Table 1
    uint64_t ist2;      // IST 2
    uint64_t ist3;      // IST 3
    uint64_t ist4;      // IST 4
    uint64_t ist5;      // IST 5
    uint64_t ist6;      // IST 6
    uint64_t ist7;      // IST 7
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset; // I/O Permission Bitmap offset
} __attribute__((packed));

/* Segment Selector Offsets */
#define KERNEL_CS   0x08
#define KERNEL_DS   0x10
#define USER_CODE   0x20
#define USER_DATA   0x18

/* Bit shift definitions */
#define SEG_DESCTYPE(x)  ((uint16_t)(x) << 0x04) // Descriptor type (0 for system, 1 for code/data)
#define SEG_PRES(x)      ((uint16_t)(x) << 0x07) // Present
#define SEG_SAVL(x)      ((uint16_t)(x) << 0x0C) // Available for system use
#define SEG_LONG(x)      ((uint16_t)(x) << 0x0D) // Long mode (64-bit code)
#define SEG_SIZE(x)      ((uint16_t)(x) << 0x0E) // Size (0 for 64-bit/16-bit, 1 for 32-bit)
#define SEG_GRAN(x)      ((uint16_t)(x) << 0x0F) // Granularity (0 for 1B, 1 for 4KB)
#define SEG_PRIV(x)     (((uint16_t)(x) & 0x03) << 0x05) // Privilege level (0 - 3)

#define SEG_DATA_RD        0x00 // Read-Only
#define SEG_DATA_RDA       0x01 // Read-Only, accessed
#define SEG_DATA_RDWR      0x02 // Read/Write
#define SEG_DATA_RDWRA     0x03 // Read/Write, accessed
#define SEG_DATA_RDEXPD    0x04 // Read-Only, expand-down
#define SEG_DATA_RDEXPDA   0x05 // Read-Only, expand-down, accessed
#define SEG_DATA_RDWREXPD  0x06 // Read/Write, expand-down
#define SEG_DATA_RDWREXPDA 0x07 // Read/Write, expand-down, accessed
#define SEG_CODE_EX        0x08 // Execute-Only
#define SEG_CODE_EXA       0x09 // Execute-Only, accessed
#define SEG_CODE_EXRD      0x0A // Execute/Read
#define SEG_CODE_EXRDA     0x0B // Execute/Read, accessed
#define SEG_CODE_EXC       0x0C // Execute-Only, conforming
#define SEG_CODE_EXCA      0x0D // Execute-Only, conforming, accessed
#define SEG_CODE_EXRDC     0x0E // Execute/Read, conforming
#define SEG_CODE_EXRDCA    0x0F // Execute/Read, conforming, accessed

/* 
 * 64-bit Kernel Code: Long=1, Size=0
 */
#define GDT_CODE_PL0 (SEG_DESCTYPE(1) | SEG_PRES(1) | SEG_SAVL(0) | \
                      SEG_LONG(1)     | SEG_SIZE(0) | SEG_GRAN(1) | \
                      SEG_PRIV(0)     | SEG_CODE_EXRD)

#define GDT_DATA_PL0 (SEG_DESCTYPE(1) | SEG_PRES(1) | SEG_SAVL(0) | \
                      SEG_LONG(0)     | SEG_SIZE(1) | SEG_GRAN(1) | \
                      SEG_PRIV(0)     | SEG_DATA_RDWR)

/* 
 * 64-bit User Code: Long=1, Size=0, Priv=3
 */
#define GDT_CODE_PL3 (SEG_DESCTYPE(1) | SEG_PRES(1) | SEG_SAVL(0) | \
                      SEG_LONG(1)     | SEG_SIZE(0) | SEG_GRAN(1) | \
                      SEG_PRIV(3)     | SEG_CODE_EXRD)

#define GDT_DATA_PL3 (SEG_DESCTYPE(1) | SEG_PRES(1) | SEG_SAVL(0) | \
                      SEG_LONG(0)     | SEG_SIZE(1) | SEG_GRAN(1) | \
                      SEG_PRIV(3)     | SEG_DATA_RDWR)

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

void gdt_init(void);
void gdt_init_cpu(int cpu);
void tss_set_stack(uint64_t stack);
uint64_t tss_get_stack(void);

#endif