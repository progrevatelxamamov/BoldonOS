#ifndef NET_UNIX_SOCKET_H
#define NET_UNIX_SOCKET_H

#include <sys/socket.h>
#include <fs/vfs.h>
#include <stdint.h>
#include <stdbool.h>

#define MAX_UNIX_SOCKETS 64
#define UNIX_SOCKET_BUF_SIZE 8192

typedef struct unix_socket {
    int id;
    int domain;
    int type;
    bool bound;
    bool listening;
    struct sockaddr_un addr;

    uint8_t rx_buffer[UNIX_SOCKET_BUF_SIZE];
    uint32_t rx_head;
    uint32_t rx_tail;

    struct unix_socket *peer;
    struct unix_socket *accept_queue[16];
    int accept_head;
    int accept_tail;
} unix_socket_t;

unix_socket_t* unix_socket_create_struct(int domain, int type, int protocol);
int unix_socket_bind_struct(unix_socket_t *sock, const struct sockaddr *addr, socklen_t addrlen);
int unix_socket_listen_struct(unix_socket_t *sock, int backlog);
unix_socket_t* unix_socket_accept_struct(unix_socket_t *sock, struct sockaddr *addr, socklen_t *addrlen);
int unix_socket_connect_struct(unix_socket_t *sock, const struct sockaddr *addr, socklen_t addrlen);
int unix_socket_send_struct(unix_socket_t *sock, const void *buf, size_t len, int flags);
int unix_socket_recv_struct(unix_socket_t *sock, void *buf, size_t len, int flags);

extern vfs_file_ops_t unix_socket_vfs_ops;

#endif