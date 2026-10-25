#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stdbool.h>

#define VFS_O_READ      (1 << 0)
#define VFS_O_WRITE     (1 << 1)
#define VFS_O_RDWR      (VFS_O_READ | VFS_O_WRITE)
#define VFS_O_CREAT     (1 << 2)
#define VFS_O_TRUNC     (1 << 3)
#define VFS_O_APPEND    (1 << 4)
#define VFS_O_NONBLOCK  (1 << 5)
#define VFS_O_CLOEXEC   (1 << 6)

#define VFS_SEEK_SET    0
#define VFS_SEEK_CUR    1
#define VFS_SEEK_END    2

#define VFS_MAX_FD      256

#define VFS_PERM_READ   4
#define VFS_PERM_WRITE  2
#define VFS_PERM_EXEC   1

typedef enum {
    VFS_TYPE_UNKNOWN,
    VFS_TYPE_FILE,
    VFS_TYPE_DIR,
    VFS_TYPE_SYMLINK,
    VFS_TYPE_BLOCK_DEV,
    VFS_TYPE_CHAR_DEV,
    VFS_TYPE_FIFO,
    VFS_TYPE_SOCKET
} vfs_node_type_t;

typedef struct {
    char name[256];
    uint64_t inode_num;
    vfs_node_type_t type;
    uint64_t size;
} vfs_dirent_t;

typedef struct {
    uint64_t inode_num;
    uint64_t size;
    uint32_t mode;
    vfs_node_type_t type;
    uint32_t uid;
    uint32_t gid;
} vfs_stat_t;

struct vfs_node;
struct vfs_file;
struct vfs_mount;

typedef struct {
    int (*read)(struct vfs_file *file, void *buf, uint64_t count);
    int (*write)(struct vfs_file *file, const void *buf, uint64_t count);
    int (*close)(struct vfs_file *file);
    uint64_t (*lseek)(struct vfs_file *file, int64_t offset, int whence);
    int (*ioctl)(struct vfs_file *file, uint64_t request, void *arg);
    int (*readdir)(struct vfs_file *file, vfs_dirent_t *dirent);
    void* (*mmap)(struct vfs_file *file, void *addr, uint64_t length, int prot, int flags, uint64_t offset);
    uint32_t (*poll)(struct vfs_file *file, uint32_t mask);
} vfs_file_ops_t;

typedef struct {
    struct vfs_node* (*lookup)(struct vfs_node *dir, const char *name);
    int (*create)(struct vfs_node *dir, const char *name, uint16_t mode, struct vfs_node **out_node);
    int (*mkdir)(struct vfs_node *dir, const char *name, uint16_t mode);
    int (*unlink)(struct vfs_node *dir, const char *name);
    int (*rmdir)(struct vfs_node *dir, const char *name);
    int (*symlink)(struct vfs_node *dir, const char *name, const char *target);
    int (*link)(struct vfs_node *dir, struct vfs_node *old_node, const char *name);
    int (*readlink)(struct vfs_node *node, char *buf, uint64_t size);
    int (*stat)(struct vfs_node *node, vfs_stat_t *st);
    int (*chmod)(struct vfs_node *node, uint32_t mode);
    int (*chown)(struct vfs_node *node, uint32_t uid, uint32_t gid);
} vfs_node_ops_t;

typedef struct vfs_node {
    char name[256];
    vfs_node_type_t type;
    uint64_t inode_num;
    uint64_t size;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t refcount;
    struct vfs_mount *mount;
    void *fs_data;
    vfs_node_ops_t *ops;
    vfs_file_ops_t *file_ops;
} vfs_node_t;

typedef struct vfs_file {
    vfs_node_t *node;
    uint64_t offset;
    uint32_t flags;
    uint32_t refcount;
    void *private_data;
} vfs_file_t;

typedef struct vfs_mount {
    char drive_letter;
    vfs_node_t *root;
    void *fs_data;
} vfs_mount_t;

typedef struct {
    vfs_mount_t* (*mount)(const char *device, void *data);
    const char *name;
} vfs_fs_type_t;

typedef struct {
    vfs_file_t *fds[VFS_MAX_FD];
    uint32_t uid;
    uint32_t gid;
} process_fd_table_t;

void vfs_init(void);
int vfs_register_fs(vfs_fs_type_t *fs);
int vfs_mount_fs(const char *fs_name, const char *device, char drive_letter, void *data);
void vfs_set_boot_drive(char drive_letter);

vfs_node_t *vfs_resolve_path(const char *path);
vfs_node_t *vfs_resolve_path_ext(const char *path, bool parent_only, char *last_component);

vfs_file_t *vfs_open(const char *path, uint32_t flags);
int vfs_read(vfs_file_t *file, void *buf, uint64_t count);
int vfs_write(vfs_file_t *file, const void *buf, uint64_t count);
int vfs_close(vfs_file_t *file);
int64_t vfs_lseek(vfs_file_t *file, int64_t offset, int whence);
int vfs_readdir(vfs_file_t *file, vfs_dirent_t *dirent);
int vfs_stat(const char *path, vfs_stat_t *st);
int vfs_chmod(const char *path, uint32_t mode);
int vfs_chown(const char *path, uint32_t uid, uint32_t gid);
int vfs_check_access(vfs_node_t *node, uint32_t uid, uint32_t gid, int req_perms);
int vfs_mkdir(const char *path, uint32_t mode);
int vfs_unlink(const char *path);
int vfs_rmdir(const char *path);
int vfs_link(const char *oldpath, const char *newpath);
int vfs_fchmod(vfs_file_t *file, uint32_t mode);

int vfs_copy_file(const char *src_path, const char *dst_path);

int sys_open(process_fd_table_t *table, const char *path, uint32_t flags);
int sys_read(process_fd_table_t *table, int fd, void *buf, uint64_t count);
int sys_write(process_fd_table_t *table, int fd, const void *buf, uint64_t count);
int sys_close(process_fd_table_t *table, int fd);
int sys_ioctl(process_fd_table_t *table, int fd, uint64_t request, void *arg);
void *sys_mmap(process_fd_table_t *table, void *addr, uint64_t length, int prot, int flags, int fd, uint64_t offset);

#endif