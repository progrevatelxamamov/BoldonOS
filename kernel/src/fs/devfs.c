#include <fs/devfs.h>
#include <fs/vfs.h>
#include <mm/malloc.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

#define MAX_DEVFS_NODES 256

typedef struct {
    char name[64];
    vfs_node_t node;
    bool used;
} devfs_node_entry_t;

static devfs_node_entry_t dev_nodes[MAX_DEVFS_NODES];
#define MAX_DEVFS_DIRS 128
static struct {
    char prefix[64];
    vfs_node_t node;
} dev_dirs[MAX_DEVFS_DIRS];
static vfs_node_t devfs_root;

vfs_fs_type_t devfs_type;

static const char* clean_name(const char *name) {
    if (!name) return "";
    if (strncmp(name, "/dev/", 5) == 0) return name + 5;
    if (strncmp(name, "D:/", 3) == 0 || strncmp(name, "d:/", 3) == 0) return name + 3;
    if (name[0] == '/') return name + 1;
    return name;
}

static const char* match_subdir(const char *full_name, const char *dir_prefix) {
    const char *cname = clean_name(full_name);
    if (!dir_prefix || dir_prefix[0] == '\0') {
        return cname;
    } else {
        size_t len = strlen(dir_prefix);
        if (strncmp(cname, dir_prefix, len) == 0 && cname[len] == '/') {
            return cname + len + 1;
        }
        return NULL;
    }
}

static vfs_node_t* devfs_vfs_lookup(vfs_node_t *dir_node, const char *name) {
    if (!dir_node || !name) return NULL;

    const char *dir_prefix = (const char*)dir_node->fs_data;
    if (!dir_prefix) dir_prefix = "";

    for (int i = 0; i < MAX_DEVFS_NODES; i++) {
        if (dev_nodes[i].used) {
            const char *sub_name = match_subdir(dev_nodes[i].name, dir_prefix);
            if (sub_name && strcmp(sub_name, name) == 0) {
                return &dev_nodes[i].node;
            }
        }
    }

    /* Synthesize intermediate directories at every level, not only the root.
       /proc and /sys/block/<disk> are both backed by registered leaves. */
    for (int i = 0; i < MAX_DEVFS_NODES; i++) {
        if (!dev_nodes[i].used) continue;
        const char *child = match_subdir(dev_nodes[i].name, dir_prefix);
        size_t name_len = strlen(name);
        if (!child || strncmp(child, name, name_len) != 0 || child[name_len] != '/')
            continue;
        char prefix[64];
        int written = *dir_prefix
            ? snprintf(prefix, sizeof(prefix), "%s/%s", dir_prefix, name)
            : snprintf(prefix, sizeof(prefix), "%s", name);
        if (written < 0 || written >= (int)sizeof(prefix)) return NULL;
        int free_slot = -1;
        for (int k = 0; k < MAX_DEVFS_DIRS; k++) {
            if (dev_dirs[k].prefix[0] && strcmp(dev_dirs[k].prefix, prefix) == 0)
                return &dev_dirs[k].node;
            if (!dev_dirs[k].prefix[0] && free_slot < 0) free_slot = k;
        }
        if (free_slot < 0) return NULL;
        vfs_node_t *dir = &dev_dirs[free_slot].node;
        memset(dir, 0, sizeof(*dir));
        strcpy(dev_dirs[free_slot].prefix, prefix);
        strncpy(dir->name, name, sizeof(dir->name) - 1);
        dir->type = VFS_TYPE_DIR;
        dir->mode = 0755;
        dir->ops = dir_node->ops;
        dir->file_ops = dir_node->file_ops;
        dir->fs_data = dev_dirs[free_slot].prefix;
        return dir;
    }

    return NULL;
}

static int devfs_vfs_readdir(vfs_file_t *file, vfs_dirent_t *dent) {
    if (!file || !file->node || !dent) return -1;

    const char *dir_prefix = (const char*)file->node->fs_data;
    if (!dir_prefix) dir_prefix = "";

    uint64_t target_idx = file->offset;
    uint64_t current_idx = 0;

    char emitted_dirs[16][32];
    int emitted_count = 0;

    for (int i = 0; i < MAX_DEVFS_NODES; i++) {
        if (!dev_nodes[i].used) continue;

        const char *cname = clean_name(dev_nodes[i].name);

        const char *sub_name = match_subdir(cname, dir_prefix);
        if (sub_name && !strchr(sub_name, '/')) {
            if (current_idx == target_idx) {
                strncpy(dent->name, sub_name, 255);
                dent->inode_num = dev_nodes[i].node.inode_num;
                dent->type = dev_nodes[i].node.type;
                dent->size = 0;
                file->offset++;
                return 0;
            }
            current_idx++;
            continue;
        }

        if (sub_name) {
            const char *slash = strchr(sub_name, '/');
            if (slash) {
                size_t dir_len = slash - sub_name;
                char dir_name[32] = {0};
                if (dir_len < 31) {
                    strncpy(dir_name, sub_name, dir_len);

                    bool already = false;
                    for (int k = 0; k < emitted_count; k++) {
                        if (strcmp(emitted_dirs[k], dir_name) == 0) {
                            already = true;
                            break;
                        }
                    }

                    if (!already) {
                        if (emitted_count < 16) {
                            strncpy(emitted_dirs[emitted_count++], dir_name, 31);
                        }

                        if (current_idx == target_idx) {
                            strncpy(dent->name, dir_name, 255);
                            dent->inode_num = 900 + emitted_count;
                            dent->type = VFS_TYPE_DIR;
                            dent->size = 0;
                            file->offset++;
                            return 0;
                        }
                        current_idx++;
                    }
                }
            }
        }
    }

    return -1; // EOF
}

