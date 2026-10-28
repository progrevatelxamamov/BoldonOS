#include <sys/elf.h>
#include <fs/vfs.h>
#include <fs/pty.h>
#include <scheduler/scheduler.h>
#include <mm/malloc.h>
#include <memory.h>
#include <gconsole.h>
#include <inout/inout.h>
#include <disk/partitions.h>

static void init_no_root_hint(void) {
    console_print_string("\n============================================================\n");
    console_print_string("[INIT] root filesystem C: is still not mounted\n");
    console_print_string("[INIT] block devices: ");
    console_print_num(partitions_block_device_count());
    console_print_string("\n");
    console_print_string("[INIT] expected /bin/sh and /etc/init.sh on C: (ext4 or FAT32)\n");
    console_print_string("[INIT] check main.img is written to USB, osdata/ is not empty,\n");
    console_print_string("[INIT] and the kernel was rebuilt (make check-image)\n");
    console_print_string("[INIT] waiting for a disk, hotplug will retry automatically\n");
    console_print_string("============================================================\n\n");
}

static void pty_input_task(void) {
    while (1) {
        uint8_t code;
        bool down;

        /* Берём события из ЕДИНОГО авторитетного потока inout.
           Раньше здесь был опрос g_keyboard.down[] с локальной тенью
           prev_down[] — второй конечный автомат модификаторов. Если break
           терялся (переполнение, смена KDSKBMODE, g_keyboard уже испорчен
           PS/2-парсером), Nano-X навсегда оставался с залипшим Shift. */
        /* Не более пачки за проход: при автоповторе поток может быть
           бесконечным, и задача не должна монополизировать CPU. */
        int budget = 64;
        while (budget-- > 0 && inout_console_pop_event(&code, &down)) {
            int kbd_mode = pty_console_kbd_mode();
            if (kbd_mode == PTY_KBD_OFF) continue;

            if (kbd_mode == PTY_KBD_MEDIUMRAW || kbd_mode == PTY_KBD_RAW) {
                uint8_t bytes[2];
                uint8_t n = inout_console_scancode_bytes(code, down, bytes);
                for (uint8_t i = 0; i < n; i++)
                    pty_feed_scancode(bytes[i]);
                continue;
            }

            /* XLATE / UNICODE: символ выдаём только на НАЖАТИЕ.
               code имеет тип uint8_t, а KEY_MAX == 256 — проверка
               'code >= KEY_MAX' всегда ложна, она не нужна. */
            if (!down) continue;

            bool shift = g_keyboard.shift_pressed;
            bool caps  = g_keyboard.caps_lock;
            bool ctrl  = g_keyboard.down[KEY_RCTRL] || g_keyboard.down[KEY_LCTRL];

            if (code == KEY_ENTER)          pty_feed_input('\n');
            else if (code == KEY_BACKSPACE) pty_feed_input('\b');
            else if (code == KEY_TAB)       pty_feed_input('\t');
            else if (code == KEY_SPACE)     pty_feed_input(' ');
            else {
                char c = 0;
                if (keymap_caps[code]) {
                    /* Ровно та же формула, что и в key_to_ascii():
                       Shift XOR Caps для БУКВ, но не для ЦИФР. */
                    bool upper = shift ^ caps;
                    c = upper ? keymap_caps[code] : keymap_normal[code];
                } else if (shift && keymap_shift[code]) {
                    c = keymap_shift[code];
                } else {
                    c = keymap_normal[code];
                }

                if (c) {
                    // Ctrl+Буква (Ctrl+C = 0x03, Ctrl+D = 0x04, Ctrl+L = 0x0C, ...)
                    if (ctrl) {
                        if (c >= 'a' && c <= 'z') c = c - 'a' + 1;
                        else if (c >= 'A' && c <= 'Z') c = c - 'A' + 1;
                    }
                    pty_feed_input(c);
                }
            }
        }
        schedule_yield();
    }
}

void launch_init_process(void) {
    process_t *p = get_current_process();
    if (!p) return;

    if (!p->fd_table) {
        p->fd_table = malloc_alloc(sizeof(process_fd_table_t));
        if (!p->fd_table) return;
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

    if (!vfs_get_mount('C')) {
        init_no_root_hint();
        while (!vfs_get_mount('C')) schedule_yield();
        console_print_string("[INIT] root appeared, continuing boot\n");
    }

    console_print_string("[INIT] Starting /etc/init.sh...\n");
    sys_execve("/bin/sh", argv, envp);

    char *fallback_argv[] = { "/bin/sh", NULL };
    sys_execve("/bin/sh", fallback_argv, envp);

    console_print_string("[INIT] Failed to start shell!\n");
    console_print_string("[INIT] C: is mounted but /bin/sh or /etc/init.sh is missing\n");
    console_print_string("[INIT] check that osdata/bin/sh and osdata/etc/init.sh exist\n");
    while (1) {
        schedule_yield();
    }
}