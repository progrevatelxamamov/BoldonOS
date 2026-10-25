#ifndef SHMEM_H
#define SHMEM_H

#include <stdint.h>
#include <fs/vfs.h>

int  shmem_init(void);
int  sys_memfd_create_impl(const char *name, uint32_t flags);
int  sys_ftruncate_impl(int fd, int64_t length);

#endif