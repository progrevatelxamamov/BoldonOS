#include <disk/block.h>
#include <bootloader.h>
#include <limine.h>
#include <stdint.h>
#include <gconsole.h>
#include <io.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/malloc.h>
#include <desc/gdt.h>
#include <desc/idt.h>
#include <cpu/acpi.h>
#include <cpu/pic.h>
#include <cpu/apic.h>
#include <cpu/time.h>
#include <scheduler/scheduler.h>
#include <pci/pci.h>
#include <disk/partitions.h>
#include <memory.h>
#include <inout/inout.h>
#include <inout/ps2kbd.h>
#include <inout/ps2mouse.h>
#include <inout/ps2.h>
#include <gui/gui.h>
#include <fs/devfs.h>
#include <fs/pty.h>
#include <syscall/syscall.h>
#include <fs/timerfd.h>
#include <fs/signalfd.h>
#include <fs/shmem.h>
#include "lwip/tcpip.h"
#include <net/wifi.h>
#include <cpu/smp.h>

static bootloader_t bootinfo;

bootloader_t get_bootinfo(void) {
    return bootinfo;
}

static void parse_boot_serial_from_cmdline(void) {
    bootloader_t bi = get_bootinfo();
    if (!bi.kernel_file || !bi.kernel_file->cmdline) return;

    const char *cmd = bi.kernel_file->cmdline;
    const char *key = "boot_serial=";
    const char *p = cmd;

    while (*p) {
        const char *k = key;
        const char *s = p;
        while (*k && *s && *k == *s) { k++; s++; }
        if (*k == 0) {
            char guid[40];
            int i = 0;
            while (s[i] && s[i] != ' ' && s[i] != '\t' && i < 39) {
                guid[i] = s[i];
                i++;
            }
            guid[i] = 0;
            if (i > 0) {
                partitions_set_boot_serial(guid);
            }
            return;
        }
        p++;
    }
}

void enable_sse(void) {
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2);
    cr0 |=  (1ULL << 1);
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));

    uint32_t a, b, c, d;

    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(1), "c"(0));
    int has_xsave = (c & (1u << 26)) != 0;

    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9);
    cr4 |= (1ULL << 10);
    if (has_xsave) cr4 |= (1ULL << 18);

    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(7), "c"(0));
    if (b & (1u << 0)) cr4 |= (1ULL << 16);

    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));

    if (has_xsave) {
        uint32_t xcr0_lo, xcr0_hi;
        __asm__ volatile("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
        xcr0_lo |= 1u | 2u;
        if (c & (1u << 28)) {
            __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                             : "a"(7), "c"(0));
            if (b & (1u << 5)) xcr0_lo |= 4u;
            if (b & (1u << 16)) xcr0_lo |= 0xE0u;
        }
        __asm__ volatile("xsetbv" :: "a"(xcr0_lo), "d"(xcr0_hi), "c"(0));
    }
}

void cpu_enable_features(void) {
    uint64_t cr0, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2);
    cr0 |= (1ULL << 1);
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));

    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9);  // OSFXSR
    cr4 |= (1ULL << 10); // OSXMMEXCPT
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));
}

void kstart(bootloader_t blinfo) {
    bootinfo = blinfo;
    set_base_framebuffer(blinfo.framebuffer);
    set_base_index_framebuffer(0);
    enable_sse();
    cpu_enable_features();

    pmm_init();
    vmm_init();
    malloc_init();
    gconsole_init_backbuffer();

    extern void draw_splash_screen(bootloader_t *blinfo);
    draw_splash_screen(&bootinfo);

    gdt_init();
    idt_init();
    PIC_remap(0x20, 0x28);
    acpi_init();
    smp_init();
    scheduler_init();
    time_init();
    tcpip_init(NULL, NULL);
    pci_init();

    partitions_level_init();
    parse_boot_serial_from_cmdline();
    mount_all_partitions();

    devfs_vfs_init();
    vfs_mount_fs("devfs", NULL, 'D', NULL);
    extern void fbdev_init(void);
    extern void evdev_init(void);
    extern void pty_register_devfs(void);
    fbdev_init();
    evdev_init();
    pty_register_devfs();

    extern int shmem_init(void);
    extern int timerfd_init(void);
    extern int signalfd_init(void);
    extern int eventfd_init(void);
    extern void input_register_devfs(void);
    extern void base_devs_register(void);
    shmem_init();
    timerfd_init();
    signalfd_init();
    eventfd_init();
    input_register_devfs();
    base_devs_register();

    syscall_init();

    inout_init();
    ps2_init();
    //gui_init();

    extern void wifi_start_boot_task(void);
    wifi_start_boot_task();

    extern void launch_init_process(void);
    create_task_kernel((void *)launch_init_process, "init");

    __asm__ volatile ("sti");

    while (1) {
        __asm__ volatile ("hlt");
    }
}