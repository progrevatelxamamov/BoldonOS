#include <net/unix_socket.h>
#include <net/socket.h>
#include <fs/vfs.h>
#include <errno.h>
#include <mm/malloc.h>
#include <memory.h>
#include <scheduler/scheduler.h>
#include <string.h>

#define SOL_SOCKET 1
#define SO_DEBUG   1
#define SO_REUSEADDR 2
#define SO_TYPE    3
#define SO_ERROR   4
#define SO_DONTROUTE 5
#define SO_BROADCAST 6
#define SO_SNDBUF  7
#define SO_RCVBUF  8

typedef struct unix_socket_internal {
    bool in_use;
    bool bound;
    bool path_node_created;
    bool peer_closed;
    bool listening;
    struct sockaddr_un addr;
    uint8_t rx_buffer[8192];
    uint32_t rx_head;
    uint32_t rx_tail;

    struct unix_socket_internal *peer;
    struct unix_socket_internal *accept_queue[16];
    int accept_head;
    int accept_tail;
} unix_intern_t;

#define MAX_UNIX 128
static unix_intern_t unix_pool[MAX_UNIX];

static bool unix_create_path_node(const char *path) {
    vfs_file_t *file = vfs_open(path, VFS_O_RDWR | VFS_O_CREAT);
    if (!file) return false;
    vfs_close(file);
    return true;
}

static int un_bind(socket_t *sock, const struct sockaddr *addr, socklen_t addrlen) {
    unix_intern_t *u = (unix_intern_t *)sock->private_data;
    if (!u) return -EINVAL;
    if (u->bound) return -EINVAL;
    if (!addr || addrlen < sizeof(sa_family_t)) return -EINVAL;

    memcpy(&u->addr, addr, addrlen < sizeof(struct sockaddr_un) ? addrlen : sizeof(struct sockaddr_un));
    u->addr.sun_path[sizeof(u->addr.sun_path) - 1] = '\0';

    if (u->addr.sun_path[0] != '\0') {
        for (int i = 0; i < MAX_UNIX; i++) {
            if (&unix_pool[i] != u && unix_pool[i].in_use && unix_pool[i].bound &&
                strcmp(unix_pool[i].addr.sun_path, u->addr.sun_path) == 0) {
                memset(&u->addr, 0, sizeof(u->addr));
                return -EADDRINUSE;
            }
        }

        if (vfs_resolve_path(u->addr.sun_path)) {
            memset(&u->addr, 0, sizeof(u->addr));
            return -EADDRINUSE;
        }

        u->path_node_created = unix_create_path_node(u->addr.sun_path);
    }

    u->bound = true;
    return 0;
}

static int un_listen(socket_t *sock, int backlog) {
    (void)backlog;
    unix_intern_t *u = (unix_intern_t *)sock->private_data;
    if (!u) return -EINVAL;
    u->listening = true;
    return 0;
}

static int un_accept(socket_t *sock, struct sockaddr *addr, socklen_t *addrlen, socket_t **new_sock) {
    unix_intern_t *u = (unix_intern_t *)sock->private_data;
    if (!u || !u->listening) return -EINVAL;

    while (u->accept_head == u->accept_tail) schedule_yield();

    unix_intern_t *client = u->accept_queue[u->accept_tail];
    u->accept_tail = (u->accept_tail + 1) % 16;

    if (addr && addrlen) {
        memcpy(addr, &client->addr, *addrlen);
    }

    socket_t *srv_sock = unix_socket_create(sock->type, sock->protocol);
    if (!srv_sock) return -ENOMEM;

    unix_intern_t *srv_u = (unix_intern_t *)srv_sock->private_data;
    srv_u->peer = client;
    client->peer = srv_u;
    *new_sock = srv_sock;
    return 0;
}

static int un_connect(socket_t *sock, const struct sockaddr *addr, socklen_t addrlen) {
    if (!sock || !addr || addrlen < sizeof(sa_family_t)) return -EINVAL;

    unix_intern_t *u = (unix_intern_t *)sock->private_data;
    const struct sockaddr_un *sun = (const struct sockaddr_un *)addr;

    for (int i = 0; i < MAX_UNIX; i++) {
        if (unix_pool[i].in_use && unix_pool[i].bound && unix_pool[i].listening) {
            if (strcmp(sun->sun_path, unix_pool[i].addr.sun_path) == 0) {
                uint32_t next = (unix_pool[i].accept_head + 1) % 16;
                if (next == unix_pool[i].accept_tail) {
                    return -EAGAIN;
                }

                unix_pool[i].accept_queue[unix_pool[i].accept_head] = u;
                unix_pool[i].accept_head = next;

                while (u->peer == NULL) {
                    schedule_yield();
                }
                return 0;
            }
        }
    }

    return -111; /* ECONNREFUSED */
}

