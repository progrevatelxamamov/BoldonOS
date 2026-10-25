#include "io.h"
#include "kernel.h"
#include <stdint.h>
#include <stdbool.h>
#include <bootloader.h>
#include <limine.h>
#include <gconsole.h>
#include <basefont.h>
#include <memory.h>
#include <scheduler/spinlock.h>
#include <scheduler/scheduler.h>
#include <mm/pmm.h>

static inline void fast_memcpy64(void *dst, const void *src, uint64_t bytes) {
    uint64_t q = bytes / 8;
    uint64_t r = bytes % 8;
    if (q) {
        __asm__ volatile (
            "cld\n\t"
            "rep movsq\n"
            : "+D"(dst), "+S"(src), "+c"(q)
            :
            : "memory"
        );
    }
    if (r) {
        __asm__ volatile (
            "cld\n\t"
            "rep movsb\n"
            : "+D"(dst), "+S"(src), "+c"(r)
            :
            : "memory"
        );
    }
}

typedef enum {
    ANSI_STATE_NORMAL,
    ANSI_STATE_ESC,
    ANSI_STATE_BRACKET
} ansi_state_t;

spinlock_t spik = SPINLOCK_INIT;

uint64_t cursor_x = 0;
uint64_t cursor_y = 0;

struct limine_framebuffer_response *base_framebuffer = NULL;
uint64_t base_framebuffer_index = 0;

rgb_t base_fg_color = {255, 255, 255};
rgb_t base_bg_color = {0, 0, 0};

static uint32_t *backbuffer = NULL;
static uint64_t bb_width = 0;
static uint64_t bb_height = 0;
static uint64_t bb_pitch_pixels = 0;
static bool backbuffer_ready = false;

static ansi_state_t g_ansi_state = ANSI_STATE_NORMAL;
static int g_ansi_args[4] = {0};
static int g_ansi_arg_idx = 0;
static bool g_ansi_invert = false;

#define CONSOLE_MAX_COLS 256
#define CONSOLE_MAX_ROWS 128

typedef struct {
    char c;
    rgb_t fg;
    rgb_t bg;
} console_cell_t;

static console_cell_t text_grid[CONSOLE_MAX_ROWS][CONSOLE_MAX_COLS];

static inline uint32_t fb_channel(uint8_t value, uint8_t mask_size, uint8_t mask_shift) {
    uint32_t max = (1U << mask_size) - 1;
    return ((uint32_t)value * max / 255) << mask_shift;
}

static inline uint32_t fb_pixel(struct limine_framebuffer *fb, rgb_t color) {
    return fb_channel(color.red, fb->red_mask_size, fb->red_mask_shift)
         | fb_channel(color.green, fb->green_mask_size, fb->green_mask_shift)
         | fb_channel(color.blue, fb->blue_mask_size, fb->blue_mask_shift);
}

void gconsole_init_backbuffer(void) {
    if (!base_framebuffer) return;
    struct limine_framebuffer *fb = base_framebuffer->framebuffers[base_framebuffer_index];
    bb_width = fb->width;
    bb_height = fb->height;
    bb_pitch_pixels = fb->pitch / 4;

    uint64_t total_bytes = bb_height * fb->pitch;
    uint64_t pages = (total_bytes + 4095) / 4096;
    void *phys = pmm_alloc_pages(pages);
    if (!phys) return;
    backbuffer = (uint32_t *)((uint64_t)phys + get_bootinfo().hhdm->offset);
    memset(backbuffer, 0, total_bytes);

    for (uint32_t r = 0; r < CONSOLE_MAX_ROWS; r++) {
        for (uint32_t c = 0; c < CONSOLE_MAX_COLS; c++) {
            text_grid[r][c].c = ' ';
            text_grid[r][c].fg = base_fg_color;
            text_grid[r][c].bg = base_bg_color;
        }
    }

    backbuffer_ready = true;
}

void gconsole_flush(void) {
    if (!backbuffer_ready || !base_framebuffer) return;
    struct limine_framebuffer *fb = base_framebuffer->framebuffers[base_framebuffer_index];
    uint64_t total_bytes = bb_height * fb->pitch;
    fast_memcpy64(fb->address, backbuffer, total_bytes);
}

void gconsole_flush_region(uint64_t y_start, uint64_t y_end) {
    if (!backbuffer_ready || !base_framebuffer) return;
    if (y_end > bb_height) y_end = bb_height;
    if (y_start >= y_end) return;
    struct limine_framebuffer *fb = base_framebuffer->framebuffers[base_framebuffer_index];
    uint64_t bytes_per_line = fb->pitch;
    uint64_t lines = y_end - y_start;
    uint8_t *dst = (uint8_t *)fb->address + y_start * bytes_per_line;
    uint8_t *src = (uint8_t *)backbuffer + y_start * bytes_per_line;
    fast_memcpy64(dst, src, bytes_per_line * lines);
}

