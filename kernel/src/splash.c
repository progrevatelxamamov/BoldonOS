#include <stdint.h>
#include <stddef.h>
#include <bootloader.h>
#include <gconsole.h>
#include <memory.h>

#pragma pack(push, 1)
typedef struct {
    uint16_t signature;
    uint32_t file_size;
    uint32_t reserved;
    uint32_t data_offset;
    uint32_t dib_header_size;
    int32_t  width;
    int32_t  height;
    uint16_t planes;
    uint16_t bpp;
    uint32_t compression;
} bmp_header_t;
#pragma pack(pop)

void draw_splash_screen(bootloader_t *blinfo) {
    if (!blinfo->modules || blinfo->modules->module_count == 0) {
        console_print_string("[SPLASH] No modules loaded (splash.bmp missing?)\n");
        return;
    }

    struct limine_file *splash_file = NULL;
    for (uint64_t i = 0; i < blinfo->modules->module_count; i++) {
        struct limine_file *f = blinfo->modules->modules[i];
        char *path = f->path;
        while (*path) {
            if (path[0] == '.' && path[1] == 'b' && path[2] == 'm' && path[3] == 'p') {
                splash_file = f;
                break;
            }
            path++;
        }
        if (splash_file) break;
    }

    if (!splash_file) {
        console_print_string("[SPLASH] splash.bmp not found in modules\n");
        return;
    }

    bmp_header_t *bmp = (bmp_header_t *)splash_file->address;
    if (bmp->signature != 0x4D42) {
        console_print_string("[SPLASH] Invalid BMP signature\n");
        return;
    }
    if (bmp->bpp != 24 && bmp->bpp != 32) {
        console_print_string("[SPLASH] Only 24-bit or 32-bit BMP supported\n");
        return;
    }
    if (bmp->compression != 0) {
        console_print_string("[SPLASH] Compressed BMP not supported\n");
        return;
    }

    uint8_t *pixel_data = (uint8_t *)splash_file->address + bmp->data_offset;
    int bmp_w = bmp->width;
    int bmp_h = bmp->height;
    if (bmp_h < 0) bmp_h = -bmp_h;

    struct limine_framebuffer *fb = get_base_framebuffer()->framebuffers[get_base_index_framebuffer()];
    uint64_t screen_w = fb->width;
    uint64_t screen_h = fb->height;

    for (uint64_t y = 0; y < screen_h; y++) {
        for (uint64_t x = 0; x < screen_w; x++) {
            int src_x = (x * bmp_w) / screen_w;
            int src_y = (y * bmp_h) / screen_h;
            
            if (bmp->height > 0) {
                src_y = bmp_h - 1 - src_y;
            }

            int pixel_index = (src_y * bmp_w + src_x) * (bmp->bpp / 8);
            
            rgb_t color;
            color.blue  = pixel_data[pixel_index];
            color.green = pixel_data[pixel_index + 1];
            color.red   = pixel_data[pixel_index + 2];

            draw_pixel(get_base_framebuffer(), get_base_index_framebuffer(), color, x, y);
        }
    }
    gconsole_flush();
}