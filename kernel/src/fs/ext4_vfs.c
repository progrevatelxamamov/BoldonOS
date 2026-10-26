#include "disk/partitions.h"
#include "fs/vfs.h"
#include "gconsole.h"
#include "kernel.h"
#include "mm/pmm.h"
#include "scheduler/scheduler.h"
#include "sys/mman.h"
#include <scheduler/spinlock.h>
#include <fs/ext4_vfs.h>
#include <fs/lwext4/ext4.h>
#include <fs/lwext4/ext4_blockdev.h>
#include <disk/block.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mm/malloc.h>
#include <errno.h>

typedef struct {
    char dev_name[32];
    uint64_t start_lba;
    struct ext4_blockdev bd;
    struct ext4_blockdev_iface iface;
} boldon_ext4_bdev_t;

typedef struct {
    ext4_file file;
    bool is_dir;
    ext4_dir dir;
} ext4_vfs_handle_t;

static boldon_ext4_bdev_t g_ext4_bdevs[4];
static int g_ext4_count = 0;

static vfs_file_ops_t g_ext4_file_ops;
static vfs_node_ops_t g_ext4_node_ops;
static vfs_fs_type_t g_ext4_fs_type;

static spinlock_t ext4_lock_guard = SPINLOCK_INIT;
static spinlock_t ext4_lock_fallback = SPINLOCK_INIT;
static thread_t *ext4_lock_owner;
static uint32_t ext4_lock_depth;

static void ext4_os_lock(void) {
    thread_t *self = get_current_thread();
    if (!self) {
        spin_lock(&ext4_lock_fallback);
        return;
    }

    for (;;) {
        uint64_t flags;
        spin_lock_irqsave(&ext4_lock_guard, &flags);
        if (ext4_lock_owner == self) {
            ext4_lock_depth++;
            spin_unlock_irqrestore(&ext4_lock_guard, flags);
            return;
        }
        if (!ext4_lock_owner) {
            ext4_lock_owner = self;
            ext4_lock_depth = 1;
            spin_unlock_irqrestore(&ext4_lock_guard, flags);
            return;
        }
        spin_unlock_irqrestore(&ext4_lock_guard, flags);
        schedule_yield();
    }
}

static void ext4_os_unlock(void) {
    thread_t *self = get_current_thread();
    if (!self) {
        spin_unlock(&ext4_lock_fallback);
        return;
    }

    uint64_t flags;
    spin_lock_irqsave(&ext4_lock_guard, &flags);
    if (ext4_lock_owner == self) {
        if (ext4_lock_depth > 1) {
            ext4_lock_depth--;
        } else {
            ext4_lock_depth = 0;
            ext4_lock_owner = NULL;
        }
    }
    spin_unlock_irqrestore(&ext4_lock_guard, flags);
}

static const struct ext4_lock ext4_os_locks = {
    ext4_os_lock,
    ext4_os_unlock
};

static int boldon_ext4_open(struct ext4_blockdev *bdev) {
    (void)bdev;
    return EOK;
}

static int boldon_ext4_bread(struct ext4_blockdev *bdev, void *buf, uint64_t blk_id, uint32_t blk_cnt) {
    boldon_ext4_bdev_t *dev = (boldon_ext4_bdev_t *)bdev->bdif->p_user;

    uint32_t sectors_per_block = bdev->bdif->ph_bsize / 512;
    if (sectors_per_block == 0) sectors_per_block = 1;

    uint64_t lba = dev->start_lba + (blk_id * sectors_per_block);
    uint32_t sectors = blk_cnt * sectors_per_block;

    if (block_read(dev->dev_name, lba, sectors, buf) != 0) {
        return EIO;
    }
    return EOK;
}

