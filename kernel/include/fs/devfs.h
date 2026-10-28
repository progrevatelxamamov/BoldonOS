#ifndef DEVFS_H
#define DEVFS_H

#include <fs/vfs.h>

void devfs_vfs_init(void);
int devfs_register_device(const char *name, vfs_node_type_t type, vfs_file_ops_t *ops, void *private_data);
int devfs_unregister_device(const char *name);

#endif