static vfs_node_ops_t devfs_dir_ops = {
    .lookup = devfs_vfs_lookup,
};

static vfs_file_ops_t devfs_dir_file_ops = {
    .readdir = devfs_vfs_readdir,
};

static vfs_mount_t* devfs_mount(const char *device, void *data) {
    (void)device; (void)data;
    vfs_mount_t *mnt = malloc_alloc(sizeof(vfs_mount_t));
    if (!mnt) return NULL;
    memset(mnt, 0, sizeof(vfs_mount_t));
    mnt->root = &devfs_root;
    return mnt;
}

vfs_fs_type_t devfs_type = {
    .name = "devfs",
    .mount = devfs_mount
};

void devfs_vfs_init(void) {
    memset(dev_nodes, 0, sizeof(dev_nodes));
    memset(dev_dirs, 0, sizeof(dev_dirs));
    
    memset(&devfs_root, 0, sizeof(vfs_node_t));
    strncpy(devfs_root.name, "dev", 255);
    devfs_root.type = VFS_TYPE_DIR;
    devfs_root.mode = 0755;
    devfs_root.ops = &devfs_dir_ops;
    devfs_root.file_ops = &devfs_dir_file_ops;
    devfs_root.fs_data = "";

    vfs_register_fs(&devfs_type);
}

int devfs_register_device(const char *name, vfs_node_type_t type, vfs_file_ops_t *ops, void *private_data) {
    if (!name) return -1;

    const char *sname = clean_name(name);

    for (int i = 0; i < MAX_DEVFS_NODES; i++) {
        if (!dev_nodes[i].used || strcmp(clean_name(dev_nodes[i].name), sname) == 0) {
            memset(&dev_nodes[i], 0, sizeof(devfs_node_entry_t));
            
            strncpy(dev_nodes[i].name, sname, 63);
            strncpy(dev_nodes[i].node.name, sname, 255);
            
            dev_nodes[i].node.type = type;
            dev_nodes[i].node.file_ops = ops;
            dev_nodes[i].node.fs_data = private_data;
            dev_nodes[i].node.mode = type == VFS_TYPE_FILE ? 0444 : 0666;
            dev_nodes[i].node.inode_num = (type == VFS_TYPE_BLOCK_DEV ? 0x0800u : 0x0100u) + (uint32_t)i;
            dev_nodes[i].used = true;
            return 0;
        }
    }
    return -1;
}
static int devfs_readlink(vfs_node_t *node, char *buf, uint64_t size) {
    const char *target = node ? (const char *)node->fs_data : NULL;
    if (!target || !buf) return -EINVAL;
    size_t len = strlen(target);
    if (len > size) len = size;
    memcpy(buf, target, len);
    return (int)len;
}

static vfs_node_ops_t devfs_link_ops = { .readlink = devfs_readlink };

int devfs_register_symlink(const char *name, const char *target) {
    if (!target || devfs_register_device(name, VFS_TYPE_SYMLINK, NULL,
                                         (void *)target) != 0) return -1;
    const char *sname = clean_name(name);
    for (int i = 0; i < MAX_DEVFS_NODES; i++) {
        if (dev_nodes[i].used && strcmp(dev_nodes[i].name, sname) == 0) {
            dev_nodes[i].node.ops = &devfs_link_ops;
            dev_nodes[i].node.mode = 0777;
            return 0;
        }
    }
    return -1;
}

int devfs_unregister_device(const char *name) {
    if (!name) return -1;
    const char *sname = clean_name(name);
    for (int i = 0; i < MAX_DEVFS_NODES; i++) {
        if (dev_nodes[i].used && strcmp(clean_name(dev_nodes[i].name), sname) == 0) {
            memset(&dev_nodes[i], 0, sizeof(devfs_node_entry_t));
            return 0;
        }
    }
    return -1;
}