static int boldon_ext4_bwrite(struct ext4_blockdev *bdev, const void *buf, uint64_t blk_id, uint32_t blk_cnt) {
    boldon_ext4_bdev_t *dev = (boldon_ext4_bdev_t *)bdev->bdif->p_user;

    uint32_t sectors_per_block = bdev->bdif->ph_bsize / 512;
    if (sectors_per_block == 0) sectors_per_block = 1;

    uint64_t lba = dev->start_lba + (blk_id * sectors_per_block);
    uint32_t sectors = blk_cnt * sectors_per_block;

    if (block_write(dev->dev_name, lba, sectors, (void *)buf) != 0) {
        return EIO;
    }
    return EOK;
}

static int boldon_ext4_close(struct ext4_blockdev *bdev) {
    (void)bdev;
    return EOK;
}

bool is_ext4_sector(const uint8_t *sb_buf) {
    uint16_t magic = *(uint16_t *)(sb_buf + 0x38);
    if (magic != 0xEF53) {
        return false;
    }
    return true;
}

static void get_lwext4_path(vfs_node_t *node, char *buf, size_t max_len) {
    if (!node || node->name[0] == '\0') {
        strncpy(buf, "/", max_len);
        return;
    }
    const char *p = node->name;
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':') {
        p += 2;
    }
    if (p[0] == '\0') {
        strncpy(buf, "/", max_len);
    } else {
        strncpy(buf, p, max_len);
    }
}

static ext4_vfs_handle_t *ext4_ensure_handle(vfs_file_t *file) {
    if (!file || !file->node) return NULL;
    if (file->private_data) return (ext4_vfs_handle_t *)file->private_data;

    ext4_vfs_handle_t *h = malloc_alloc(sizeof(ext4_vfs_handle_t));
    if (!h) return NULL;
    memset(h, 0, sizeof(ext4_vfs_handle_t));

    char path[512];
    get_lwext4_path(file->node, path, sizeof(path));

    if (file->node->type == VFS_TYPE_DIR) {
        h->is_dir = true;
        int r = ext4_dir_open(&h->dir, path);
        if (r != EOK) {
            malloc_free(h);
            return NULL;
        }
    } else {
        h->is_dir = false;
        const char *mode = "r+";
        if (file->flags & VFS_O_CREAT) mode = "w+";
        else if (!(file->flags & VFS_O_WRITE)) mode = "r";

        int r = ext4_fopen(&h->file, path, mode);
        if (r != EOK) {
            r = ext4_fopen(&h->file, path, "r");
            if (r != EOK) {
                malloc_free(h);
                return NULL;
            }
        }
    }

    file->private_data = h;
    return h;
}

static int ext4_vfs_op_read(vfs_file_t *file, void *buf, uint64_t count) {
    ext4_vfs_handle_t *h = ext4_ensure_handle(file);
    if (!h || h->is_dir) return -1;

    size_t bytes_read = 0;
    int r = ext4_fread(&h->file, buf, count, &bytes_read);
    if (r != EOK) return -1;

    file->offset = ext4_ftell(&h->file);
    return (int)bytes_read;
}

static int ext4_vfs_op_write(vfs_file_t *file, const void *buf, uint64_t count) {
    ext4_vfs_handle_t *h = ext4_ensure_handle(file);
    if (!h || h->is_dir) return -1;

    size_t bytes_written = 0;
    int r = ext4_fwrite(&h->file, buf, count, &bytes_written);
    if (r != EOK) return -1;

    r = ext4_cache_flush("/");
    if (r != EOK) return -1;

    file->offset = ext4_ftell(&h->file);
    return (int)bytes_written;
}

static int ext4_vfs_op_close(vfs_file_t *file) {
    if (!file || !file->private_data) return 0;
    ext4_vfs_handle_t *h = (ext4_vfs_handle_t *)file->private_data;

    if (h->is_dir) {
        ext4_dir_close(&h->dir);
    } else {
        ext4_fclose(&h->file);
    }

    malloc_free(h);
    file->private_data = NULL;
    return 0;
}

