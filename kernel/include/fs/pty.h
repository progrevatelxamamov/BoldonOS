#ifndef PTY_H
#define PTY_H

#include <fs/vfs.h>
#include <stdint.h>
#include <stdbool.h>

#define MAX_PTYS 16
#define PTY_BUF_SIZE 4096

#define PTY_KBD_RAW       0
#define PTY_KBD_XLATE     1
#define PTY_KBD_MEDIUMRAW 2
#define PTY_KBD_UNICODE   3
#define PTY_KBD_OFF       4
#define NCCS 19

#define IGNBRK  0000001
#define BRKINT  0000002
#define IGNPAR  0000004
#define PARMRK  0000010
#define INPCK   0000020
#define ISTRIP  0000040
#define INLCR   0000100
#define IGNCR   0000200
#define ICRNL   0000400
#define IUCLC   0001000
#define IXON    0002000
#define IXANY   0004000
#define IXOFF   0010000
#define IMAXBEL 0020000
#define IUTF8   0040000

#define OPOST   0000001
#define OLCUC   0000002
#define ONLCR   0000004
#define OCRNL   0000010
#define ONOCR   0000020
#define ONLRET  0000040

#define B38400  0000017
#define CS8     0000060
#define CREAD   0000200
#define PARENB  0000400
#define HUPCL   0002000
#define CLOCAL  0004000

#define ISIG    0000001
#define ICANON  0000002
#define ECHO    0000010
#define ECHOE   0000020
#define ECHOK   0000040
#define ECHONL  0000100
#define NOFLSH  0000200
#define TOSTOP  0000400
#define ECHOCTL 0001000
#define ECHOPRT 0002000
#define ECHOKE  0004000
#define IEXTEN  0100000

#define VINTR    0
#define VQUIT    1
#define VERASE   2
#define VKILL    3
#define VEOF     4
#define VTIME    5
#define VMIN     6
#define VSWTC    7
#define VSTART   8
#define VSTOP    9
#define VSUSP   10
#define VEOL    11

struct termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[NCCS];
    uint32_t c_ispeed;
    uint32_t c_ospeed;
} __attribute__((packed));

struct winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

typedef struct pty_ring {
    uint8_t buf[PTY_BUF_SIZE];
    uint32_t head;
    uint32_t tail;
} pty_ring_t;

typedef struct pty {
    int id;
    bool used;
    bool dynamic;
    bool master_open;
    bool slave_open;
    int master_refs;
    int slave_refs;
    vfs_node_t *master_node;
    vfs_node_t *slave_node;
    pty_ring_t in;
    pty_ring_t out;
    
    uint16_t winsize_row;
    uint16_t winsize_col;
    int kbd_mode;
    struct termios termios;
} pty_t;

void pty_init(void);
int pty_alloc(int *master_fd, int *slave_fd, process_fd_table_t *table);
void pty_feed_input(char c);
void pty_feed_scancode(uint8_t byte);
int pty_console_kbd_mode(void);
void pty_register_devfs(void);
int pty_poll_events(vfs_file_t *file, short events);
void pty_set_size(int pty_id, uint16_t rows, uint16_t cols);

#endif