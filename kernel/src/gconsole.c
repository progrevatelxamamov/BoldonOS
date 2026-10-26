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
    ANSI_STATE_BRACKET,
    ANSI_STATE_STRING,
    ANSI_STATE_STRING_ESC
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
static volatile uint32_t framebuffer_users = 0;
static process_t *framebuffer_owner = NULL;

static ansi_state_t g_ansi_state = ANSI_STATE_NORMAL;
static int g_ansi_args[16] = {0};
static int g_ansi_arg_idx = 0;
static bool g_ansi_invert = false;
static bool g_ansi_private = false;
static bool g_ansi_bold = false;
static int g_ansi_fg_palette = -1;
static rgb_t g_current_fg_color = {255, 255, 255};
static rgb_t g_current_bg_color = {0, 0, 0};

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

static rgb_t ansi_palette_color(int index, bool bright) {
    static const rgb_t normal_colors[8] = {
        {0, 0, 0}, {170, 0, 0}, {0, 170, 0}, {170, 85, 0},
        {0, 0, 170}, {170, 0, 170}, {0, 170, 170}, {170, 170, 170}
    };
    static const rgb_t bright_colors[8] = {
        {85, 85, 85}, {255, 85, 85}, {85, 255, 85}, {255, 255, 85},
        {85, 85, 255}, {255, 85, 255}, {85, 255, 255}, {255, 255, 255}
    };
    if (index < 0) index = 0;
    if (index > 7) index = 7;
    return bright ? bright_colors[index] : normal_colors[index];
}

static uint8_t ansi_component(int value) {
    if (value < 0) return 0;
    if (value > 255) return 255;
    return (uint8_t)value;
}

static rgb_t ansi_256_color(int index) {
    static const uint8_t levels[6] = {0, 95, 135, 175, 215, 255};
    if (index < 0) return ansi_palette_color(0, false);
    if (index < 8) return ansi_palette_color(index, false);
    if (index < 16) return ansi_palette_color(index - 8, true);
    if (index < 232) {
        int value = index - 16;
        rgb_t color = {levels[value / 36], levels[(value / 6) % 6], levels[value % 6]};
        return color;
    }
    if (index > 255) index = 255;
    uint8_t gray = ansi_component(8 + (index - 232) * 10);
    rgb_t color = {gray, gray, gray};
    return color;
}

static rgb_t ansi_effective_fg(void) {
    return g_ansi_invert ? g_current_bg_color : g_current_fg_color;
}

static rgb_t ansi_effective_bg(void) {
    return g_ansi_invert ? g_current_fg_color : g_current_bg_color;
}

static void ansi_reset_graphics(void) {
    g_current_fg_color = base_fg_color;
    g_current_bg_color = base_bg_color;
    g_ansi_bold = false;
    g_ansi_invert = false;
    g_ansi_fg_palette = -1;
}

