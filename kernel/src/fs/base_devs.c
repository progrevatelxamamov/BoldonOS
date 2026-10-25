#include <fs/vfs.h>
#include <fs/devfs.h>
#include <errno.h>
#include <memory.h>
#include <stdint.h>
#include <cpu/time.h>

static int null_read(vfs_file_t *f, void *buf, uint64_t count) {
    (void)f; (void)buf; (void)count;
    return 0;
}

static int null_write(vfs_file_t *f, const void *buf, uint64_t count) {
    (void)f; (void)buf;
    return (int)count;
}

static int zero_read(vfs_file_t *f, void *buf, uint64_t count) {
    (void)f;
    if (buf && count > 0) {
        memset(buf, 0, count);
    }
    return (int)count;
}

static int zero_write(vfs_file_t *f, const void *buf, uint64_t count) {
    (void)f; (void)buf;
    return (int)count;
}

static uint32_t rng_state = 0;

static int urandom_read(vfs_file_t *f, void *buf, uint64_t count) {
    (void)f;
    rng_state = (uint32_t)get_unix_time() / 2;
    if (!buf) return -EFAULT;

    uint8_t *p = (uint8_t *)buf;
    for (uint64_t i = 0; i < count; i++) {
        rng_state = rng_state * 1103515245 + 12345;
        p[i] = (uint8_t)(rng_state >> 16);
    }
    return (int)count;
}

static int urandom_write(vfs_file_t *f, const void *buf, uint64_t count) {
    (void)f; (void)buf;
    return (int)count;
}

static int full_write(vfs_file_t *f, const void *buf, uint64_t count) {
    (void)f; (void)buf; (void)count;
    return -ENOSPC;
}

static vfs_file_ops_t null_ops    = { .read = null_read,    .write = null_write };
static vfs_file_ops_t zero_ops    = { .read = zero_read,    .write = zero_write };
static vfs_file_ops_t urandom_ops = { .read = urandom_read, .write = urandom_write };
static vfs_file_ops_t full_ops    = { .read = zero_read,    .write = full_write };

void base_devs_register(void) {
    devfs_register_device("null",    VFS_TYPE_CHAR_DEV, &null_ops, NULL);
    devfs_register_device("zero",    VFS_TYPE_CHAR_DEV, &zero_ops, NULL);
    devfs_register_device("random",  VFS_TYPE_CHAR_DEV, &urandom_ops, NULL);
    devfs_register_device("urandom", VFS_TYPE_CHAR_DEV, &urandom_ops, NULL);
    devfs_register_device("full",    VFS_TYPE_CHAR_DEV, &full_ops, NULL);
}