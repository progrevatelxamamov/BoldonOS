#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>
#include <memory.h>
#include <stdio.h>

#include <kernel.h>
#include <gconsole.h>
#include <disk/block.h>
#include <disk/bcache.h>
#include <disk/partitions.h>
#include <fs/vfs.h>
#include <fs/mount.h>
#include <fs/devfs.h>
#include <mm/malloc.h>
#include <mm/pmm.h>
#include <scheduler/scheduler.h>
#include <scheduler/spinlock.h>
#include <cpu/time.h>

#ifndef POLLIN
#define POLLIN  0x0001
#define POLLOUT 0x0004
#endif

#define BLKSSZGET       0x1268u
#define BLKGETSIZE      0x1260u
#define BLKRRPART       0x125Fu
#define BLKFLSBUF       0x1261u
#define BLKPG           0x1269u
#define BLKPBSZGET      0x127Bu
#define BLKGETSIZE64    0x80081272u
#define HDIO_GETGEO     0x0301u

#define MAX_BLOCK_NODES 96
#define MAX_KNOWN_DISKS 32

struct hd_geometry {
    uint8_t  heads;
    uint8_t  sectors;
    uint16_t cylinders;
    uint32_t pad;
    uint64_t start;
};

typedef struct {
    char node[32];
    char dev_name[32];
    uint32_t part_index;
    uint64_t start_lba;
    uint64_t sector_count;
    char fstype[16];
    bool used;
} block_node_t;

static block_node_t nodes[MAX_BLOCK_NODES];
static char known[MAX_KNOWN_DISKS][32];
static int known_count = 0;
static uint32_t next_sd = 0;
static char disk_alias[MAX_KNOWN_DISKS][32];
static spinlock_t proc_lock = SPINLOCK_INIT;
static char proc_buffer[4096];
static bool hotplug_ready = false;

static void append(const char *text, uint32_t cap);
static void hotplug_scan(void);

