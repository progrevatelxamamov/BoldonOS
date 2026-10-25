#include "cpu/time.h"
#include "gconsole.h"
#include <syscall/syscall.h>
#include <scheduler/scheduler.h>
#include <fs/vfs.h>
#include <mm/malloc.h>
#include <string.h>
#include <errno.h>

#ifndef EPOLL_CTL_ADD
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3
#endif

#ifndef EPOLLIN
#define EPOLLIN  0x001
#define EPOLLOUT 0x004
#define EPOLLERR 0x008
#define EPOLLHUP 0x010
#endif

#ifndef MAX_FDS
#define MAX_FDS 256
#endif

struct epoll_event {
    uint32_t events;
    uint64_t data;
} __attribute__((packed));

typedef struct epoll_entry {
    int fd;
    struct epoll_event event;
    struct epoll_entry *next;
} epoll_entry_t;

typedef struct epoll_instance {
    epoll_entry_t *head;
    int count;
} epoll_instance_t;

static int epoll_vfs_close(vfs_file_t *file) {
    if (!file) return 0;

    if (file->private_data) {
        epoll_instance_t *ep = (epoll_instance_t *)file->private_data;
        epoll_entry_t *curr = ep->head;
        while (curr) {
            epoll_entry_t *next = curr->next;
            malloc_free(curr);
            curr = next;
        }
        malloc_free(ep);
        file->private_data = NULL;
    }

    if (file->node) {
        malloc_free(file->node);
        file->node = NULL;
    }

    return 0;
}

static vfs_file_ops_t epoll_file_ops = {
    .close = epoll_vfs_close
};

static epoll_instance_t *get_epoll(int epfd) {
    process_t *curr = get_current_process();
    if (!curr || !curr->fd_table || epfd < 0 || epfd >= MAX_FDS) return NULL;

    vfs_file_t *file = curr->fd_table->fds[epfd];
    if (!file || !file->private_data) return NULL;

    return (epoll_instance_t *)file->private_data;
}

uint64_t sys_epoll_create1_impl(int flags) {
    (void)flags;
    process_t *curr = get_current_process();
    if (!curr || !curr->fd_table) return (uint64_t)-EINVAL;

    int fd = -1;
    for (int i = 3; i < MAX_FDS; i++) {
        if (curr->fd_table->fds[i] == NULL) {
            fd = i;
            break;
        }
    }
    if (fd == -1) return (uint64_t)-ENOMEM;

    epoll_instance_t *ep = (epoll_instance_t *)malloc_alloc(sizeof(epoll_instance_t));
    if (!ep) return (uint64_t)-ENOMEM;
    memset(ep, 0, sizeof(epoll_instance_t));

    vfs_node_t *node = (vfs_node_t *)malloc_alloc(sizeof(vfs_node_t));
    if (!node) {
        malloc_free(ep);
        return (uint64_t)-ENOMEM;
    }
    memset(node, 0, sizeof(vfs_node_t));
    node->file_ops = &epoll_file_ops;

    vfs_file_t *file = (vfs_file_t *)malloc_alloc(sizeof(vfs_file_t));
    if (!file) {
        malloc_free(node);
        malloc_free(ep);
        return (uint64_t)-ENOMEM;
    }
    memset(file, 0, sizeof(vfs_file_t));
    file->node = node;
    file->private_data = ep;
    file->refcount = 1;

    curr->fd_table->fds[fd] = file;

    return (uint64_t)fd;
}

uint64_t sys_epoll_create_impl(int size) {
    if (size <= 0) return (uint64_t)-EINVAL;
    return sys_epoll_create1_impl(0);
}