static uint64_t ext4_vfs_op_lseek(vfs_file_t *file, int64_t offset, int whence) {
    ext4_vfs_handle_t *h = ext4_ensure_handle(file);
    if (!h || h->is_dir) return 0;

    int origin = SEEK_SET;
    if (whence == VFS_SEEK_CUR) origin = SEEK_CUR;
    if (whence == VFS_SEEK_END) origin = SEEK_END;

    ext4_fseek(&h->file, offset, origin);
    file->offset = ext4_ftell(&h->file);
    return file->offset;
}

static int ext4_vfs_op_readdir(vfs_file_t *file, vfs_dirent_t *dirent) {
    if (!file || !dirent) return -1;

    ext4_vfs_handle_t *h = ext4_ensure_handle(file);
    if (!h || !h->is_dir) return -1;

    const ext4_direntry *d = ext4_dir_entry_next(&h->dir);
    if (!d) return -1;

    strncpy(dirent->name, (const char *)d->name, d->name_length);
    dirent->name[d->name_length] = '\0';
    dirent->inode_num = d->inode;

    if (d->inode_type == EXT4_DE_DIR) {
        dirent->type = VFS_TYPE_DIR;
    } else if (d->inode_type == EXT4_DE_SYMLINK) {
        dirent->type = VFS_TYPE_SYMLINK;
    } else {
        dirent->type = VFS_TYPE_FILE;
    }

    dirent->size = 0;
    return 0;
}

static vfs_node_t *ext4_vfs_op_lookup(vfs_node_t *dir, const char *name) {
    if (!dir || !name) return NULL;

    char parent_path[512];
    get_lwext4_path(dir, parent_path, sizeof(parent_path));

    char target_path[512];
    if (strcmp(parent_path, "/") == 0) {
        snprintf(target_path, sizeof(target_path), "/%s", name);
    } else {
        snprintf(target_path, sizeof(target_path), "%s/%s", parent_path, name);
    }

    uint32_t mode = 0;
    const char *p_path = target_path;
    int r = ext4_mode_get(p_path, &mode);
    if (r != EOK && p_path[0] == '/') {
        p_path = target_path + 1;
        r = ext4_mode_get(p_path, &mode);
    }

    if (r != EOK) return NULL;

    uint32_t ino = 0;
    uint64_t size = 0;
    struct ext4_inode stat_buf;
    if (ext4_raw_inode_fill(p_path, &ino, &stat_buf) == EOK) {
        size = ((uint64_t)stat_buf.size_hi << 32) | stat_buf.size_lo;
    }

    vfs_node_t *node = malloc_alloc(sizeof(vfs_node_t));
    if (!node) return NULL;
    memset(node, 0, sizeof(vfs_node_t));

    strncpy(node->name, target_path, 255);
    node->inode_num = ino;
    node->size = size;
    node->mode = mode;
    node->refcount = 1;
    node->mount = dir->mount;
    node->ops = &g_ext4_node_ops;
    node->file_ops = &g_ext4_file_ops;

    uint16_t fmt = mode & 0xF000;
    if (fmt == 0x4000) node->type = VFS_TYPE_DIR;
    else if (fmt == 0xA000) node->type = VFS_TYPE_SYMLINK;
    else node->type = VFS_TYPE_FILE;

    return node;
}

static int ext4_vfs_op_readlink(vfs_node_t *node, char *buf, uint64_t size) {
    if (!node || !buf) return -1;
    char path[512];
    get_lwext4_path(node, path, sizeof(path));

    size_t rsize = 0;
    int r = ext4_readlink(path, buf, size, &rsize);
    if (r != EOK) return -1;

    buf[rsize] = '\0';
    return (int)rsize;
}

static int ext4_vfs_op_stat(vfs_node_t *node, vfs_stat_t *st) {
    if (!node || !st) return -1;
    st->inode_num = node->inode_num;
    st->size = node->size;
    st->mode = node->mode;
    st->type = node->type;
    st->uid = node->uid;
    st->gid = node->gid;
    return 0;
}

