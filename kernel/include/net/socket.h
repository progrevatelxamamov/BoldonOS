#ifndef NET_SOCKET_H
#define NET_SOCKET_H

#include <fs/vfs.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "lwip/sockets.h"

#define AF_UNSPEC   0
#define AF_UNIX     1
#define AF_INET     2
#define AF_INET6    10

#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_RAW    3

typedef uint32_t socklen_t;

struct socket;

typedef struct socket_ops {
    int (*bind)(struct socket *sock, const struct sockaddr *addr, socklen_t addrlen);
    int (*listen)(struct socket *sock, int backlog);
    int (*accept)(struct socket *sock, struct sockaddr *addr, socklen_t *addrlen, struct socket **new_sock);
    int (*connect)(struct socket *sock, const struct sockaddr *addr, socklen_t addrlen);
    int (*send)(struct socket *sock, const void *buf, size_t len, int flags);
    int (*recv)(struct socket *sock, void *buf, size_t len, int flags);
    int (*getsockname)(struct socket *sock, struct sockaddr *addr, socklen_t *addrlen);
    int (*getpeername)(struct socket *sock, struct sockaddr *addr, socklen_t *addrlen);
    int (*getsockopt)(struct socket *sock, int level, int optname, void *optval, socklen_t *optlen);
    int (*setsockopt)(struct socket *sock, int level, int optname, const void *optval, socklen_t optlen);
    int (*close)(struct socket *sock);
    short (*poll)(struct socket *sock, short events);
} socket_ops_t;

typedef struct socket {
    int domain;
    int type;
    int protocol;
    int state;
    socket_ops_t *ops;
    void *private_data;
} socket_t;

extern vfs_file_ops_t generic_socket_vfs_ops;

socket_t* unix_socket_create(int type, int protocol);
socket_t* inet_socket_create(int type, int protocol);

#endif