static bool str_eq(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void str_copy(char *dst, const char *src, uint32_t cap) {
    uint32_t i = 0;
    if (!dst || !src || cap == 0) return;
    for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

static const char *strip_dev(const char *name) {
    if (!name) return "";
    if (strncmp(name, "/dev/", 5) == 0) return name + 5;
    if (strncmp(name, "D:/", 3) == 0) return name + 3;
    if (name[0] == '/') return name + 1;
    return name;
}

static int find_node(const char *name) {
    for (int i = 0; i < MAX_BLOCK_NODES; i++)
        if (nodes[i].used && str_eq(nodes[i].node, name)) return i;
    return -1;
}

static int alloc_node(void) {
    for (int i = 0; i < MAX_BLOCK_NODES; i++)
        if (!nodes[i].used) return i;
    return -1;
}

static uint64_t node_size(block_node_t *n) {
    struct block_device *d = find_device(n->dev_name);
    if (!d) return 0;
    uint32_t ss = d->sector_size ? d->sector_size : 512;
    uint64_t sectors = n->sector_count ? n->sector_count : d->sector_count;
    return sectors * (uint64_t)ss;
}

static int blk_read(vfs_file_t *file, void *buf, uint64_t count) {
    if (!file || !file->node || !buf || count == 0) return 0;
    block_node_t *n = (block_node_t *)file->node->fs_data;
    struct block_device *d = find_device(n->dev_name);
    if (!n || !d || !d->read) return -EIO;

    uint32_t ss = d->sector_size ? d->sector_size : 512;
    if (ss > 4096) return -EINVAL;
    uint64_t limit = node_size(n);
    uint64_t pos = file->offset;
    if (pos >= limit) return 0;
    if (count > limit - pos) count = limit - pos;

    uint8_t *tmp = (uint8_t *)malloc_alloc(4096);
    if (!tmp) return -ENOMEM;

    uint8_t *out = (uint8_t *)buf;
    uint64_t done = 0;
    while (done < count) {
        uint64_t cur = pos + done;
        uint64_t lba = cur / ss;
        uint32_t skip = (uint32_t)(cur % ss);
        uint64_t want = count - done;
        if (want > 4096 - skip) want = 4096 - skip;
        uint32_t sectors = (uint32_t)((want + skip + ss - 1) / ss);
        if (sectors == 0 || sectors > 8) sectors = 8;

        if (block_read(n->dev_name, n->start_lba + lba, sectors, tmp) != 0) break;
        uint64_t take = (uint64_t)sectors * ss - skip;
        if (take > count - done) take = count - done;
        memcpy(out + done, tmp + skip, take);
        done += take;
    }

    malloc_free(tmp);
    file->offset = pos + done;
    return (int)done;
}

static int blk_write(vfs_file_t *file, const void *buf, uint64_t count) {
    if (!file || !file->node || !buf || count == 0) return 0;
    block_node_t *n = (block_node_t *)file->node->fs_data;
    struct block_device *d = find_device(n->dev_name);
    if (!n || !d || !d->write) return -EIO;

    uint32_t ss = d->sector_size ? d->sector_size : 512;
    if (ss > 4096) return -EINVAL;
    uint64_t limit = node_size(n);
    uint64_t pos = file->offset;
    if (pos >= limit) return 0;
    if (count > limit - pos) count = limit - pos;

    uint8_t *tmp = (uint8_t *)malloc_alloc(4096);
    if (!tmp) return -ENOMEM;

    const uint8_t *in = (const uint8_t *)buf;
    uint64_t done = 0;
    while (done < count) {
        uint64_t cur = pos + done;
        uint64_t lba = cur / ss;
        uint32_t skip = (uint32_t)(cur % ss);
        uint64_t want = count - done;
        if (want > 4096 - skip) want = 4096 - skip;
        uint32_t sectors = (uint32_t)((want + skip + ss - 1) / ss);
        if (sectors == 0 || sectors > 8) sectors = 8;

        memset(tmp, 0, 4096);
        if (skip || (uint64_t)sectors * ss - skip < count - done)
            block_read(n->dev_name, n->start_lba + lba, sectors, tmp);
        uint64_t take = (uint64_t)sectors * ss - skip;
        if (take > count - done) take = count - done;
        memcpy(tmp + skip, in + done, take);
        if (block_write(n->dev_name, n->start_lba + lba, sectors, tmp) != 0) break;
        done += take;
    }

    malloc_free(tmp);
    file->offset = pos + done;
    return (int)done;
}

static uint64_t blk_lseek(vfs_file_t *file, int64_t offset, int whence) {
    if (!file || !file->node) return (uint64_t)-EINVAL;
    block_node_t *n = (block_node_t *)file->node->fs_data;
    if (!n) return (uint64_t)-EINVAL;
    uint64_t size = node_size(n);
    int64_t base = (int64_t)file->offset;
    if (whence == VFS_SEEK_SET) base = 0;
    else if (whence == VFS_SEEK_END) base = (int64_t)size;
    int64_t result = base + offset;
    if (result < 0) return (uint64_t)-EINVAL;
    file->offset = (uint64_t)result;
    return (uint64_t)result;
}

static int blk_ioctl(vfs_file_t *file, uint64_t request, void *arg) {
    if (!file || !file->node) return -ENODEV;
    block_node_t *n = (block_node_t *)file->node->fs_data;
    struct block_device *d = n ? find_device(n->dev_name) : NULL;
    if (!n || !d) return -ENODEV;

    uint32_t ss = d->sector_size ? d->sector_size : 512;
    uint64_t sectors = n->sector_count ? n->sector_count : d->sector_count;

    if (request == BLKGETSIZE) {
        if (!arg) return -EFAULT;
        *(uint64_t *)arg = sectors;
        return 0;
    }
    if (request == BLKGETSIZE64) {
        if (!arg) return -EFAULT;
        *(uint64_t *)arg = sectors * (uint64_t)ss;
        return 0;
    }
    if (request == BLKSSZGET || request == BLKPBSZGET) {
        if (!arg) return -EFAULT;
        *(int *)arg = (int)ss;
        return 0;
    }
    if (request == HDIO_GETGEO) {
        if (!arg) return -EFAULT;
        struct hd_geometry *g = (struct hd_geometry *)arg;
        g->pad = 0;
        g->heads = 255;
        g->sectors = 63;
        uint64_t cyl = sectors / (255ULL * 63ULL);
        g->cylinders = (uint16_t)(cyl > 65535 ? 65535 : cyl);
        g->start = n->start_lba;
        return 0;
    }
    if (request == BLKFLSBUF) {
        bcache_flush(n->dev_name);
        return 0;
    }
    if (request == BLKRRPART || request == BLKPG) {
        bcache_flush(n->dev_name);
        bcache_drop_device(n->dev_name);
        (void)partitions_refresh(n->dev_name);
        return 0;
    }
    return -ENOTTY;
}

static uint32_t blk_poll(vfs_file_t *file, uint32_t mask) {
    (void)file;
    return mask & (POLLIN | POLLOUT);
}

static vfs_file_ops_t blk_ops = {
    .read  = blk_read,
    .write = blk_write,
    .lseek = blk_lseek,
    .ioctl = blk_ioctl,
    .poll  = blk_poll
};

static void append(const char *text, uint32_t cap) {
    uint32_t pos = (uint32_t)strlen(proc_buffer);
    while (*text && pos + 1 < cap) proc_buffer[pos++] = *text++;
    proc_buffer[pos] = 0;
}

static int proc_read(vfs_file_t *file, void *buf, uint64_t count) {
    if (!file || !file->node || !buf || count == 0) return 0;
    int id = (int)(uint64_t)(uintptr_t)file->node->fs_data;

    uint64_t flags;
    spin_lock_irqsave(&proc_lock, &flags);
    memset(proc_buffer, 0, sizeof(proc_buffer));
    if (id == 1) {
        append("major minor  #blocks  name\n", sizeof(proc_buffer));
        for (int i = 0; i < MAX_BLOCK_NODES; i++) {
            if (!nodes[i].used) continue;
            char line[128];
            snprintf(line, sizeof(line), "%4u %5u %10llu %s\n",
                     8u, (unsigned)i, (unsigned long long)(nodes[i].sector_count / 2), nodes[i].node);
            append(line, sizeof(proc_buffer));
        }
    } else if (id == 2) {
        vfs_proc_mounts(proc_buffer, sizeof(proc_buffer));
    } else if (id == 3) {
        vfs_proc_mountinfo(proc_buffer, sizeof(proc_buffer));
    } else if (id == 4) {
        append("device\tpartitions\tstatus\n", sizeof(proc_buffer));
        for (int k = 0; k < known_count; k++) {
            char line[160];
            snprintf(line, sizeof(line), "%s\t%s\t%s\n", known[k],
                     disk_alias[k][0] ? disk_alias[k] : "-", "online");
            append(line, sizeof(proc_buffer));
        }
        append("\ncommands: rescan; eject <dev>; mount <dev> <path>; umount <path>\n",
               sizeof(proc_buffer));
    }

    uint32_t len = (uint32_t)strlen(proc_buffer);
    if (file->offset >= len) {
        spin_unlock_irqrestore(&proc_lock, flags);
        return 0;
    }
    uint64_t avail = (uint64_t)len - file->offset;
    if (count > avail) count = avail;
    memcpy(buf, proc_buffer + file->offset, count);
    file->offset += count;
    spin_unlock_irqrestore(&proc_lock, flags);
    return (int)count;
}

static int ctl_write(vfs_file_t *file, const void *buf, uint64_t count) {
    (void)file;
    if (!buf || count == 0) return 0;
    char cmd[256];
    uint32_t n = count < sizeof(cmd) - 1 ? (uint32_t)count : sizeof(cmd) - 1;
    memcpy(cmd, buf, n);
    cmd[n] = 0;

    char *arg = cmd;
    while (*arg == ' ' || *arg == '\t' || *arg == '\n' || *arg == '\r') arg++;
    char *end = arg + strlen(arg);
    while (end > arg && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) *--end = 0;

    if (strncmp(arg, "rescan", 6) == 0) {
        hotplug_scan();
        return (int)count;
    }
    if (strncmp(arg, "eject ", 6) == 0) {
        partitions_device_eject(strip_dev(arg + 6));
        return (int)count;
    }
    if (strncmp(arg, "mount ", 6) == 0) {
        char *rest = arg + 6;
        char *sep = (char *)strchr(rest, ' ');
        if (sep) {
            *sep = 0;
            char *path = sep + 1;
            while (*path == ' ') path++;
            vfs_mount(rest, path, "auto", 0, NULL);
        }
        return (int)count;
    }
    if (strncmp(arg, "umount ", 7) == 0) {
        vfs_umount(arg + 7, 0);
        return (int)count;
    }
    return (int)count;
}

static vfs_file_ops_t proc_ops = {
    .read = proc_read
};

static vfs_file_ops_t ctl_ops = {
    .read  = proc_read,
    .write = ctl_write
};

static int node_add(const char *name, const char *dev, uint32_t index,
                    uint64_t lba, uint64_t sectors, const char *fstype) {
    int slot = find_node(name);
    if (slot < 0) slot = alloc_node();
    if (slot < 0) return -1;

    block_node_t *n = &nodes[slot];
    memset(n, 0, sizeof(*n));
    str_copy(n->node, name, sizeof(n->node));
    str_copy(n->dev_name, dev, sizeof(n->dev_name));
    n->part_index = index;
    n->start_lba = lba;
    n->sector_count = sectors;
    if (fstype) str_copy(n->fstype, fstype, sizeof(n->fstype));
    n->used = true;

    devfs_register_device(name, VFS_TYPE_BLOCK_DEV, &blk_ops, n);

    char path[64];
    snprintf(path, sizeof(path), "/dev/%s", name);
    vfs_node_t *vn = vfs_resolve_path(path);
    if (vn) {
        struct block_device *bd = find_device(dev);
        uint32_t ss = bd && bd->sector_size ? bd->sector_size : 512;
        vn->size = sectors * (uint64_t)ss;
    }
    return 0;
}

static void node_remove_device(const char *dev) {
    for (int i = 0; i < MAX_BLOCK_NODES; i++) {
        if (!nodes[i].used || !str_eq(nodes[i].dev_name, dev)) continue;
        devfs_unregister_device(nodes[i].node);
        memset(&nodes[i], 0, sizeof(block_node_t));
    }
}

static void node_remove_partitions(const char *dev) {
    for (int i = 0; i < MAX_BLOCK_NODES; i++) {
        if (!nodes[i].used || nodes[i].part_index == 0) continue;
        if (!str_eq(nodes[i].dev_name, dev)) continue;
        devfs_unregister_device(nodes[i].node);
        memset(&nodes[i], 0, sizeof(block_node_t));
    }
}

static struct partition_table *build_table(const char *dev) {
    struct block_device *d = find_device(dev);
    if (!d || d->sector_count == 0) return NULL;

    struct partition_table *pt = osdev_get_partition_struct(d->name);
    if (pt) return pt;

    pt = (struct partition_table *)malloc_alloc(sizeof(struct partition_table));
    if (!pt) return NULL;
    memset(pt, 0, sizeof(*pt));
    pt->block_device_name = d->name;
    pt->scheme = PART_SCHEME_MBR;
    pt->count = 1;
    pt->parts[0].start_lba = 0;
    pt->parts[0].sector_count = d->sector_count;
    pt->parts[0].type = PART_TYPE_UNKNOWN;
    return pt;
}

static const char *alias_for(const char *dev) {
    for (int k = 0; k < known_count; k++)
        if (str_eq(known[k], dev)) return disk_alias[k];
    return NULL;
}

static int register_partition_nodes(const char *dev, struct partition_table *pt,
                                    const char *alias) {
    struct block_device *d = find_device(dev);
    if (!d) return -1;

    node_add(dev, dev, 0, 0, d->sector_count, NULL);
    if (alias && alias[0]) node_add(alias, dev, 0, 0, d->sector_count, NULL);

    for (uint32_t i = 0; i < pt->count; i++) {
        uint64_t lba = pt->parts[i].start_lba;
        uint64_t sectors = pt->parts[i].sector_count;
        if (sectors == 0) continue;
        const char *fs = partitions_detect_fs(dev, lba);

        char name[32], alias_name[32];
        snprintf(name, sizeof(name), "%sp%u", dev, i + 1);
        node_add(name, dev, i + 1, lba, sectors, fs);

        if (alias && alias[0]) {
            if (strncmp(alias, "nvme", 4) == 0 || strncmp(alias, "mmcblk", 6) == 0)
                snprintf(alias_name, sizeof(alias_name), "%sp%u", alias, i + 1);
            else
                snprintf(alias_name, sizeof(alias_name), "%s%u", alias, i + 1);
            node_add(alias_name, dev, i + 1, lba, sectors, fs);
        }
    }
    return 0;
}

static void auto_mount_partitions(const char *dev, struct partition_table *pt,
                                  const char *alias) {
    for (uint32_t i = 0; i < pt->count; i++) {
        uint64_t lba = pt->parts[i].start_lba;
        if (pt->parts[i].sector_count == 0) continue;
        const char *fs = partitions_detect_fs(dev, lba);
        if (!fs) continue;

        char name[64], target[128];
        snprintf(name, sizeof(name), "/dev/%s", alias && alias[0] ? alias : dev);
        if (i < pt->count && alias && alias[0]) {
            if (strncmp(alias, "nvme", 4) == 0 || strncmp(alias, "mmcblk", 6) == 0)
                snprintf(name, sizeof(name), "/dev/%sp%u", alias, i + 1);
            else
                snprintf(name, sizeof(name), "/dev/%s%u", alias, i + 1);
        } else {
            snprintf(name, sizeof(name), "/dev/%sp%u", dev, i + 1);
        }
        snprintf(target, sizeof(target), "/media/%s", strip_dev(name));
        if (vfs_mount(name, target, fs, 0, NULL) == 0) {
            console_print_string("[HOTPLUG] mounted ");
            console_print_string(name);
            console_print_string(" at ");
            console_print_string(target);
            console_print_string("\n");
        }
    }
}

static int device_add(const char *dev, bool auto_mount) {
    struct block_device *d = find_device(dev);
    if (!d) return -ENODEV;
    if (alias_for(dev)) return 0;

    struct partition_table *pt = get_partition_struct(d->name);
    if (!pt) pt = build_table(d->name);
    if (!pt) return -EIO;

    if (partitions_add_table(pt) != 0) {
        malloc_free(pt);
        return -ENOMEM;
    }

    char alias[32];
    char sd[16];
    snprintf(sd, sizeof(sd), "sd%c", (char)('a' + (next_sd++ % 26)));
    str_copy(alias, sd, sizeof(alias));

    if (known_count < MAX_KNOWN_DISKS) {
        str_copy(known[known_count], dev, sizeof(known[known_count]));
        str_copy(disk_alias[known_count], alias, sizeof(disk_alias[known_count]));
        known_count++;
    }

    (void)register_partition_nodes(dev, pt, alias);
    int root_res = -1;
    if (!vfs_get_mount('C')) root_res = partitions_try_mount_root(dev);
    if (root_res == 0) console_print_string("[HOTPLUG] root filesystem became available as C:\n");
    if (auto_mount && root_res != 0) auto_mount_partitions(dev, pt, alias);
    return 0;
}

void partitions_device_remove(const char *name) {
    if (!name) return;
    const char *dev = strip_dev(name);

    for (char c = 'E'; c <= 'Z'; c++) {
        vfs_mount_t *mnt = vfs_get_mount(c);
        if (mnt && str_eq(mnt->device, dev)) vfs_umount_fs(c);
    }

    bcache_invalidate_device(dev);
    node_remove_device(dev);
    partitions_remove_table(dev);

    for (int k = 0; k < known_count; k++) {
        if (!str_eq(known[k], dev)) continue;
        for (int j = k + 1; j < known_count; j++) {
            memcpy(known[j - 1], known[j], sizeof(known[j - 1]));
            memcpy(disk_alias[j - 1], disk_alias[j], sizeof(disk_alias[j - 1]));
        }
        memset(known[known_count - 1], 0, sizeof(known[known_count - 1]));
        memset(disk_alias[known_count - 1], 0, sizeof(disk_alias[known_count - 1]));
        known_count--;
        break;
    }
}

void partitions_device_eject(const char *name) {
    if (!name) return;
    const char *dev = strip_dev(name);
    bcache_flush(dev);
    partitions_device_remove(dev);
}

int partitions_refresh(const char *name) {
    const char *dev = strip_dev(name);
    struct block_device *d = find_device(dev);
    if (!d) return -ENODEV;

    struct partition_table *pt = build_table(d->name);
    if (!pt) return -EIO;
    if (partitions_add_table(pt) != 0) {
        if (get_partition_struct(dev) != pt) malloc_free(pt);
        return -ENOMEM;
    }

    node_remove_partitions(dev);
    const char *alias = alias_for(dev);
    for (uint32_t i = 0; i < pt->count; i++) {
        uint64_t lba = pt->parts[i].start_lba;
        uint64_t sectors = pt->parts[i].sector_count;
        if (sectors == 0) continue;
        const char *fs = partitions_detect_fs(dev, lba);

        char pname[32];
        snprintf(pname, sizeof(pname), "%sp%u", dev, i + 1);
        node_add(pname, dev, i + 1, lba, sectors, fs);

        if (alias && alias[0]) {
            char aname[32];
            if (strncmp(alias, "nvme", 4) == 0 || strncmp(alias, "mmcblk", 6) == 0)
                snprintf(aname, sizeof(aname), "%sp%u", alias, i + 1);
            else
                snprintf(aname, sizeof(aname), "%s%u", alias, i + 1);
            node_add(aname, dev, i + 1, lba, sectors, fs);
        }
    }
    return 0;
}

int partitions_resolve_source(const char *source, char *dev_out,
                              uint64_t *lba_out, const char **fstype_out) {
    if (!source || !dev_out || !lba_out) return -EINVAL;
    const char *name = strip_dev(source);

    for (int i = 0; i < MAX_BLOCK_NODES; i++) {
        if (!nodes[i].used || !str_eq(nodes[i].node, name)) continue;
        str_copy(dev_out, nodes[i].dev_name, 32);
        *lba_out = nodes[i].start_lba;
        if (fstype_out) *fstype_out = nodes[i].fstype[0] ? nodes[i].fstype : NULL;
        return 0;
    }

    struct block_device *d = find_device(name);
    if (d) {
        str_copy(dev_out, name, 32);
        *lba_out = 0;
        if (fstype_out) *fstype_out = NULL;
        return 0;
    }
    return -ENODEV;
}

int partitions_node_for(const char *dev, uint64_t lba, char *out, uint32_t cap) {
    if (!dev || !out || cap < 8) return -1;
    for (int i = 0; i < MAX_BLOCK_NODES; i++) {
        if (!nodes[i].used || nodes[i].part_index == 0) continue;
        if (!str_eq(nodes[i].dev_name, dev) || nodes[i].start_lba != lba) continue;
        snprintf(out, cap, "/dev/%s", nodes[i].node);
        return 0;
    }
    for (int i = 0; i < MAX_BLOCK_NODES; i++) {
        if (!nodes[i].used || nodes[i].part_index != 0) continue;
        if (!str_eq(nodes[i].dev_name, dev) || nodes[i].start_lba != lba) continue;
        snprintf(out, cap, "/dev/%s", nodes[i].node);
        return 0;
    }
    return -1;
}

int partitions_proc_partitions(char *buf, uint32_t len) {
    if (!buf || len < 32) return 0;
    uint32_t pos = 0;
    const char *header = "major minor  #blocks  name\n";
    while (*header && pos + 1 < len) buf[pos++] = *header++;
    for (int i = 0; i < MAX_BLOCK_NODES; i++) {
        if (!nodes[i].used) continue;
        char line[128];
        snprintf(line, sizeof(line), "%4u %5u %10llu %s\n",
                 8u, (unsigned)i, (unsigned long long)(nodes[i].sector_count / 2), nodes[i].node);
        char *p = line;
        while (*p && pos + 1 < len) buf[pos++] = *p++;
    }
    buf[pos] = 0;
    return (int)pos;
}

static void hotplug_scan(void) {
    for (int k = 0; k < known_count;) {
        if (find_device(known[k])) { k++; continue; }
        char name[32];
        str_copy(name, known[k], sizeof(name));
        partitions_device_remove(name);
        k = 0;
    }

    for (int i = 0; i < MAX_BLOCK_DEVICES; i++) {
        struct block_device *d = &block_devices[i];
        if (d->sector_count == 0 || d->name[0] == 0) continue;
        if (alias_for(d->name)) continue;
        char name[32];
        str_copy(name, d->name, sizeof(name));
        device_add(name, true);
    }
}

static void hotplug_task(void) {
    uint64_t start = get_monotonic_ms();
    uint64_t next = start + 200;
    uint64_t next_root = start + 2000;
    for (;;) {
        uint64_t now = get_monotonic_ms();
        if (hotplug_ready && now >= next) {
            next = now + 200;
            hotplug_scan();
        }
        if (hotplug_ready && !vfs_get_mount('C') && now >= next_root) {
            next_root = now + 2000;
            if (partitions_try_mount_root(NULL) == 0)
                console_print_string("[HOTPLUG] root filesystem became available as C:\n");
        }
        schedule_yield();
    }
}

void partitions_hotplug_start(void) {
    devfs_register_device("proc/partitions", VFS_TYPE_CHAR_DEV, &proc_ops, (void *)(uintptr_t)1);
    devfs_register_device("proc/mounts",     VFS_TYPE_CHAR_DEV, &proc_ops, (void *)(uintptr_t)2);
    devfs_register_device("proc/mountinfo",  VFS_TYPE_CHAR_DEV, &proc_ops, (void *)(uintptr_t)3);
    devfs_register_device("diskctl",         VFS_TYPE_CHAR_DEV, &ctl_ops,  (void *)(uintptr_t)4);

    for (int i = 0; i < MAX_BLOCK_DEVICES; i++) {
        struct block_device *d = &block_devices[i];
        if (d->sector_count == 0 || d->name[0] == 0) continue;
        struct partition_table *pt = get_partition_struct(d->name);
        if (!pt) pt = build_table(d->name);
        if (pt) partitions_add_table(pt);

        char alias[32];
        char sd[16];
        snprintf(sd, sizeof(sd), "sd%c", (char)('a' + (next_sd++ % 26)));
        str_copy(alias, sd, sizeof(alias));

        if (known_count < MAX_KNOWN_DISKS) {
            str_copy(known[known_count], d->name, sizeof(known[known_count]));
            str_copy(disk_alias[known_count], alias, sizeof(disk_alias[known_count]));
            known_count++;
        }
        if (pt) (void)register_partition_nodes(d->name, pt, alias);
    }

    console_print_string("[HOTPLUG] disks found: ");
    console_print_num(known_count);
    console_print_string("\n");

    if (!vfs_get_mount('C')) partitions_try_mount_root(NULL);

    hotplug_ready = true;
    create_task_kernel((void *)hotplug_task, "hotplug");
}
