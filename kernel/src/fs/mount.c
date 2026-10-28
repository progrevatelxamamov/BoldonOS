#include <fs/mount.h>
#include <fs/vfs.h>
#include <disk/partitions.h>
#include <disk/bcache.h>
#include <mm/malloc.h>
#include <errno.h>
#include <memory.h>
#include <string.h>
#include <stdio.h>

static char upper_letter(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - 32);
    return c;
}

static bool is_drive_target(const char *p) {
    if (!p) return false;
    char c = upper_letter(p[0]);
    if (c < 'A' || c > 'Z' || p[1] != ':') return false;
    return p[2] == '\0' || p[2] == '/' || p[2] == '\\' || p[2] == '.';
}

int vfs_mount(const char *source, const char *target, const char *fstype,
              uint32_t flags, const void *data) {
    (void)flags;
    (void)data;
    if (!source || !target) return -EINVAL;

    char dev[32];
    uint64_t lba = 0;
    const char *detected = NULL;
    memset(dev, 0, sizeof(dev));

    int res = partitions_resolve_source(source, dev, &lba, &detected);
    if (res) return res;

    const char *fs = fstype;
    if (!fs || !fs[0] || strcmp(fs, "auto") == 0) fs = detected;
    if (!fs || !fs[0]) return -EINVAL;
    if (strcmp(fs, "vfat") == 0 || strcmp(fs, "msdos") == 0 ||
        strcmp(fs, "fat") == 0 || strcmp(fs, "fat32") == 0) fs = "fat32";
    else if (strcmp(fs, "ext4") == 0) fs = "ext4";
    else return -EINVAL;

    char letter = 0;
    bool bind = false;
    if (is_drive_target(target)) {
        letter = upper_letter(target[0]);
    } else {
        letter = vfs_alloc_drive_letter();
        bind = true;
    }
    if (!letter) return -ENOSPC;
    if (vfs_drive_mounted(letter)) return -EBUSY;

    res = vfs_mount_fs(fs, dev, letter, (void *)lba);
    if (res) return res;

    if (bind) {
        if (vfs_bind_mountpoint(target, letter) != 0) {
            vfs_umount_fs(letter);
            return -ENOMEM;
        }
    }
    return 0;
}

int vfs_umount(const char *target, uint32_t flags) {
    (void)flags;
    if (!target) return -EINVAL;

    char letter = 0;
    if (is_drive_target(target)) {
        letter = upper_letter(target[0]);
    } else {
        char *norm = normalize_path(target);
        if (!norm) return -EINVAL;
        if (norm[0] >= 'A' && norm[0] <= 'Z' && norm[1] == ':') letter = norm[0];
        malloc_free(norm);
    }
    if (!letter) return -EINVAL;
    vfs_mount_t *mnt = vfs_get_mount(letter);
    if (!mnt) return -EINVAL;
    if (mnt->device[0]) bcache_flush(mnt->device);
    return vfs_umount_fs(letter);
}

static void append(char *buf, uint32_t cap, uint32_t *pos, const char *text) {
    while (*text && *pos + 1 < cap) {
        buf[(*pos)++] = *text++;
    }
    buf[*pos] = 0;
}

int vfs_proc_mounts(char *buf, uint32_t len) {
    if (!buf || len < 8) return 0;
    memset(buf, 0, len);
    uint32_t pos = 0;
    char line[256];

    append(buf, len, &pos, "devfs /dev devfs rw 0 0\n");

    for (char c = 'A'; c <= 'Z'; c++) {
        vfs_mount_t *mnt = vfs_get_mount(c);
        if (!mnt || mnt->dead) continue;

        char source[64];
        if (partitions_node_for(mnt->device, mnt->start_lba, source, sizeof(source)) != 0) {
            snprintf(source, sizeof(source), "/dev/%s", mnt->device[0] ? mnt->device : "root");
        }

        const char *target = vfs_mountpoint_path(c);
        if (!target) {
            if (c == 'C') target = "/";
            else target = "none";
        } else if (target[2] == '\0') {
            target = "/";
        } else {
            target += 2;
        }

        snprintf(line, sizeof(line), "%s %s %s rw 0 0\n",
                 source, target, mnt->fs_name[0] ? mnt->fs_name : "none");
        append(buf, len, &pos, line);
    }
    return (int)pos;
}

int vfs_proc_mountinfo(char *buf, uint32_t len) {
    if (!buf || len < 16) return 0;
    char tmp[2048];
    int n = vfs_proc_mounts(tmp, sizeof(tmp));
    (void)n;

    memset(buf, 0, len);
    uint32_t pos = 0;
    uint32_t id = 1;
    char *line = tmp;
    while (*line && pos + 1 < len) {
        char *end = (char *)strchr(line, '\n');
        if (end) *end = 0;
        char out[256];
        snprintf(out, sizeof(out), "%u 1 0:1 / %s - - rw\n", id++, line);
        append(buf, len, &pos, out);
        if (!end) break;
        line = end + 1;
    }
    return (int)pos;
}
