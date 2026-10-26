#include "kernel.h"
#include <fs/vfs.h>
#include <fs/devfs.h>
#include <gconsole.h>
#include <bootloader.h>
#include <mm/vmm.h>
#include <scheduler/scheduler.h>
#include <errno.h>
#include <memory.h>
#include <stdint.h>

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOGETCMAP 0x4604
#define FBIOPUTCMAP 0x4605
#define FBIOPAN_DISPLAY 0x4606
#define FBIOBLANK 0x4611
#define FBIO_WAITFORVSYNC 0x4620

#define FB_TYPE_PACKED_PIXELS 0
#define FB_VISUAL_TRUECOLOR 2
#define FB_PROT_WRITE 2
#define FB_FILE_ACQUIRED ((void *)(uintptr_t)1)

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

struct fb_cmap {
    uint32_t start;
    uint32_t len;
    uint16_t *red;
    uint16_t *green;
    uint16_t *blue;
    uint16_t *transp;
};

static struct limine_framebuffer *fb_current(void) {
    struct limine_framebuffer_response *response = get_base_framebuffer();
    if (!response || response->framebuffer_count == 0) return NULL;

    uint64_t index = get_base_index_framebuffer();
    if (index >= response->framebuffer_count) return NULL;
    return response->framebuffers[index];
}

static uint64_t fb_size(struct limine_framebuffer *fb) {
    if (!fb) return 0;
    return fb->height * fb->pitch;
}

static uint64_t fb_phys(struct limine_framebuffer *fb) {
    if (!fb || !get_bootinfo().hhdm) return 0;
    return (uint64_t)fb->address - get_bootinfo().hhdm->offset;
}

static void fb_acquire_file(vfs_file_t *file) {
    if (!file || file->private_data == FB_FILE_ACQUIRED) return;
    file->private_data = FB_FILE_ACQUIRED;
    gconsole_acquire_framebuffer();
}

static int fb_validate_cmap(struct fb_cmap *cmap) {
    if (!cmap) return -EFAULT;
    if (cmap->start > 256 || cmap->len > 256) return -EINVAL;
    if (cmap->len > 256 - cmap->start) return -EINVAL;
    return 0;
}

static int fb_get_cmap(struct fb_cmap *cmap) {
    int result = fb_validate_cmap(cmap);
    if (result != 0) return result;

    for (uint32_t i = 0; i < cmap->len; i++) {
        uint16_t value = (uint16_t)((cmap->start + i) * 0x101U);
        if (cmap->red) cmap->red[i] = value;
        if (cmap->green) cmap->green[i] = value;
        if (cmap->blue) cmap->blue[i] = value;
        if (cmap->transp) cmap->transp[i] = 0xFFFF;
    }
    return 0;
}

static int fb_dev_ioctl(vfs_file_t *file, uint64_t request, void *arg) {
    (void)file;
    struct limine_framebuffer *fb = fb_current();
    if (!fb) return -ENODEV;

    if (request == FBIOGET_VSCREENINFO || request == FBIOPUT_VSCREENINFO) {
        if (!arg) return -EFAULT;
        struct fb_var_screeninfo *var = (struct fb_var_screeninfo *)arg;
        memset(var, 0, sizeof(*var));

        var->xres = (uint32_t)fb->width;
        var->yres = (uint32_t)fb->height;
        var->xres_virtual = (uint32_t)fb->width;
        var->yres_virtual = (uint32_t)fb->height;
        var->bits_per_pixel = (uint32_t)fb->bpp;

        var->red.offset = fb->red_mask_shift;
        var->red.length = fb->red_mask_size;
        var->green.offset = fb->green_mask_shift;
        var->green.length = fb->green_mask_size;
        var->blue.offset = fb->blue_mask_shift;
        var->blue.length = fb->blue_mask_size;
        if (fb->bpp == 32) {
            var->transp.offset = 24;
            var->transp.length = 8;
        }
        return 0;
    }

    if (request == FBIOGET_FSCREENINFO) {
        if (!arg) return -EFAULT;
        struct fb_fix_screeninfo *fix = (struct fb_fix_screeninfo *)arg;
        memset(fix, 0, sizeof(*fix));

        memcpy(fix->id, "BoldonFB", 8);
        fix->smem_start = fb_phys(fb);
        fix->smem_len = (uint32_t)fb_size(fb);
        fix->type = FB_TYPE_PACKED_PIXELS;
        fix->visual = FB_VISUAL_TRUECOLOR;
        fix->line_length = (uint32_t)fb->pitch;
        return 0;
    }

    if (request == FBIOGETCMAP) return fb_get_cmap((struct fb_cmap *)arg);
    if (request == FBIOPUTCMAP) return fb_validate_cmap((struct fb_cmap *)arg);

    if (request == FBIOPAN_DISPLAY) {
        if (!arg) return -EFAULT;
        struct fb_var_screeninfo *var = (struct fb_var_screeninfo *)arg;
        if (var->xoffset != 0 || var->yoffset != 0) return -EINVAL;
        return 0;
    }

    if (request == FBIO_WAITFORVSYNC || request == FBIOBLANK) return 0;

    return -ENOTTY;
}

