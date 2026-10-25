#ifndef BOOTLOADER_H
#define BOOTLOADER_H

#include <limine.h>

typedef struct {
    struct limine_framebuffer_response *framebuffer;
    struct limine_rsdp_response *acpi;
    struct limine_memmap_response *memmap;
    struct limine_hhdm_response *hhdm;
    struct limine_executable_cmdline_response *kernel_file;
    struct limine_module_response *modules;
} bootloader_t;

#endif