static void scroll_grid(void) {
    if (!backbuffer_ready || !base_framebuffer) return;
    struct limine_framebuffer *fb = base_framebuffer->framebuffers[base_framebuffer_index];

    uint32_t max_cols = bb_width / 8;
    if (max_cols > CONSOLE_MAX_COLS) max_cols = CONSOLE_MAX_COLS;
    uint32_t max_rows = bb_height / 16;
    if (max_rows > CONSOLE_MAX_ROWS) max_rows = CONSOLE_MAX_ROWS;

    if (max_rows <= 1) return;

    memmove(&text_grid[0][0], &text_grid[1][0], (max_rows - 1) * CONSOLE_MAX_COLS * sizeof(console_cell_t));

    for (uint32_t c = 0; c < CONSOLE_MAX_COLS; c++) {
        text_grid[max_rows - 1][c].c = ' ';
        text_grid[max_rows - 1][c].fg = base_fg_color;
        text_grid[max_rows - 1][c].bg = base_bg_color;
    }

    size_t pitch_bytes = fb->pitch;
    size_t scroll_bytes = 16 * pitch_bytes;
    size_t total_bytes = bb_height * pitch_bytes;

    uint8_t *bb = (uint8_t *)backbuffer;
    memmove(bb, bb + scroll_bytes, total_bytes - scroll_bytes);

    uint32_t bg_raw = fb_pixel(fb, base_bg_color);
    uint32_t *bottom_pixels = (uint32_t *)(bb + (total_bytes - scroll_bytes));
    size_t bottom_pixel_count = scroll_bytes / 4;
    for (size_t i = 0; i < bottom_pixel_count; i++) {
        bottom_pixels[i] = bg_raw;
    }

    gconsole_flush();
}

void set_fg_color(rgb_t color) { base_fg_color = color; }
void set_bg_color(rgb_t color) { base_bg_color = color; }

void set_base_framebuffer(struct limine_framebuffer_response *fb) { base_framebuffer = fb; }
void set_base_index_framebuffer(uint64_t indx) { base_framebuffer_index = indx; }

struct limine_framebuffer_response* get_base_framebuffer(void) { return base_framebuffer; }
uint64_t get_base_index_framebuffer(void) { return base_framebuffer_index; }

void draw_pixel(struct limine_framebuffer_response *fb, uint64_t fbindex, rgb_t color, uint64_t x, uint64_t y) {
    if (!fb || fbindex >= fb->framebuffer_count) return;
    struct limine_framebuffer *f = fb->framebuffers[fbindex];
    if (x >= f->width || y >= f->height) return;

    uint32_t raw_color = fb_pixel(f, color);
    if (backbuffer_ready && backbuffer) {
        backbuffer[y * bb_pitch_pixels + x] = raw_color;
    } else {
        volatile uint32_t *fb_ptr = (volatile uint32_t *)f->address;
        fb_ptr[y * (f->pitch / 4) + x] = raw_color;
    }
}

void fill_rect(struct limine_framebuffer_response *fb, uint64_t fbindex, rgb_t color, uint64_t start_x, uint64_t start_y, uint64_t end_x, uint64_t end_y) {
    if (!fb || fbindex >= fb->framebuffer_count) return;
    struct limine_framebuffer *f = fb->framebuffers[fbindex];

    if (start_x >= f->width || start_y >= f->height) return;
    if (end_x > f->width) end_x = f->width;
    if (end_y > f->height) end_y = f->height;
    if (start_x >= end_x || start_y >= end_y) return;

    uint32_t raw_color = fb_pixel(f, color);
    uint64_t pitch_pixels = f->pitch / 4;

    if (backbuffer_ready && backbuffer) {
        for (uint64_t y = start_y; y < end_y; y++) {
            uint64_t line_offset = y * pitch_pixels;
            for (uint64_t x = start_x; x < end_x; x++) {
                backbuffer[line_offset + x] = raw_color;
            }
        }
    } else {
        volatile uint32_t *fb_ptr = (volatile uint32_t *)f->address;
        for (uint64_t y = start_y; y < end_y; y++) {
            uint64_t line_offset = y * pitch_pixels;
            for (uint64_t x = start_x; x < end_x; x++) {
                fb_ptr[line_offset + x] = raw_color;
            }
        }
    }
}

