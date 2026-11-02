#include <gui/gui.h>
#include <gconsole.h>
#include <limine.h>
#include <scheduler/scheduler.h>
#include <inout/inout.h>

rgb_t color_cursor_base = {255, 255, 255};
rgb_t color_cursor_pressed = {255, 0, 0};
rgb_t color_black = {0, 0, 0};

int old_x = 0;
int old_y = 0;
bool old_pressed = false;

void gui_server(void) {
    rgb_t color_cursor = color_cursor_base;

    struct limine_framebuffer_response *fb = get_base_framebuffer();
    uint64_t max_w = (fb && fb->framebuffer_count > 0) ? fb->framebuffers[0]->width : 1024;
    uint64_t max_h = (fb && fb->framebuffer_count > 0) ? fb->framebuffers[0]->height : 768;

    fill_rect(get_base_framebuffer(), 0, color_cursor, old_x, old_y, old_x + 25, old_y + 25);
    gconsole_flush_region(old_y, old_y + 25);

    while (1) {
        if (gconsole_framebuffer_acquired()) {
            schedule_yield();
            continue;
        }

        int current_x = g_mouse.x;
        int current_y = g_mouse.y;

        if (current_x < 0) current_x = 0;
        if (current_y < 0) current_y = 0;
        if (current_x > (int)max_w - 25) current_x = max_w - 25;
        if (current_y > (int)max_h - 25) current_y = max_h - 25;
        g_mouse.x = current_x;
        g_mouse.y = current_y;

        bool pressed = g_mouse.leftbtn_pressed;
        bool need_redraw = (current_x != old_x) || (current_y != old_y) || (pressed != old_pressed);

        if (need_redraw) {
            color_cursor = pressed ? color_cursor_pressed : color_cursor_base;

            fill_rect(get_base_framebuffer(), 0, color_black, old_x, old_y, old_x + 25, old_y + 25);
            fill_rect(get_base_framebuffer(), 0, color_cursor, current_x, current_y, current_x + 25, current_y + 25);

            int min_y = (old_y < current_y) ? old_y : current_y;
            int max_y = ((old_y > current_y) ? old_y : current_y) + 25;
            gconsole_flush_region(min_y, max_y);

            old_x = current_x;
            old_y = current_y;
            old_pressed = pressed;
        }

        schedule_yield();
    }
}

void gui_init(void) {
    create_task_kernel((void*)gui_server, "gui_server");
}