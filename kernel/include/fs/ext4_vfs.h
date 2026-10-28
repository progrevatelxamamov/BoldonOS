#ifndef EXT4_VFS_H
#define EXT4_VFS_H

#include <stdint.h>
#include <stdbool.h>
#include <fs/vfs.h>

bool is_ext4_sector(const uint8_t *sb_buf);
void ext4_vfs_init(void);
int  ext4_vfs_unmount(vfs_mount_t *mnt);

#endif