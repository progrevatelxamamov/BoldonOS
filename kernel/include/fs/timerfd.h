#ifndef TIMERFD_H
#define TIMERFD_H

#include <stdint.h>

struct timespec_k {
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct itimerspec_k {
    struct timespec_k it_interval;
    struct timespec_k it_value;
};

int  timerfd_init(void);
int  sys_timerfd_create_impl(int clockid, int flags);
int  sys_timerfd_settime_impl(int fd, int flags,
                              const struct itimerspec_k *new_val,
                              struct itimerspec_k *old_val);
int  sys_timerfd_gettime_impl(int fd, struct itimerspec_k *cur);

#endif  