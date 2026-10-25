#ifndef EVENTFD_H
#define EVENTFD_H

#include <stdint.h>

int eventfd_init(void);
int sys_eventfd2_impl(int initval, int flags);

#endif