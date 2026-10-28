#ifndef BOLDON_MOUNT_H
#define BOLDON_MOUNT_H

#include <stdint.h>

int vfs_mount(const char *source, const char *target, const char *fstype,
              uint32_t flags, const void *data);
int vfs_umount(const char *target, uint32_t flags);
int vfs_proc_mounts(char *buf, uint32_t len);
int vfs_proc_mountinfo(char *buf, uint32_t len);

#endif
