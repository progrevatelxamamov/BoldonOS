#include <net/socket.h>
#include <errno.h>
#include <scheduler/scheduler.h>
#include <mm/malloc.h>
#include <memory.h>

static uint32_t sock_vfs_poll(vfs_file_t *file, uint32_t events);

static int sock_vfs_read(vfs_file_t *file, void *buf, uint64_t count) {
    socket_t *sock = (socket_t *)file->node->fs_data;
    if (!sock || !sock->ops || !sock->ops->recv) return -ENOTSUP;
    return sock->ops->recv(sock, buf, count, (file->flags & VFS_O_NONBLOCK) ? 0x4000 : 0);
}

static int sock_vfs_write(vfs_file_t *file, const void *buf, uint64_t count) {
    socket_t *sock = (socket_t *)file->node->fs_data;
    if (!sock || !sock->ops || !sock->ops->send) return -ENOTSUP;
    return sock->ops->send(sock, buf, count, (file->flags & VFS_O_NONBLOCK) ? 0x4000 : 0);
}

static int sock_vfs_close(vfs_file_t *file) {
    socket_t *sock = (socket_t *)file->node->fs_data;
    if (sock && sock->ops && sock->ops->close) {
        sock->ops->close(sock);
    }
    malloc_free(sock);
    return 0;
}

vfs_file_ops_t generic_socket_vfs_ops = {
    .read  = sock_vfs_read,
    .write = sock_vfs_write,
    .close = sock_vfs_close,
    .poll  = sock_vfs_poll
};

static int allocate_socket_fd(socket_t *sock) {
    process_t *p = get_current_process();
    if (!p || !p->fd_table) return -EINVAL;

    vfs_node_t *node = malloc_alloc(sizeof(vfs_node_t));
    memset(node, 0, sizeof(vfs_node_t));
    node->type = VFS_TYPE_CHAR_DEV;
    node->file_ops = &generic_socket_vfs_ops;
    node->fs_data = sock;

    vfs_file_t *file = malloc_alloc(sizeof(vfs_file_t));
    memset(file, 0, sizeof(vfs_file_t));
    file->node = node;
    file->flags = VFS_O_RDWR;
    file->refcount = 1;

    for (int fd = 0; fd < VFS_MAX_FD; fd++) {
        if (!p->fd_table->fds[fd]) {
            p->fd_table->fds[fd] = file;
            return fd;
        }
    }
    malloc_free(file);
    malloc_free(node);
    return -EMFILE;
}

socket_t* get_socket_from_fd(int sockfd) {
    process_t *p = get_current_process();
    if (!p || !p->fd_table || sockfd < 0 || sockfd >= VFS_MAX_FD || !p->fd_table->fds[sockfd]) return NULL;
    vfs_file_t *file = p->fd_table->fds[sockfd];
    if (file->node->file_ops != &generic_socket_vfs_ops) return NULL;
    return (socket_t *)file->node->fs_data;
}

int sys_socket(int domain, int type, int protocol) {
    socket_t *sock = NULL;
    
    if (domain == AF_UNIX) {
        sock = unix_socket_create(type, protocol);
    } else if (domain == AF_INET) {
        sock = inet_socket_create(type, protocol);
    }

    if (!sock) return -97;
    return allocate_socket_fd(sock);
}

int sys_bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    socket_t *sock = get_socket_from_fd(sockfd);
    if (!sock || !sock->ops->bind) return -EBADF;
    return sock->ops->bind(sock, addr, addrlen);
}

int sys_listen(int sockfd, int backlog) {
    socket_t *sock = get_socket_from_fd(sockfd);
    if (!sock || !sock->ops->listen) return -EBADF;
    return sock->ops->listen(sock, backlog);
}

int sys_accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    socket_t *sock = get_socket_from_fd(sockfd);
    if (!sock || !sock->ops->accept) return -EBADF;

    socket_t *new_sock = NULL;
    int res = sock->ops->accept(sock, addr, addrlen, &new_sock);
    if (res < 0) return res;

    return allocate_socket_fd(new_sock);
}

int sys_connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    socket_t *sock = get_socket_from_fd(sockfd);
    if (!sock || !sock->ops->connect) return -EBADF;
    return sock->ops->connect(sock, addr, addrlen);
}

int sys_send(int sockfd, const void *buf, size_t len, int flags) {
    socket_t *sock = get_socket_from_fd(sockfd);
    if (!sock || !sock->ops->send) return -EBADF;
    return sock->ops->send(sock, buf, len, flags);
}

int sys_recv(int sockfd, void *buf, size_t len, int flags) {
    socket_t *sock = get_socket_from_fd(sockfd);
    if (!sock || !sock->ops->recv) return -EBADF;
    return sock->ops->recv(sock, buf, len, flags);
}

int sys_getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    socket_t *sock = get_socket_from_fd(sockfd);
    if (!sock || !sock->ops->getsockname) return -EBADF;
    return sock->ops->getsockname(sock, addr, addrlen);
}

short socket_poll_events(vfs_file_t *file, short events) {
    if (!file || !file->node || file->node->file_ops != &generic_socket_vfs_ops) return 0;
    socket_t *sock = (socket_t *)file->node->fs_data;
    if (!sock || !sock->ops->poll) return 0;
    return sock->ops->poll(sock, events);
}

int sys_getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen) {
    socket_t *sock = get_socket_from_fd(sockfd);
    if (!sock || !sock->ops || !sock->ops->getsockopt) return -EBADF;
    return sock->ops->getsockopt(sock, level, optname, optval, optlen);
}

int sys_setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen) {
    socket_t *sock = get_socket_from_fd(sockfd);
    if (!sock || !sock->ops || !sock->ops->setsockopt) return -EBADF;
    return sock->ops->setsockopt(sock, level, optname, optval, optlen);
}

static uint32_t sock_vfs_poll(vfs_file_t *file, uint32_t events) {
    return (uint32_t)socket_poll_events(file, (short)events);
}