static int un_send(socket_t *sock, const void *buf, size_t len, int flags) {
    (void)flags;
    unix_intern_t *u = (unix_intern_t *)sock->private_data;
    if (!u) return -EINVAL;
    if (u->peer_closed) return -EPIPE;
    unix_intern_t *target = u->peer ? u->peer : u;
    const uint8_t *b = (const uint8_t *)buf;

    uint32_t head = target->rx_head;
    for (size_t i = 0; i < len; i++) {
        uint32_t next = (head + 1) % 8192;
        while (next == target->rx_tail) schedule_yield();
        target->rx_buffer[head] = b[i];
        head = next;
    }
    target->rx_head = head;
    return (int)len;
}

static int un_recv(socket_t *sock, void *buf, size_t len, int flags) {
    unix_intern_t *u = (unix_intern_t *)sock->private_data;
    if (!u) return -EINVAL;

    while (u->rx_head == u->rx_tail) {
        if (u->peer_closed) return 0;
        if (flags & 0x4000) return -EAGAIN;
        schedule_yield();
    }

    uint8_t *b = (uint8_t *)buf;
    size_t read_bytes = 0;
    while (u->rx_tail != u->rx_head && read_bytes < len) {
        b[read_bytes++] = u->rx_buffer[u->rx_tail];
        u->rx_tail = (u->rx_tail + 1) % 8192;
    }
    return (int)read_bytes;
}

static int un_getsockname(socket_t *sock, struct sockaddr *addr, socklen_t *addrlen) {
    unix_intern_t *u = (unix_intern_t *)sock->private_data;
    if (u && addr && addrlen) {
        memcpy(addr, &u->addr, *addrlen);
    }
    return 0;
}

static short un_poll(socket_t *sock, short events) {
    unix_intern_t *u = (unix_intern_t *)sock->private_data;
    if (!u) return 0;
    short revents = 0;
    if ((events & 1) && (u->rx_head != u->rx_tail || u->peer_closed || (u->listening && u->accept_head != u->accept_tail))) {
        revents |= 1;
    }
    if (events & 4) revents |= 4; // POLLOUT
    return revents;
}

static int un_getsockopt(socket_t *sock, int level, int optname, void *optval, socklen_t *optlen) {
    if (!sock || !optval || !optlen) return -EFAULT;

    if (level == SOL_SOCKET) {
        if (optname == SO_ERROR) {
            if (*optlen >= sizeof(int)) {
                *(int *)optval = 0;
                *optlen = sizeof(int);
            }
            return 0;
        }
        if (optname == SO_TYPE) {
            if (*optlen >= sizeof(int)) {
                *(int *)optval = sock->type;
                *optlen = sizeof(int);
            }
            return 0;
        }
    }

    if (*optlen >= sizeof(int)) {
        *(int *)optval = 0;
        *optlen = sizeof(int);
    }
    return 0;
}

static int un_setsockopt(socket_t *sock, int level, int optname, const void *optval, socklen_t optlen) {
    (void)sock; (void)level; (void)optname; (void)optval; (void)optlen;
    return 0;
}

static int un_close(socket_t *sock) {
    if (!sock || !sock->private_data) return 0;
    unix_intern_t *u = (unix_intern_t *)sock->private_data;

    char path[sizeof(u->addr.sun_path)];
    memcpy(path, u->addr.sun_path, sizeof(path));
    path[sizeof(path) - 1] = '\0';
    bool remove_path = u->path_node_created && path[0] != '\0';

    if (u->peer) {
        u->peer->peer_closed = true;
        u->peer->peer = NULL;
    }

    u->in_use = false;
    u->bound = false;
    u->path_node_created = false;
    u->listening = false;
    u->peer = NULL;

    if (remove_path) {
        vfs_unlink(path);
    }
    return 0;
}

static socket_ops_t unix_ops = {
    .bind        = un_bind, 
    .listen      = un_listen, 
    .accept      = un_accept,
    .connect     = un_connect, 
    .send        = un_send, 
    .recv        = un_recv,
    .getsockname = un_getsockname, 
    .getsockopt  = un_getsockopt, 
    .setsockopt  = un_setsockopt,
    .poll        = un_poll,
    .close       = un_close
};

socket_t* unix_socket_create(int type, int protocol) {
    for (int i = 0; i < MAX_UNIX; i++) {
        if (!unix_pool[i].in_use) {
            memset(&unix_pool[i], 0, sizeof(unix_intern_t));
            unix_pool[i].in_use = true; /* МЕТИМ СЛОТ КАК ЗАНЯТЫЙ */

            socket_t *s = malloc_alloc(sizeof(socket_t));
            if (!s) {
                unix_pool[i].in_use = false;
                return NULL;
            }
            memset(s, 0, sizeof(socket_t));
            s->domain = AF_UNIX;
            s->type = type;
            s->protocol = protocol;
            s->ops = &unix_ops;
            s->private_data = &unix_pool[i];
            return s;
        }
    }
    return NULL;
}