static void ansi_apply_sgr(void) {
    for (int i = 0; i <= g_ansi_arg_idx; i++) {
        int arg = g_ansi_args[i];
        if (arg == 0) {
            ansi_reset_graphics();
        } else if (arg == 1) {
            g_ansi_bold = true;
            if (g_ansi_fg_palette >= 0 && g_ansi_fg_palette < 8)
                g_current_fg_color = ansi_palette_color(g_ansi_fg_palette, true);
        } else if (arg == 22) {
            g_ansi_bold = false;
            if (g_ansi_fg_palette >= 0 && g_ansi_fg_palette < 8)
                g_current_fg_color = ansi_palette_color(g_ansi_fg_palette, false);
        } else if (arg == 7) {
            g_ansi_invert = true;
        } else if (arg == 27) {
            g_ansi_invert = false;
        } else if (arg >= 30 && arg <= 37) {
            g_ansi_fg_palette = arg - 30;
            g_current_fg_color = ansi_palette_color(g_ansi_fg_palette, g_ansi_bold);
        } else if (arg == 39) {
            g_current_fg_color = base_fg_color;
            g_ansi_fg_palette = -1;
        } else if (arg >= 40 && arg <= 47) {
            g_current_bg_color = ansi_palette_color(arg - 40, false);
        } else if (arg == 49) {
            g_current_bg_color = base_bg_color;
        } else if (arg >= 90 && arg <= 97) {
            g_ansi_fg_palette = 8 + arg - 90;
            g_current_fg_color = ansi_palette_color(arg - 90, true);
        } else if (arg >= 100 && arg <= 107) {
            g_current_bg_color = ansi_palette_color(arg - 100, true);
        } else if (arg == 38 || arg == 48) {
            if (i + 1 > g_ansi_arg_idx) break;
            int mode = g_ansi_args[++i];
            rgb_t color = arg == 38 ? g_current_fg_color : g_current_bg_color;
            bool valid = false;
            if (mode == 5) {
                if (i + 1 > g_ansi_arg_idx) break;
                color = ansi_256_color(g_ansi_args[++i]);
                valid = true;
            } else if (mode == 2) {
                if (i + 3 > g_ansi_arg_idx) break;
                color.red = ansi_component(g_ansi_args[i + 1]);
                color.green = ansi_component(g_ansi_args[i + 2]);
                color.blue = ansi_component(g_ansi_args[i + 3]);
                i += 3;
                valid = true;
            }
            if (valid && arg == 38) {
                g_current_fg_color = color;
                g_ansi_fg_palette = -1;
            } else if (valid) {
                g_current_bg_color = color;
            }
        }
    }
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

void gconsole_acquire_framebuffer(void) {
    if (__sync_fetch_and_add(&framebuffer_users, 1) == 0)
        framebuffer_owner = get_current_process();
}

void gconsole_release_framebuffer(void) {
    uint32_t users = framebuffer_users;
    while (users != 0) {
        if (__sync_bool_compare_and_swap(&framebuffer_users, users, users - 1)) {
            if (users == 1) {
                framebuffer_owner = NULL;
                gconsole_flush();
            }
            return;
        }
        users = framebuffer_users;
    }
}

bool gconsole_framebuffer_acquired(void) {
    return framebuffer_users != 0;
}

process_t *gconsole_framebuffer_owner(void) {
    return framebuffer_owner;
}

void gconsole_flush(void) {
    if (framebuffer_users != 0) return;
    if (!backbuffer_ready || !base_framebuffer) return;
    struct limine_framebuffer *fb = base_framebuffer->framebuffers[base_framebuffer_index];
    uint64_t total_bytes = bb_height * fb->pitch;
    fast_memcpy64(fb->address, backbuffer, total_bytes);
}

void gconsole_flush_region(uint64_t y_start, uint64_t y_end) {
    if (framebuffer_users != 0) return;
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
        text_grid[max_rows - 1][c].fg = ansi_effective_fg();
        text_grid[max_rows - 1][c].bg = ansi_effective_bg();
    }

    size_t pitch_bytes = fb->pitch;
    size_t scroll_bytes = 16 * pitch_bytes;
    size_t total_bytes = bb_height * pitch_bytes;

    uint8_t *bb = (uint8_t *)backbuffer;
    memmove(bb, bb + scroll_bytes, total_bytes - scroll_bytes);

    uint32_t bg_raw = fb_pixel(fb, ansi_effective_bg());
    uint32_t *bottom_pixels = (uint32_t *)(bb + (total_bytes - scroll_bytes));
    size_t bottom_pixel_count = scroll_bytes / 4;
    for (size_t i = 0; i < bottom_pixel_count; i++) {
        bottom_pixels[i] = bg_raw;
    }

    gconsole_flush();
}

void set_fg_color(rgb_t color) {
    base_fg_color = color;
    g_current_fg_color = color;
    g_ansi_fg_palette = -1;
}