static int ext4_vfs_op_create(vfs_node_t *dir, const char *name, uint16_t mode, vfs_node_t **out_node) {
    if (!dir || !name || !out_node) return -1;

    char parent_path[512];
    get_lwext4_path(dir, parent_path, sizeof(parent_path));

    char target_path[512];
    if (strcmp(parent_path, "/") == 0)
        snprintf(target_path, sizeof(target_path), "/%s", name);
    else
        snprintf(target_path, sizeof(target_path), "%s/%s", parent_path, name);

    ext4_file f;
    int r = ext4_fopen(&f, target_path, "w+");
    if (r != EOK) return -1;
    ext4_fclose(&f);

    ext4_mode_set(target_path, mode);

    struct ext4_inode stat_buf;
    uint32_t ino = 0;
    r = ext4_raw_inode_fill(target_path, &ino, &stat_buf);
    if (r != EOK) return -1;

    vfs_node_t *node = malloc_alloc(sizeof(vfs_node_t));
    if (!node) return -1;
    memset(node, 0, sizeof(vfs_node_t));

    strncpy(node->name, target_path, 255);
    node->inode_num = ino;
    node->size = 0;
    node->mode = mode;
    node->uid = stat_buf.uid;
    node->gid = stat_buf.gid;
    node->refcount = 1;
    node->type = VFS_TYPE_FILE;
    node->mount = dir->mount;
    node->ops = &g_ext4_node_ops;
    node->file_ops = &g_ext4_file_ops;

    *out_node = node;
    return 0;
}

static int ext4_vfs_op_mkdir(vfs_node_t *dir, const char *name, uint16_t mode) {
    if (!dir || !name) return -1;

    char parent_path[512];
    get_lwext4_path(dir, parent_path, sizeof(parent_path));

    char target_path[512];
    if (strcmp(parent_path, "/") == 0)
        snprintf(target_path, sizeof(target_path), "/%s", name);
    else
        snprintf(target_path, sizeof(target_path), "%s/%s", parent_path, name);

    int r = ext4_dir_mk(target_path);
    if (r != EOK) return -1;

    ext4_mode_set(target_path, mode | 0x4000);
    return 0;
}

static int ext4_vfs_op_unlink(vfs_node_t *dir, const char *name) {
    if (!dir || !name) return -1;

    char parent_path[512];
    get_lwext4_path(dir, parent_path, sizeof(parent_path));

    char target_path[512];
    if (strcmp(parent_path, "/") == 0)
        snprintf(target_path, sizeof(target_path), "/%s", name);
    else
        snprintf(target_path, sizeof(target_path), "%s/%s", parent_path, name);

    int r = ext4_fremove(target_path);
    return (r == EOK) ? 0 : -1;
}

static int ext4_vfs_op_rmdir(vfs_node_t *dir, const char *name) {
    if (!dir || !name) return -1;

    char parent_path[512];
    get_lwext4_path(dir, parent_path, sizeof(parent_path));

    char target_path[512];
    if (strcmp(parent_path, "/") == 0)
        snprintf(target_path, sizeof(target_path), "/%s", name);
    else
        snprintf(target_path, sizeof(target_path), "%s/%s", parent_path, name);

    int r = ext4_dir_rm(target_path);
    return (r == EOK) ? 0 : -1;
}

static int ext4_vfs_op_symlink(vfs_node_t *dir, const char *name, const char *target) {
    if (!dir || !name || !target) return -1;

    char parent_path[512];
    get_lwext4_path(dir, parent_path, sizeof(parent_path));

    char link_path[512];
    if (strcmp(parent_path, "/") == 0)
        snprintf(link_path, sizeof(link_path), "/%s", name);
    else
        snprintf(link_path, sizeof(link_path), "%s/%s", parent_path, name);

    int r = ext4_fsymlink(target, link_path);
    return (r == EOK) ? 0 : -1;
}

