#include <net/socket.h>
#include <errno.h>
#include <scheduler/scheduler.h>
#include <memory.h>


extern int sys_getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen);

static inline bool user_range_ok(uint64_t ptr, uint64_t len) {
    if (ptr == 0) return len == 0;
    uint64_t end;
    if (__builtin_add_overflow(ptr, len, &end)) return false;
    return end <= 0x00007FFFFFFFFFFFULL;
}

extern int sys_socket(int domain, int type, int protocol);
extern int sys_bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
extern int sys_listen(int sockfd, int backlog);
extern int sys_accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
extern int sys_connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
extern int sys_send(int sockfd, const void *buf, size_t len, int flags);
extern int sys_recv(int sockfd, void *buf, size_t len, int flags);
extern int sys_getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
extern int sys_getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen);
extern int sys_setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen);
extern int sys_connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);

uint64_t sys_socket_impl(uint64_t domain, uint64_t type, uint64_t protocol) {
    int res = sys_socket((int)domain, (int)type, (int)protocol);
    if (res < 0) return (uint64_t)(int64_t)res;
    return (uint64_t)res;
}

uint64_t sys_bind_impl(uint64_t sockfd, uint64_t addr_ptr, uint64_t addrlen) {
    if (!addr_ptr || !user_range_ok(addr_ptr, addrlen))
        return (uint64_t)-EFAULT;

    int res = sys_bind((int)sockfd, (struct sockaddr*)addr_ptr, (socklen_t)addrlen);
    if (res < 0) return (uint64_t)(int64_t)res;
    return (uint64_t)res;
}

uint64_t sys_listen_impl(uint64_t sockfd, uint64_t backlog) {
    int res = sys_listen((int)sockfd, (int)backlog);
    if (res < 0) return (uint64_t)(int64_t)res;
    return (uint64_t)res;
}

uint64_t sys_accept_impl(uint64_t sockfd, uint64_t addr_ptr, uint64_t addrlen_ptr) {
    if (addr_ptr && !user_range_ok(addr_ptr, sizeof(struct sockaddr)))
        return (uint64_t)-EFAULT;
    if (addrlen_ptr && !user_range_ok(addrlen_ptr, sizeof(socklen_t)))
        return (uint64_t)-EFAULT;

    int res = sys_accept((int)sockfd, (struct sockaddr *)addr_ptr, (socklen_t *)addrlen_ptr);
    if (res < 0) return (uint64_t)(int64_t)res;
    return (uint64_t)res;
}

uint64_t sys_connect_impl(uint64_t sockfd, uint64_t addr_ptr, uint64_t addrlen) {
    if (!addr_ptr || !user_range_ok(addr_ptr, addrlen))
        return (uint64_t)-EFAULT;

    int res = sys_connect((int)sockfd, (const struct sockaddr *)addr_ptr, (socklen_t)addrlen);
    if (res < 0) return (uint64_t)(int64_t)res;
    return (uint64_t)res;
}

uint64_t sys_send_impl(uint64_t sockfd, uint64_t buf_ptr, uint64_t len, uint64_t flags) {
    if (!buf_ptr || !user_range_ok(buf_ptr, len))
        return (uint64_t)-EFAULT;

    int res = sys_send((int)sockfd, (const void *)buf_ptr, (size_t)len, (int)flags);
    if (res < 0) return (uint64_t)(int64_t)res;
    return (uint64_t)res;
}

uint64_t sys_recv_impl(uint64_t sockfd, uint64_t buf_ptr, uint64_t len, uint64_t flags) {
    if (!buf_ptr || !user_range_ok(buf_ptr, len))
        return (uint64_t)-EFAULT;

    int res = sys_recv((int)sockfd, (void *)buf_ptr, (size_t)len, (int)flags);
    if (res < 0) return (uint64_t)(int64_t)res;
    return (uint64_t)res;
}

uint64_t sys_getsockname_impl(uint64_t sockfd, uint64_t addr_ptr, uint64_t addrlen_ptr) {
    if (!addr_ptr || !user_range_ok(addr_ptr, sizeof(struct sockaddr)))
        return (uint64_t)-EFAULT;
    if (!addrlen_ptr || !user_range_ok(addrlen_ptr, sizeof(socklen_t)))
        return (uint64_t)-EFAULT;

    int res = sys_getsockname((int)sockfd, (struct sockaddr *)addr_ptr, (socklen_t *)addrlen_ptr);
    if (res < 0) return (uint64_t)(int64_t)res;
    return (uint64_t)res;
}

uint64_t sys_getsockopt_impl(uint64_t sockfd, uint64_t level, uint64_t optname, uint64_t optval_ptr, uint64_t optlen_ptr) {
    if (!optval_ptr || !optlen_ptr) return (uint64_t)-EFAULT;
    if (!user_range_ok(optlen_ptr, sizeof(socklen_t))) return (uint64_t)-EFAULT;

    socklen_t len = *(socklen_t *)optlen_ptr;
    if (!user_range_ok(optval_ptr, len)) return (uint64_t)-EFAULT;

    int res = sys_getsockopt((int)sockfd, (int)level, (int)optname, (void *)optval_ptr, (socklen_t *)optlen_ptr);
    if (res < 0) return (uint64_t)(int64_t)res;
    return (uint64_t)res;
}

uint64_t sys_setsockopt_impl(uint64_t sockfd, uint64_t level, uint64_t optname, uint64_t optval_ptr, uint64_t optlen) {
    if (optval_ptr && optlen > 0 && !user_range_ok(optval_ptr, optlen))
        return (uint64_t)-EFAULT;

    int res = sys_setsockopt((int)sockfd, (int)level, (int)optname, (const void *)optval_ptr, (socklen_t)optlen);
    if (res < 0) return (uint64_t)(int64_t)res;
    return (uint64_t)res;
}