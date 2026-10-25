#include "kernel.h"
#include <fs/vfs.h>
#include <fs/devfs.h>
#include <gconsole.h>
#include <bootloader.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <scheduler/scheduler.h>
#include <errno.h>
#include <memory.h>

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOPAN_DISPLAY     0x4606
#define FBIOBLANK           0x4611
#define FBIO_WAITFORVSYNC   0x4620

// Флаг Write-Combining для быстрой записи по PCIe
#ifndef PTE_PWT
#define PTE_PWT (1ULL << 3) // Page Write-Through / Write-Combining
#endif

struct fb_bitfield {
    uint32_t offset;
    uint32_t length;
    uint32_t msb_right;
};

struct fb_var_screeninfo {
    uint32_t xres;
    uint32_t yres;
    uint32_t xres_virtual;
    uint32_t yres_virtual;
    uint32_t xoffset;
    uint32_t yoffset;
    uint32_t bits_per_pixel;
    uint32_t grayscale;
    struct fb_bitfield red;
    struct fb_bitfield green;
    struct fb_bitfield blue;
    struct fb_bitfield transp;
    uint32_t nonstd;
    uint32_t activate;
    uint32_t height;
    uint32_t width;
    uint32_t accel_flags;
    uint32_t pixclock;
    uint32_t left_margin;
    uint32_t right_margin;
    uint32_t upper_margin;
    uint32_t lower_margin;
    uint32_t hsync_len;
    uint32_t vsync_len;
    uint32_t sync;
    uint32_t vmode;
    uint32_t rotate;
    uint32_t reserved[5];
};

struct fb_fix_screeninfo {
    char id[16];
    uint64_t smem_start;
    uint32_t smem_len;
    uint32_t type;
    uint32_t type_aux;
    uint32_t visual;
    uint16_t xpanstep;
    uint16_t ypanstep;
    uint16_t ywrapstep;
    uint32_t line_length;
    uint64_t mmio_start;
    uint32_t mmio_len;
    uint32_t accel;
    uint16_t capabilities;
    uint16_t reserved[2];
};

static int fb_dev_ioctl(vfs_file_t *file, uint64_t request, void *arg) {
    (void)file;
    struct limine_framebuffer_response *fb_resp = get_base_framebuffer();
    if (!fb_resp || fb_resp->framebuffer_count == 0 || !arg) return -EINVAL;

    struct limine_framebuffer *fb = fb_resp->framebuffers[get_base_index_framebuffer()];

    if (request == FBIOGET_VSCREENINFO || request == FBIOPUT_VSCREENINFO) {
        struct fb_var_screeninfo *var = (struct fb_var_screeninfo *)arg;
        memset(var, 0, sizeof(*var));

        var->xres = (uint32_t)fb->width;
        var->yres = (uint32_t)fb->height;
        var->xres_virtual = (uint32_t)fb->width;
        var->yres_virtual = (uint32_t)fb->height;
        var->bits_per_pixel = (uint32_t)fb->bpp;

        var->red.offset   = fb->red_mask_shift;   var->red.length   = fb->red_mask_size;
        var->green.offset = fb->green_mask_shift; var->green.length = fb->green_mask_size;
        var->blue.offset  = fb->blue_mask_shift;  var->blue.length  = fb->blue_mask_size;
        return 0;
    }

    if (request == FBIOGET_FSCREENINFO) {
        struct fb_fix_screeninfo *fix = (struct fb_fix_screeninfo *)arg;
        memset(fix, 0, sizeof(*fix));

        uint64_t hhdm = get_bootinfo().hhdm->offset;
        uint64_t phys_addr = (uint64_t)fb->address - hhdm;

        memcpy(fix->id, "BoldonFB", 8);
        fix->smem_start  = phys_addr;
        fix->smem_len    = (uint32_t)(fb->height * fb->pitch);
        fix->type        = 0; /* FB_TYPE_PACKED_PIXELS */
        fix->visual      = 2; /* FB_VISUAL_TRUECOLOR */
        fix->line_length = (uint32_t)fb->pitch;
        return 0;
    }

    // Добавили обработку VSync / Pan Display для плавности
    if (request == FBIOPAN_DISPLAY || request == FBIO_WAITFORVSYNC || request == FBIOBLANK) {
        return 0;
    }

    return -25; // -ENOTTY
}

static void *fb_dev_mmap(vfs_file_t *file, void *addr, uint64_t len, int prot, int flags, uint64_t offset) {
    (void)file; (void)prot; (void)flags; (void)offset;
    process_t *p = get_current_process();
    struct limine_framebuffer_response *fb_resp = get_base_framebuffer();
    if (!p || !fb_resp || fb_resp->framebuffer_count == 0) return NULL;

    struct limine_framebuffer *fb = fb_resp->framebuffers[get_base_index_framebuffer()];

    uint64_t hhdm = get_bootinfo().hhdm->offset;
    uint64_t phys_addr = (uint64_t)fb->address - hhdm;

    uint64_t virt = addr ? (uint64_t)addr : p->mmap_base;
    if (!addr) p->mmap_base += (len + 4095) & ~4095ULL;

    uint64_t pages = (len + 4095) / 4096;

    // ВАЖНО: Добавляем PTE_PWT (Write-Combining)!
    // Это включает аппаратный PCIe burst-mode для процессора.
    for (uint64_t i = 0; i < pages; i++) {
        vmm_map_page(p->pml4, phys_addr + i * 4096, virt + i * 4096,
                     PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_PWT);
    }

    return (void *)virt;
}

static vfs_file_ops_t fb_file_ops = {
    .ioctl = fb_dev_ioctl,
    .mmap  = fb_dev_mmap
};

void fbdev_init(void) {
    devfs_register_device("fb0",       VFS_TYPE_CHAR_DEV, &fb_file_ops, NULL);
    devfs_register_device("fb/0",      VFS_TYPE_CHAR_DEV, &fb_file_ops, NULL);
    devfs_register_device("dri/card0", VFS_TYPE_CHAR_DEV, &fb_file_ops, NULL);
    devfs_register_device("card0",     VFS_TYPE_CHAR_DEV, &fb_file_ops, NULL);
}