static int ext4_vfs_op_link(vfs_node_t *dir, vfs_node_t *old_node, const char *name) {
    if (!dir || !old_node || !name) return -1;

    char old_path[512];
    get_lwext4_path(old_node, old_path, sizeof(old_path));

    char parent_path[512];
    get_lwext4_path(dir, parent_path, sizeof(parent_path));

    char new_path[512];
    if (strcmp(parent_path, "/") == 0)
        snprintf(new_path, sizeof(new_path), "/%s", name);
    else
        snprintf(new_path, sizeof(new_path), "%s/%s", parent_path, name);

    int r = ext4_flink(old_path, new_path);
    return (r == EOK) ? 0 : -1;
}

static int ext4_vfs_op_chmod(vfs_node_t *node, uint32_t mode) {
    if (!node) return -1;

    char path[512];
    get_lwext4_path(node, path, sizeof(path));

    int r = ext4_mode_set(path, mode);
    if (r != EOK) return -1;

    node->mode = mode;
    return 0;
}

static int ext4_vfs_op_chown(vfs_node_t *node, uint32_t uid, uint32_t gid) {
    if (!node) return -1;

    char path[512];
    get_lwext4_path(node, path, sizeof(path));

    int r = ext4_owner_set(path, uid, gid);
    if (r != EOK) return -1;

    node->uid = uid;
    node->gid = gid;
    return 0;
}

static int ext4_vfs_op_ioctl(vfs_file_t *file, uint64_t request, void *arg) {
    ext4_vfs_handle_t *h = ext4_ensure_handle(file);
    if (!h) return -1;

    switch (request) {
        case 0x5413:
            if (arg) *(uint64_t *)arg = file->node->size;
            return 0;
        default:
            return -ENOTTY;
    }
}

static void *ext4_vfs_op_mmap(vfs_file_t *file, void *addr, uint64_t length, int prot, int flags, uint64_t offset) {
    ext4_vfs_handle_t *h = ext4_ensure_handle(file);
    if (!h || h->is_dir || length == 0) return MAP_FAILED;

    process_t *proc = get_current_process();
    if (!proc || !proc->pml4) return MAP_FAILED;

    uint64_t pages = (length + 4095) / 4096;
    uint64_t virt;

    if (!proc->mmap_base) proc->mmap_base = 0x5000000000ULL;

    if (addr && (flags & MAP_FIXED)) {
        virt = (uint64_t)addr;
        if (virt + pages * 4096 > proc->mmap_base) {
            proc->mmap_base = (virt + pages * 4096 + 0xFFF) & ~0xFFFULL;
        }
    } else if (addr) {
        virt = (uint64_t)addr;
    } else {
        virt = proc->mmap_base;
        proc->mmap_base += pages * 4096;
    }

    void *phys = pmm_alloc_pages(pages);
    if (!phys) return MAP_FAILED;

    uint64_t phys_addr = (uint64_t)phys;
    uint64_t hhdm = get_bootinfo().hhdm->offset;
    memset((void *)(phys_addr + hhdm), 0, pages * 4096);

    uint64_t vmm_flags = PTE_PRESENT | PTE_USER;
    if (prot & PROT_WRITE) vmm_flags |= PTE_WRITABLE;

    for (uint64_t i = 0; i < pages; i++) {
        vmm_map_page(proc->pml4, phys_addr + i * 4096, virt + i * 4096, vmm_flags);
    }

    ext4_fseek(&h->file, offset, SEEK_SET);
    size_t read_bytes = 0;
    ext4_fread(&h->file, (void *)(phys_addr + hhdm), length, &read_bytes);

    return (void *)virt;
}

