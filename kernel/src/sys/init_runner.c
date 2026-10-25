#include <sys/elf.h>
#include <fs/vfs.h>
#include <fs/pty.h>
#include <scheduler/scheduler.h>
#include <mm/malloc.h>
#include <memory.h>
#include <gconsole.h>
#include <inout/inout.h>

static void pty_input_task(void) {
    static bool prev_down[KEY_MAX];
    memset(prev_down, 0, sizeof(prev_down));

    while (1) {
        for (int k = 0; k < KEY_MAX; k++) {
            bool now = g_keyboard.down[k];
            if (now && !prev_down[k]) {
                bool shift = g_keyboard.shift_pressed;
                bool caps  = g_keyboard.caps_lock;
                bool ctrl  = g_keyboard.down[KEY_RCTRL] || g_keyboard.down[KEY_LCTRL];

                if (k == KEY_ENTER)          pty_feed_input('\n');
                else if (k == KEY_BACKSPACE) pty_feed_input('\b');
                else if (k == KEY_TAB)       pty_feed_input('\t');
                else if (k == KEY_SPACE)     pty_feed_input(' ');
                else {
                    char c = 0;
                    if (keymap_caps[k]) {
                        bool upper = shift ^ caps;
                        c = upper ? keymap_caps[k] : keymap_normal[k];
                    } else if (shift && keymap_shift[k]) {
                        c = keymap_shift[k];
                    } else {
                        c = keymap_normal[k];
                    }

                    if (c) {
                        // Обработка Ctrl+Буква (Ctrl+C = 0x03, Ctrl+D = 0x04, Ctrl+L = 0x0C, и т.д.)
                        if (ctrl) {
                            if (c >= 'a' && c <= 'z') c = c - 'a' + 1;
                            else if (c >= 'A' && c <= 'Z') c = c - 'A' + 1;
                        }
                        pty_feed_input(c);
                    }
                }
            }
            prev_down[k] = now;
        }
        schedule_yield();
    }
}

void launch_init_process(void) {
    process_t *p = get_current_process();
    if (!p) return;

    if (!p->fd_table) {
        p->fd_table = malloc_alloc(sizeof(process_fd_table_t));
        memset(p->fd_table, 0, sizeof(process_fd_table_t));
    }

    int master_fd = -1, slave_fd = -1;
    if (pty_alloc(&master_fd, &slave_fd, p->fd_table) != 0) {
        console_print_string("[INIT] pty_alloc failed\n");
        return;
    }

    vfs_file_t *master_file = p->fd_table->fds[master_fd];
    vfs_file_t *slave_file  = p->fd_table->fds[slave_fd];

    p->fd_table->fds[0] = slave_file;
    p->fd_table->fds[1] = slave_file;
    p->fd_table->fds[2] = slave_file;
    p->fd_table->fds[3] = master_file;

    slave_file->refcount = 3;
    master_file->refcount = 1;

    create_task_kernel((void *)pty_input_task, "pty-input");

    /* Переменные окружения для старта init-скрипта */
    char *argv[] = { "/bin/sh", "/etc/init.sh", NULL };
    char *envp[] = {
        "PATH=/bin:/usr/bin:/sbin:/usr/sbin",
        "HOME=/root",
        "TERM=linux",
        "SHELL=/bin/sh",
        NULL
    };

    console_print_string("[INIT] Starting /etc/init.sh...\n");
    sys_execve("/bin/sh", argv, envp);

    /* Если /etc/init.sh не найден, пробуем запустить просто sh */
    char *fallback_argv[] = { "/bin/sh", NULL };
    sys_execve("/bin/sh", fallback_argv, envp);

    console_print_string("[INIT] Failed to start shell!\n");
    while (1) {
        schedule_yield();
    }
}