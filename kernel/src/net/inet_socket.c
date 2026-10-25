#include <net/socket.h>
#include <errno.h>
#include <mm/malloc.h>
#include <memory.h>
#include <stdint.h>

#include "lwip/sockets.h"

/* lwIP fd лежит в sock->private_data как (void*)(uintptr_t)fd */

static int inet_get_lfd(socket_t *sock)
{
    if (!sock || !sock->private_data) return -1;
    return (int)(uintptr_t)sock->private_data;
}

static int inet_bind(struct socket *sock, const struct sockaddr *addr, socklen_t addrlen)
{
    int lfd = inet_get_lfd(sock);
    if (lfd < 0) return -EBADF;
    int r = lwip_bind(lfd, addr, addrlen);
    return r < 0 ? -errno : 0;
}

static int inet_listen(socket_t *sock, int backlog)
{
    int lfd = inet_get_lfd(sock);
    if (lfd < 0) return -EBADF;
    int r = lwip_listen(lfd, backlog);
    return r < 0 ? -errno : 0;
}

static int inet_connect(socket_t *sock, const struct sockaddr *addr, socklen_t addrlen)
{
    int lfd = inet_get_lfd(sock);
    if (lfd < 0) return -EBADF;
    int r = lwip_connect(lfd, addr, addrlen);
    return r < 0 ? -errno : 0;
}

static int inet_send(socket_t *sock, const void *buf, size_t len, int flags)
{
    int lfd = inet_get_lfd(sock);
    if (lfd < 0) return -EBADF;

    /* У тебя NONBLOCK из VFS приходит как 0x4000 — это Linux MSG_DONTWAIT */
    int r = lwip_send(lfd, buf, len, flags);
    return r < 0 ? -errno : r;
}

static int inet_recv(socket_t *sock, void *buf, size_t len, int flags)
{
    int lfd = inet_get_lfd(sock);
    if (lfd < 0) return -EBADF;

    int r = lwip_recv(lfd, buf, len, flags);
    return r < 0 ? -errno : r;
}

static int inet_getsockname(socket_t *sock, struct sockaddr *addr, socklen_t *addrlen)
{
    int lfd = inet_get_lfd(sock);
    if (lfd < 0) return -EBADF;
    int r = lwip_getsockname(lfd, addr, addrlen);
    return r < 0 ? -errno : 0;
}

static int inet_getpeername(socket_t *sock, struct sockaddr *addr, socklen_t *addrlen)
{
    int lfd = inet_get_lfd(sock);
    if (lfd < 0) return -EBADF;
    int r = lwip_getpeername(lfd, addr, addrlen);
    return r < 0 ? -errno : 0;
}

static int inet_getsockopt(socket_t *sock, int level, int optname,
                           void *optval, socklen_t *optlen)
{
    int lfd = inet_get_lfd(sock);
    if (lfd < 0) return -EBADF;
    int r = lwip_getsockopt(lfd, level, optname, optval, optlen);
    return r < 0 ? -errno : 0;
}

static int inet_setsockopt(socket_t *sock, int level, int optname,
                           const void *optval, socklen_t optlen)
{
    int lfd = inet_get_lfd(sock);
    if (lfd < 0) return -EBADF;
    int r = lwip_setsockopt(lfd, level, optname, optval, optlen);
    return r < 0 ? -errno : 0;
}

/* close: только lwIP fd. socket_t освободит sock_vfs_close() */
static int inet_close(socket_t *sock)
{
    int lfd = inet_get_lfd(sock);
    if (lfd >= 0) {
        lwip_close(lfd);
        sock->private_data = NULL;
    }
    return 0;
}

/*
 * poll: short events как в Linux poll.
 * Если unix_socket отдаёт другие биты — выровняй один-в-один.
 * Обычно: POLLIN=0x0001, POLLOUT=0x0004, POLLERR=0x0008, POLLHUP=0x0010
 */
static short inet_poll(socket_t *sock, short events)
{
    int lfd = inet_get_lfd(sock);
    if (lfd < 0) return 0;

    struct pollfd pfd;
    pfd.fd = lfd;
    pfd.events = 0;
    pfd.revents = 0;

    if (events & 0x0001) pfd.events |= POLLIN;
    if (events & 0x0004) pfd.events |= POLLOUT;
    if (events & 0x0008) pfd.events |= POLLERR;
    if (events & 0x0010) pfd.events |= POLLHUP;
    if (pfd.events == 0)
        pfd.events = POLLIN | POLLOUT | POLLERR | POLLHUP;

    if (lwip_poll(&pfd, 1, 0) < 0)
        return 0;

    short out = 0;
    if (pfd.revents & POLLIN)  out |= 0x0001;
    if (pfd.revents & POLLOUT) out |= 0x0004;
    if (pfd.revents & POLLERR) out |= 0x0008;
    if (pfd.revents & POLLHUP) out |= 0x0010;
    return out & events;
}

static int inet_accept(socket_t *sock, struct sockaddr *addr, socklen_t *addrlen,
                       socket_t **out_sock)
{
    int lfd = inet_get_lfd(sock);
    if (lfd < 0 || !out_sock) return -EBADF;

    int new_lfd = lwip_accept(lfd, addr, addrlen);
    if (new_lfd < 0) return -errno;

    socket_t *ns = malloc_alloc(sizeof(socket_t));
    if (!ns) {
        lwip_close(new_lfd);
        return -ENOMEM;
    }
    memset(ns, 0, sizeof(socket_t));
    ns->domain   = AF_INET;
    ns->type     = sock->type;
    ns->protocol = sock->protocol;
    ns->ops      = sock->ops; /* те же inet_ops */
    ns->private_data = (void *)(uintptr_t)new_lfd;

    *out_sock = ns;
    return 0;
}

static socket_ops_t inet_ops = {
    .bind        = inet_bind,
    .listen      = inet_listen,
    .accept      = inet_accept,
    .connect     = inet_connect,
    .send        = inet_send,
    .recv        = inet_recv,
    .getsockname = inet_getsockname,
    .getpeername = inet_getpeername,
    .close       = inet_close,
    .poll        = inet_poll,
    .getsockopt  = inet_getsockopt,
    .setsockopt  = inet_setsockopt,
};

socket_t *inet_socket_create(int type, int protocol)
{
    int lfd = lwip_socket(AF_INET, type, protocol);
    if (lfd < 0)
        return NULL;

    socket_t *s = malloc_alloc(sizeof(socket_t));
    if (!s) {
        lwip_close(lfd);
        return NULL;
    }
    memset(s, 0, sizeof(socket_t));

    s->domain        = AF_INET;
    s->type          = type;
    s->protocol      = protocol;
    s->ops           = &inet_ops;
    s->private_data  = (void *)(uintptr_t)lfd;

    return s;
}