void draw_char(struct limine_framebuffer_response *fb_resp, uint64_t fbindex, uint64_t x, uint64_t y, char c, rgb_t fg_color, rgb_t bg_color) {
    if (!fb_resp || fbindex >= fb_resp->framebuffer_count) return;
    struct limine_framebuffer *fb = fb_resp->framebuffers[fbindex];

    unsigned char ch = (unsigned char)c;
    int offset = (int)ch * 16;
    
    uint32_t fg_raw = fb_pixel(fb, fg_color);
    uint32_t bg_raw = fb_pixel(fb, bg_color);
    uint64_t pitch_pixels = fb->pitch / 4;

    if (backbuffer_ready && backbuffer) {
        for (int row = 0; row < 16; row++) {
            unsigned char row_data = font_8x16[offset + row];
            uint64_t line_offset = (y + row) * pitch_pixels + x;

            for (int col = 0; col < 8; col++) {
                if (row_data & (0x80 >> col)) {
                    backbuffer[line_offset + col] = fg_raw;
                } else {
                    backbuffer[line_offset + col] = bg_raw;
                }
            }
        }
    } else {
        volatile uint32_t *fb_ptr = (volatile uint32_t *)fb->address;
        for (int row = 0; row < 16; row++) {
            unsigned char row_data = font_8x16[offset + row];
            uint64_t line_offset = (y + row) * pitch_pixels + x;

            for (int col = 0; col < 8; col++) {
                if (row_data & (0x80 >> col)) {
                    fb_ptr[line_offset + col] = fg_raw;
                } else {
                    fb_ptr[line_offset + col] = bg_raw;
                }
            }
        }
    }
}

static void console_print_char_unlocked(char c) {
    if (!base_framebuffer) return;
    struct limine_framebuffer *fb = base_framebuffer->framebuffers[base_framebuffer_index];
    outb(0x3F8, c);

    uint32_t max_cols = bb_width ? (bb_width / 8) : (fb->width / 8);
    uint32_t max_rows = bb_height ? (bb_height / 16) : (fb->height / 16);
    if (max_cols > CONSOLE_MAX_COLS) max_cols = CONSOLE_MAX_COLS;
    if (max_rows > CONSOLE_MAX_ROWS) max_rows = CONSOLE_MAX_ROWS;

    if (g_ansi_state == ANSI_STATE_NORMAL) {
        if (c == 0x1B /* ESC */) {
            g_ansi_state = ANSI_STATE_ESC;
            return;
        }

        if (c == '\n') {
            cursor_x = 0;
            cursor_y += 16;
            if (cursor_y >= max_rows * 16) {
                scroll_grid();
                cursor_y = (max_rows - 1) * 16;
            }
            return;
        }
        if (c == '\r') {
            cursor_x = 0;
            return;
        }
        if (c == '\b') {
            if (cursor_x >= 8) {
                cursor_x -= 8;
            } else if (cursor_y >= 16) {
                cursor_y -= 16;
                cursor_x = (max_cols - 1) * 8;
            }
            uint32_t col = cursor_x / 8;
            uint32_t row = cursor_y / 16;
            rgb_t cur_fg = g_ansi_invert ? base_bg_color : base_fg_color;
            rgb_t cur_bg = g_ansi_invert ? base_fg_color : base_bg_color;
            if (col < CONSOLE_MAX_COLS && row < CONSOLE_MAX_ROWS) {
                text_grid[row][col].c = ' ';
                text_grid[row][col].fg = cur_fg;
                text_grid[row][col].bg = cur_bg;
            }
            draw_char(base_framebuffer, base_framebuffer_index, cursor_x, cursor_y, ' ', cur_fg, cur_bg);
            gconsole_flush_region(cursor_y, cursor_y + 16);
            return;
        }

        if (cursor_x + 8 > max_cols * 8) {
            cursor_x = 0;
            cursor_y += 16;
        }

        if (cursor_y >= max_rows * 16) {
            scroll_grid();
            cursor_y = (max_rows - 1) * 16;
        }

        uint32_t col = cursor_x / 8;
        uint32_t row = cursor_y / 16;
        rgb_t cur_fg = g_ansi_invert ? base_bg_color : base_fg_color;
        rgb_t cur_bg = g_ansi_invert ? base_fg_color : base_bg_color;

        if (col < CONSOLE_MAX_COLS && row < CONSOLE_MAX_ROWS) {
            text_grid[row][col].c = c;
            text_grid[row][col].fg = cur_fg;
            text_grid[row][col].bg = cur_bg;
        }

        draw_char(base_framebuffer, base_framebuffer_index, cursor_x, cursor_y, c, cur_fg, cur_bg);
        gconsole_flush_region(cursor_y, cursor_y + 16);
        cursor_x += 8;
        return;
    }

    if (g_ansi_state == ANSI_STATE_ESC) {
        if (c == '[') {
            g_ansi_state = ANSI_STATE_BRACKET;
            g_ansi_arg_idx = 0;
            g_ansi_args[0] = 0;
            g_ansi_args[1] = 0;
            g_ansi_args[2] = 0;
            g_ansi_args[3] = 0;
        } else {
            g_ansi_state = ANSI_STATE_NORMAL;
        }
        return;
    }

    if (g_ansi_state == ANSI_STATE_BRACKET) {
        if (c >= '0' && c <= '9') {
            g_ansi_args[g_ansi_arg_idx] = g_ansi_args[g_ansi_arg_idx] * 10 + (c - '0');
            return;
        }
        if (c == ';' || c == '?') {
            if (g_ansi_arg_idx < 3) g_ansi_arg_idx++;
            return;
        }

        if (c == 'H' || c == 'f') {
            int row_idx = g_ansi_args[0] > 0 ? g_ansi_args[0] - 1 : 0;
            int col_idx = g_ansi_args[1] > 0 ? g_ansi_args[1] - 1 : 0;
            
            if (row_idx >= (int)max_rows) row_idx = max_rows - 1;
            if (col_idx >= (int)max_cols) col_idx = max_cols - 1;

            cursor_y = row_idx * 16;
            cursor_x = col_idx * 8;
        } 
        else if (c == 'J') {
            rgb_t cur_bg = g_ansi_invert ? base_fg_color : base_bg_color;
            fill_rect(base_framebuffer, base_framebuffer_index, cur_bg, 0, 0, max_cols * 8, max_rows * 16);
            for (uint32_t r = 0; r < max_rows && r < CONSOLE_MAX_ROWS; r++) {
                for (uint32_t cl = 0; cl < max_cols && cl < CONSOLE_MAX_COLS; cl++) {
                    text_grid[r][cl].c = ' ';
                    text_grid[r][cl].fg = base_fg_color;
                    text_grid[r][cl].bg = cur_bg;
                }
            }
            gconsole_flush();
        } 
        else if (c == 'K') {
            rgb_t cur_bg = g_ansi_invert ? base_fg_color : base_bg_color;
            fill_rect(base_framebuffer, base_framebuffer_index, cur_bg, cursor_x, cursor_y, max_cols * 8, cursor_y + 16);
            
            uint32_t start_col = cursor_x / 8;
            uint32_t row_idx = cursor_y / 16;
            if (row_idx < CONSOLE_MAX_ROWS) {
                for (uint32_t cl = start_col; cl < max_cols && cl < CONSOLE_MAX_COLS; cl++) {
                    text_grid[row_idx][cl].c = ' ';
                    text_grid[row_idx][cl].fg = base_fg_color;
                    text_grid[row_idx][cl].bg = cur_bg;
                }
            }
            gconsole_flush_region(cursor_y, cursor_y + 16);
        } 
        else if (c == 'm') {
            for (int i = 0; i <= g_ansi_arg_idx; i++) {
                int arg = g_ansi_args[i];
                if (arg == 7) {
                    g_ansi_invert = true;
                } else if (arg == 0 || arg == 39 || arg == 49 || arg == 27) {
                    g_ansi_invert = false;
                }
            }
        }

        g_ansi_state = ANSI_STATE_NORMAL;
    }
}

