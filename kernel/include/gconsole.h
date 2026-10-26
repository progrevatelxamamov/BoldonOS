#ifndef GCONSOLE_H
#define GCONSOLE_H

#include <stdint.h>
#include <stdbool.h>
#include <limine.h>

typedef struct rgb_struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} rgb_t;

void set_base_framebuffer(struct limine_framebuffer_response *fb);
void set_base_index_framebuffer(uint64_t indx);
struct limine_framebuffer_response* get_base_framebuffer(void);
uint64_t get_base_index_framebuffer(void);

void set_fg_color(rgb_t color);
void set_bg_color(rgb_t color);

void draw_pixel(struct limine_framebuffer_response *fb, uint64_t fbindex, rgb_t color, uint64_t x, uint64_t y);
void fill_rect(struct limine_framebuffer_response *fb, uint64_t fbindex, rgb_t color, uint64_t start_x, uint64_t start_y, uint64_t end_x, uint64_t end_y);

void draw_char(struct limine_framebuffer_response *fb_resp, uint64_t fbindex, uint64_t x, uint64_t y, char c, rgb_t fg_color, rgb_t bg_color);

void console_print_char(char c);
void console_print_string(const char *str);
void console_print_hex(uint64_t n);
void console_print_num(uint64_t n);

void gconsole_init_backbuffer(void);
void gconsole_flush(void);
void gconsole_flush_region(uint64_t y_start, uint64_t y_end);
void gconsole_acquire_framebuffer(void);
void gconsole_release_framebuffer(void);
bool gconsole_framebuffer_acquired(void);

struct process;
struct process *gconsole_framebuffer_owner(void);

#endif