uint64_t sys_epoll_ctl_impl(int epfd, int op, int fd, struct epoll_event *event) {
    epoll_instance_t *ep = get_epoll(epfd);
    if (!ep) {
        return (uint64_t)-EBADF;
    }

    if (op == EPOLL_CTL_ADD) {
        for (epoll_entry_t *curr = ep->head; curr; curr = curr->next) {
            if (curr->fd == fd) return (uint64_t)-EEXIST;
        }

        epoll_entry_t *entry = (epoll_entry_t *)malloc_alloc(sizeof(epoll_entry_t));
        if (!entry) return (uint64_t)-ENOMEM;

        entry->fd = fd;
        if (event) {
            entry->event = *event;
        } else {
            memset(&entry->event, 0, sizeof(struct epoll_event));
        }

        entry->next = ep->head;
        ep->head = entry;
        ep->count++;
        return 0;
    } 
    else if (op == EPOLL_CTL_DEL) {
        epoll_entry_t **pp = &ep->head;
        while (*pp) {
            if ((*pp)->fd == fd) {
                epoll_entry_t *unneeded = *pp;
                *pp = unneeded->next;
                malloc_free(unneeded);
                ep->count--;
                return 0;
            }
            pp = &(*pp)->next;
        }
        return (uint64_t)-ENOENT;
    } 
    else if (op == EPOLL_CTL_MOD) {
        for (epoll_entry_t *curr = ep->head; curr; curr = curr->next) {
            if (curr->fd == fd) {
                if (event) curr->event = *event;
                return 0;
            }
        }
        return (uint64_t)-ENOENT;
    }

    return (uint64_t)-EINVAL;
}

uint64_t sys_epoll_wait_impl(int epfd, struct epoll_event *events, int maxevents, int timeout) {
    if (maxevents <= 0 || !events) return (uint64_t)-EINVAL;

    epoll_instance_t *ep = get_epoll(epfd);
    if (!ep) return (uint64_t)-EBADF;

    process_t *curr = get_current_process();
    if (!curr || !curr->fd_table) return (uint64_t)-EINVAL;

    uint64_t start = get_unix_time();

    while (1) {
        int ready_count = 0;
        epoll_entry_t *entry = ep->head;

        while (entry && ready_count < maxevents) {
            int fd = entry->fd;
            if (fd >= 0 && fd < MAX_FDS && curr->fd_table->fds[fd] != NULL) {
                vfs_file_t *file = curr->fd_table->fds[fd];
                uint32_t revents = 0;

                if (file->node && file->node->file_ops && file->node->file_ops->poll) {
                    revents = file->node->file_ops->poll(file, entry->event.events);
                } else {
                    revents = entry->event.events & (EPOLLIN | EPOLLOUT);
                }

                if (revents) {
                    events[ready_count].events = revents;
                    events[ready_count].data = entry->event.data;
                    ready_count++;
                }
            }
            entry = entry->next;
        }

        if (ready_count > 0) return (uint64_t)ready_count;

        if (timeout == 0) return 0;
        if (timeout > 0 && (get_unix_time() - start) * 1000 >= (uint64_t)timeout)
            return 0;

        schedule_yield();
    }
}

uint64_t sys_epoll_pwait_impl(int epfd, struct epoll_event *events, int maxevents, int timeout, const void *sigmask) {
    (void)sigmask;
    return sys_epoll_wait_impl(epfd, events, maxevents, timeout);
}

uint64_t sys_epoll_create1(int flags) __attribute__((alias("sys_epoll_create1_impl")));
uint64_t sys_epoll_create(int size) __attribute__((alias("sys_epoll_create_impl")));
uint64_t sys_epoll_ctl(int epfd, int op, int fd, struct epoll_event *event) __attribute__((alias("sys_epoll_ctl_impl")));
uint64_t sys_epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout) __attribute__((alias("sys_epoll_wait_impl")));
uint64_t sys_epoll_pwait(int epfd, struct epoll_event *events, int maxevents, int timeout, const void *sigmask) __attribute__((alias("sys_epoll_pwait_impl")));

uint64_t sc_sys_epoll_create1(int flags) __attribute__((alias("sys_epoll_create1_impl")));
uint64_t sc_sys_epoll_create(int size) __attribute__((alias("sys_epoll_create_impl")));
uint64_t sc_sys_epoll_ctl(int epfd, int op, int fd, struct epoll_event *event) __attribute__((alias("sys_epoll_ctl_impl")));
uint64_t sc_sys_epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout) __attribute__((alias("sys_epoll_wait_impl")));
uint64_t sc_sys_epoll_pwait(int epfd, struct epoll_event *events, int maxevents, int timeout, const void *sigmask) __attribute__((alias("sys_epoll_pwait_impl")));