static int fb_dev_read(vfs_file_t *file, void *buf, uint64_t count) {
    if (!file) return -EBADF;
    if (!buf) return -EFAULT;

    struct limine_framebuffer *fb = fb_current();
    if (!fb) return -ENODEV;

    uint64_t size = fb_size(fb);
    if (file->offset >= size) return 0;
    if (count > size - file->offset) count = size - file->offset;

    memcpy(buf, (uint8_t *)fb->address + file->offset, count);
    file->offset += count;
    return (int)count;
}

static int fb_dev_write(vfs_file_t *file, const void *buf, uint64_t count) {
    if (!file) return -EBADF;
    if (!buf) return -EFAULT;

    struct limine_framebuffer *fb = fb_current();
    if (!fb) return -ENODEV;

    uint64_t size = fb_size(fb);
    if (file->offset >= size) return 0;
    if (count > size - file->offset) count = size - file->offset;

    fb_acquire_file(file);
    memcpy((uint8_t *)fb->address + file->offset, buf, count);
    file->offset += count;
    return (int)count;
}

static uint64_t fb_dev_lseek(vfs_file_t *file, int64_t offset, int whence) {
    if (!file) return (uint64_t)-EBADF;

    struct limine_framebuffer *fb = fb_current();
    if (!fb) return (uint64_t)-ENODEV;

    int64_t base;
    if (whence == VFS_SEEK_SET) base = 0;
    else if (whence == VFS_SEEK_CUR) base = (int64_t)file->offset;
    else if (whence == VFS_SEEK_END) base = (int64_t)fb_size(fb);
    else return (uint64_t)-EINVAL;

    int64_t position = base + offset;
    if (position < 0) return (uint64_t)-EINVAL;

    file->offset = (uint64_t)position;
    return (uint64_t)position;
}

static void *fb_dev_mmap(vfs_file_t *file, void *addr, uint64_t len, int prot, int flags, uint64_t offset) {
    (void)flags;
    if (!file || len == 0) return NULL;

    process_t *process = get_current_process();
    struct limine_framebuffer *fb = fb_current();
    if (!process || !process->pml4 || !fb) return NULL;

    uint64_t size = fb_size(fb);
    uint64_t mapped_size = (size + 4095ULL) & ~4095ULL;
    uint64_t phys = fb_phys(fb);
    if (phys == 0 || offset > mapped_size || len > mapped_size - offset) return NULL;
    if ((offset & 4095ULL) != 0) return NULL;

    uint64_t phys_start = phys + offset;
    if ((phys_start & 4095ULL) != 0) return NULL;

    uint64_t aligned_len = (len + 4095ULL) & ~4095ULL;
    uint64_t virt;
    if (addr) {
        virt = (uint64_t)addr;
    } else {
        if (process->mmap_base == 0) process->mmap_base = 0x5000000000ULL;
        virt = process->mmap_base;
        process->mmap_base += aligned_len;
    }
    if ((virt & 4095ULL) != 0) return NULL;

    uint64_t page_flags = PTE_PRESENT | PTE_USER | PTE_PWT | PTE_HUGE;
    if (prot & FB_PROT_WRITE) page_flags |= PTE_WRITABLE;

    uint64_t pages = aligned_len / 4096;
    for (uint64_t i = 0; i < pages; i++) {
        vmm_map_page(process->pml4, phys_start + i * 4096, virt + i * 4096, page_flags);
    }

    fb_acquire_file(file);
    return (void *)virt;
}

static int fb_dev_close(vfs_file_t *file) {
    if (file && file->private_data == FB_FILE_ACQUIRED) {
        file->private_data = NULL;
        gconsole_release_framebuffer();
    }
    return 0;
}

static vfs_file_ops_t fb_file_ops = {
    .read = fb_dev_read,
    .write = fb_dev_write,
    .close = fb_dev_close,
    .lseek = fb_dev_lseek,
    .ioctl = fb_dev_ioctl,
    .mmap = fb_dev_mmap
};

void fbdev_init(void) {
    devfs_register_device("fb0", VFS_TYPE_CHAR_DEV, &fb_file_ops, NULL);
    devfs_register_device("fb/0", VFS_TYPE_CHAR_DEV, &fb_file_ops, NULL);
    devfs_register_device("dri/card0", VFS_TYPE_CHAR_DEV, &fb_file_ops, NULL);
    devfs_register_device("card0", VFS_TYPE_CHAR_DEV, &fb_file_ops, NULL);
}