vfs_mount_t *ext4_vfs_mount(const char *device, void *data) {
    if (g_ext4_count >= 4) return NULL;

    uint64_t start_lba = (uint64_t)data;

    struct partition_table *part = get_partition_struct(device);
    if (!part) return NULL;

    int part_index = -1;
    for (uint32_t i = 0; i < part->count; i++) {
        if (part->parts[i].start_lba == start_lba) {
            part_index = i;
            break;
        }
    }

    if (part_index < 0) return NULL;

    boldon_ext4_bdev_t *dev = &g_ext4_bdevs[g_ext4_count++];

    strncpy(dev->dev_name, device, 31);
    dev->start_lba = start_lba;

    uint64_t blocks_count = part->parts[part_index].sector_count;

    dev->iface.open = boldon_ext4_open;
    dev->iface.bread = boldon_ext4_bread;
    dev->iface.bwrite = boldon_ext4_bwrite;
    dev->iface.close = boldon_ext4_close;
    dev->iface.ph_bsize = 512;
    dev->iface.ph_bcnt = blocks_count;
    dev->iface.p_user = dev;
    dev->iface.ph_bbuf = malloc_alloc(65536);

    dev->bd.bdif = &dev->iface;
    dev->bd.part_offset = 0;
    dev->bd.part_size = blocks_count * 512;

    char mp_name[16];
    snprintf(mp_name, sizeof(mp_name), "ext4_%d", g_ext4_count);

    int r = ext4_device_register(&dev->bd, mp_name);
    if (r != EOK) return NULL;

    char mount_path[4] = { '/', '\0', '\0', '\0' };
    r = ext4_mount(mp_name, mount_path, false);
    if (r != EOK) {
        return NULL;
    }
    (void)ext4_mount_setup_locks(mount_path, &ext4_os_locks);

    vfs_mount_t *mnt = malloc_alloc(sizeof(vfs_mount_t));
    if (!mnt) return NULL;

    vfs_node_t *root = malloc_alloc(sizeof(vfs_node_t));
    if (!root) {
        malloc_free(mnt);
        return NULL;
    }

    memset(root, 0, sizeof(vfs_node_t));
    strncpy(root->name, "", 255);
    root->inode_num = 2;
    root->type = VFS_TYPE_DIR;
    root->refcount = 1;
    root->mount = mnt;
    root->ops = &g_ext4_node_ops;
    root->file_ops = &g_ext4_file_ops;

    mnt->root = root;
    mnt->fs_data = dev;

    return mnt;
}

void ext4_vfs_init(void) {
    g_ext4_file_ops.read = ext4_vfs_op_read;
    g_ext4_file_ops.write = ext4_vfs_op_write;
    g_ext4_file_ops.close = ext4_vfs_op_close;
    g_ext4_file_ops.lseek = ext4_vfs_op_lseek;
    g_ext4_file_ops.ioctl = ext4_vfs_op_ioctl;
    g_ext4_file_ops.readdir = ext4_vfs_op_readdir;
    g_ext4_file_ops.mmap = ext4_vfs_op_mmap;

    g_ext4_node_ops.lookup = ext4_vfs_op_lookup;
    g_ext4_node_ops.create = ext4_vfs_op_create;
    g_ext4_node_ops.mkdir = ext4_vfs_op_mkdir;
    g_ext4_node_ops.unlink = ext4_vfs_op_unlink;
    g_ext4_node_ops.rmdir = ext4_vfs_op_rmdir;
    g_ext4_node_ops.symlink = ext4_vfs_op_symlink;
    g_ext4_node_ops.link = ext4_vfs_op_link;
    g_ext4_node_ops.readlink = ext4_vfs_op_readlink;
    g_ext4_node_ops.stat = ext4_vfs_op_stat;
    g_ext4_node_ops.chmod = ext4_vfs_op_chmod;
    g_ext4_node_ops.chown = ext4_vfs_op_chown;

    g_ext4_fs_type.mount = ext4_vfs_mount;
    g_ext4_fs_type.name = "ext4";

    vfs_register_fs(&g_ext4_fs_type);
}