void set_bg_color(rgb_t color) {
    base_bg_color = color;
    g_current_bg_color = color;
}

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
    } else if (framebuffer_users == 0) {
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
    } else if (framebuffer_users == 0) {
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
    } else if (framebuffer_users == 0) {
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
        if (c == '\a') {
            return;
        }
        if (c == '\t') {
            uint32_t col = cursor_x / 8;
            uint32_t next_col = ((col / 8) + 1) * 8;
            if (next_col >= max_cols) next_col = max_cols ? max_cols - 1 : 0;
            cursor_x = next_col * 8;
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
            rgb_t cur_fg = ansi_effective_fg();
            rgb_t cur_bg = ansi_effective_bg();
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
        rgb_t cur_fg = ansi_effective_fg();
        rgb_t cur_bg = ansi_effective_bg();

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

    if (g_ansi_state == ANSI_STATE_STRING) {
        if (c == '\a') g_ansi_state = ANSI_STATE_NORMAL;
        else if (c == 0x1B) g_ansi_state = ANSI_STATE_STRING_ESC;
        return;
    }

    if (g_ansi_state == ANSI_STATE_STRING_ESC) {
        g_ansi_state = c == '\\' ? ANSI_STATE_NORMAL : ANSI_STATE_STRING;
        return;
    }

    if (g_ansi_state == ANSI_STATE_ESC) {
        if (c == '[') {
            g_ansi_state = ANSI_STATE_BRACKET;
            g_ansi_arg_idx = 0;
            g_ansi_private = false;
            for (int i = 0; i < 16; i++) g_ansi_args[i] = 0;
        } else if (c == ']' || c == 'P' || c == 'X' || c == '^' || c == '_') {
            g_ansi_state = ANSI_STATE_STRING;
        } else if (c >= ' ' && c <= '/') {
            return;
        } else {
            g_ansi_state = ANSI_STATE_NORMAL;
        }
        return;
    }

    if (g_ansi_state == ANSI_STATE_BRACKET) {
        if (c >= '0' && c <= '9') {
            if (g_ansi_args[g_ansi_arg_idx] < 100000)
                g_ansi_args[g_ansi_arg_idx] = g_ansi_args[g_ansi_arg_idx] * 10 + (c - '0');
            return;
        }
        if (c == ';' || c == ':') {
            if (g_ansi_arg_idx < 15) {
                g_ansi_arg_idx++;
                g_ansi_args[g_ansi_arg_idx] = 0;
            }
            return;
        }
        if (c == '?') {
            g_ansi_private = true;
            return;
        }
        if (c >= '<' && c <= '?') {
            g_ansi_private = true;
            return;
        }
        if (c >= ' ' && c <= '/') {
            return;
        }

        if (!g_ansi_private && (c == 'H' || c == 'f')) {
            int row_idx = g_ansi_args[0] > 0 ? g_ansi_args[0] - 1 : 0;
            int col_idx = g_ansi_args[1] > 0 ? g_ansi_args[1] - 1 : 0;
            if (max_rows && row_idx >= (int)max_rows) row_idx = (int)max_rows - 1;
            if (max_cols && col_idx >= (int)max_cols) col_idx = (int)max_cols - 1;
            if (row_idx < 0) row_idx = 0;
            if (col_idx < 0) col_idx = 0;
            cursor_y = (uint64_t)row_idx * 16;
            cursor_x = (uint64_t)col_idx * 8;
        } else if (!g_ansi_private && (c == 'A' || c == 'B' || c == 'C' || c == 'D' ||
                                      c == 'E' || c == 'F' || c == 'G' || c == 'd')) {
            int count = g_ansi_args[0] > 0 ? g_ansi_args[0] : 1;
            if (c == 'A') {
                uint64_t delta = (uint64_t)count * 16;
                cursor_y = cursor_y > delta ? cursor_y - delta : 0;
            } else if (c == 'B') {
                cursor_y += (uint64_t)count * 16;
                if (max_rows && cursor_y >= (uint64_t)max_rows * 16)
                    cursor_y = (uint64_t)(max_rows - 1) * 16;
            } else if (c == 'C') {
                cursor_x += (uint64_t)count * 8;
                if (max_cols && cursor_x >= (uint64_t)max_cols * 8)
                    cursor_x = (uint64_t)(max_cols - 1) * 8;
            } else if (c == 'D') {
                uint64_t delta = (uint64_t)count * 8;
                cursor_x = cursor_x > delta ? cursor_x - delta : 0;
            } else if (c == 'E') {
                cursor_y += (uint64_t)count * 16;
                if (max_rows && cursor_y >= (uint64_t)max_rows * 16)
                    cursor_y = (uint64_t)(max_rows - 1) * 16;
                cursor_x = 0;
            } else if (c == 'F') {
                uint64_t delta = (uint64_t)count * 16;
                cursor_y = cursor_y > delta ? cursor_y - delta : 0;
                cursor_x = 0;
            } else if (c == 'G') {
                int col_idx = count - 1;
                if (max_cols && col_idx >= (int)max_cols) col_idx = (int)max_cols - 1;
                cursor_x = (uint64_t)col_idx * 8;
            } else {
                int row_idx = count - 1;
                if (max_rows && row_idx >= (int)max_rows) row_idx = (int)max_rows - 1;
                cursor_y = (uint64_t)row_idx * 16;
            }
        } else if (!g_ansi_private && c == 'J') {
            rgb_t cur_fg = ansi_effective_fg();
            rgb_t cur_bg = ansi_effective_bg();
            int mode = g_ansi_args[0];
            uint32_t row_idx = max_rows ? (uint32_t)(cursor_y / 16) : 0;
            uint32_t col_idx = max_cols ? (uint32_t)(cursor_x / 8) : 0;
            if (row_idx >= max_rows) row_idx = max_rows ? max_rows - 1 : 0;
            if (col_idx >= max_cols) col_idx = max_cols ? max_cols - 1 : 0;
            if (mode == 0) {
                fill_rect(base_framebuffer, base_framebuffer_index, cur_bg, cursor_x, cursor_y, max_cols * 8, cursor_y + 16);
                if (row_idx + 1 < max_rows)
                    fill_rect(base_framebuffer, base_framebuffer_index, cur_bg, 0, cursor_y + 16, max_cols * 8, max_rows * 16);
                for (uint32_t cl = col_idx; cl < max_cols && cl < CONSOLE_MAX_COLS; cl++) {
                    text_grid[row_idx][cl].c = ' ';
                    text_grid[row_idx][cl].fg = cur_fg;
                    text_grid[row_idx][cl].bg = cur_bg;
                }
                for (uint32_t r = row_idx + 1; r < max_rows && r < CONSOLE_MAX_ROWS; r++) {
                    for (uint32_t cl = 0; cl < max_cols && cl < CONSOLE_MAX_COLS; cl++) {
                        text_grid[r][cl].c = ' ';
                        text_grid[r][cl].fg = cur_fg;
                        text_grid[r][cl].bg = cur_bg;
                    }
                }
            } else if (mode == 1) {
                fill_rect(base_framebuffer, base_framebuffer_index, cur_bg, 0, cursor_y, cursor_x + 8, cursor_y + 16);
                if (row_idx > 0)
                    fill_rect(base_framebuffer, base_framebuffer_index, cur_bg, 0, 0, max_cols * 8, cursor_y);
                for (uint32_t r = 0; r < row_idx && r < CONSOLE_MAX_ROWS; r++) {
                    for (uint32_t cl = 0; cl < max_cols && cl < CONSOLE_MAX_COLS; cl++) {
                        text_grid[r][cl].c = ' ';
                        text_grid[r][cl].fg = cur_fg;
                        text_grid[r][cl].bg = cur_bg;
                    }
                }
                for (uint32_t cl = 0; cl <= col_idx && cl < max_cols && cl < CONSOLE_MAX_COLS; cl++) {
                    text_grid[row_idx][cl].c = ' ';
                    text_grid[row_idx][cl].fg = cur_fg;
                    text_grid[row_idx][cl].bg = cur_bg;
                }
            } else if (mode == 2 || mode == 3) {
                fill_rect(base_framebuffer, base_framebuffer_index, cur_bg, 0, 0, max_cols * 8, max_rows * 16);
                for (uint32_t r = 0; r < max_rows && r < CONSOLE_MAX_ROWS; r++) {
                    for (uint32_t cl = 0; cl < max_cols && cl < CONSOLE_MAX_COLS; cl++) {
                        text_grid[r][cl].c = ' ';
                        text_grid[r][cl].fg = cur_fg;
                        text_grid[r][cl].bg = cur_bg;
                    }
                }
            }
            gconsole_flush();
        } else if (!g_ansi_private && c == 'K') {
            rgb_t cur_fg = ansi_effective_fg();
            rgb_t cur_bg = ansi_effective_bg();
            int mode = g_ansi_args[0];
            uint32_t start_col = max_cols ? (uint32_t)(cursor_x / 8) : 0;
            uint32_t end_col = start_col;
            uint32_t row_idx = max_rows ? (uint32_t)(cursor_y / 16) : 0;
            if (mode == 0) {
                end_col = max_cols;
                fill_rect(base_framebuffer, base_framebuffer_index, cur_bg, cursor_x, cursor_y, max_cols * 8, cursor_y + 16);
            } else if (mode == 1) {
                start_col = 0;
                end_col = (max_cols ? (uint32_t)(cursor_x / 8) : 0) + 1;
                fill_rect(base_framebuffer, base_framebuffer_index, cur_bg, 0, cursor_y, cursor_x + 8, cursor_y + 16);
            } else if (mode == 2) {
                start_col = 0;
                end_col = max_cols;
                fill_rect(base_framebuffer, base_framebuffer_index, cur_bg, 0, cursor_y, max_cols * 8, cursor_y + 16);
            }
            if ((mode == 0 || mode == 1 || mode == 2) && row_idx < CONSOLE_MAX_ROWS) {
                for (uint32_t cl = start_col; cl < end_col && cl < max_cols && cl < CONSOLE_MAX_COLS; cl++) {
                    text_grid[row_idx][cl].c = ' ';
                    text_grid[row_idx][cl].fg = cur_fg;
                    text_grid[row_idx][cl].bg = cur_bg;
                }
            }
            gconsole_flush_region(cursor_y, cursor_y + 16);
        } else if (!g_ansi_private && c == 'X') {
            rgb_t cur_fg = ansi_effective_fg();
            rgb_t cur_bg = ansi_effective_bg();
            int count = g_ansi_args[0] > 0 ? g_ansi_args[0] : 1;
            uint32_t start_col = max_cols ? (uint32_t)(cursor_x / 8) : 0;
            uint32_t end_col = start_col + (uint32_t)count;
            if (end_col > max_cols) end_col = max_cols;
            uint32_t row_idx = max_rows ? (uint32_t)(cursor_y / 16) : 0;
            fill_rect(base_framebuffer, base_framebuffer_index, cur_bg, cursor_x, cursor_y, end_col * 8, cursor_y + 16);
            if (row_idx < CONSOLE_MAX_ROWS) {
                for (uint32_t cl = start_col; cl < end_col && cl < CONSOLE_MAX_COLS; cl++) {
                    text_grid[row_idx][cl].c = ' ';
                    text_grid[row_idx][cl].fg = cur_fg;
                    text_grid[row_idx][cl].bg = cur_bg;
                }
            }
            gconsole_flush_region(cursor_y, cursor_y + 16);
        } else if (!g_ansi_private && c == 'm') {
            ansi_apply_sgr();
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