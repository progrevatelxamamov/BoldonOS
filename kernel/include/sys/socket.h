#ifndef SYS_SOCKET_H
#define SYS_SOCKET_H

#include <stdint.h>
#include <stddef.h>

#define AF_UNIX         1
#define AF_INET         2
#define AF_INET6        3

#define SOCK_STREAM     1
#define SOCK_DGRAM      2
#define SOCK_RAW        3

typedef uint32_t socklen_t;
typedef uint16_t sa_family_t;

struct sockaddr_un {
    sa_family_t sun_family;
    char        sun_path[108];
};

int sys_socket(int domain, int type, int protocol);
int sys_bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int sys_listen(int sockfd, int backlog);
int sys_accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
int sys_connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int sys_send(int sockfd, const void *buf, size_t len, int flags);
int sys_recv(int sockfd, void *buf, size_t len, int flags);

#endif