void console_print_char(char c) {
    uint64_t flags;
    spin_lock_irqsave(&spik, &flags);
    console_print_char_unlocked(c);
    spin_unlock_irqrestore(&spik, flags);
}

void console_print_string(const char *str) {
    if (!str) return;

    uint64_t flags;
    spin_lock_irqsave(&spik, &flags);
    while (*str) {
        console_print_char_unlocked(*str);
        str++;
    }
    spin_unlock_irqrestore(&spik, flags);
}

void console_print_hex(uint64_t n) {
    uint64_t flags;
    spin_lock_irqsave(&spik, &flags);
    const char *hex = "0123456789ABCDEF";
    console_print_char_unlocked('0');
    console_print_char_unlocked('x');
    for (int i = 60; i >= 0; i -= 4) {
        console_print_char_unlocked(hex[(n >> i) & 0xF]);
    }
    spin_unlock_irqrestore(&spik, flags);
}

void console_print_num(uint64_t n) {
    uint64_t flags;
    spin_lock_irqsave(&spik, &flags);
    if (n == 0) {
        console_print_char_unlocked('0');
        spin_unlock_irqrestore(&spik, flags);
        return;
    }
    char buf[21];
    int i = 20;
    buf[i--] = '\0';
    while (n > 0) {
        buf[i--] = (n % 10) + '0';
        n /= 10;
    }
    const char *ptr = &buf[i + 1];
    while (*ptr) {
        console_print_char_unlocked(*ptr);
        ptr++;
    }
    spin_unlock_irqrestore(&spik, flags);
}