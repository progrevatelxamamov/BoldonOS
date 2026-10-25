#include <fs/shmem.h>
#include <fs/vfs.h>
#include <mm/malloc.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <memory.h>
#include <scheduler/scheduler.h>
#include <kernel.h>
#include <bootloader.h>
#include <sys/mman.h>
#include <errno.h>

#define SHMEM_MAX        128
#define SHMEM_MAX_PAGES  1024
#define SHMEM_PAGE_SIZE  4096

typedef struct {
    bool     used;
    bool     sealed;
    uint64_t size;
    void    *pages[SHMEM_MAX_PAGES];
    char     name[64];
} shmem_object_t;

static shmem_object_t g_shmem[SHMEM_MAX];

static int shmem_read(vfs_file_t *file, void *buf, uint64_t count);
static int shmem_write(vfs_file_t *file, const void *buf, uint64_t count);
static int shmem_close(vfs_file_t *file);
static uint64_t shmem_lseek(vfs_file_t *file, int64_t offset, int whence);
static void *shmem_mmap_ops(vfs_file_t *file, void *addr, uint64_t length,
                            int prot, int flags, uint64_t offset);

static vfs_file_ops_t shmem_fops = {
    .read   = shmem_read,
    .write  = shmem_write,
    .close  = shmem_close,
    .lseek  = shmem_lseek,
    .mmap   = shmem_mmap_ops,
};

static void *shmem_ensure_page(shmem_object_t *obj, uint64_t idx) {
    if (idx >= SHMEM_MAX_PAGES) return NULL;
    if (obj->pages[idx]) return obj->pages[idx];
    void *p = pmm_alloc_page_and_clear();
    if (!p) return NULL;
    obj->pages[idx] = p;
    return p;
}

static void *shmem_page_kva(shmem_object_t *obj, uint64_t idx) {
    if (idx >= SHMEM_MAX_PAGES) return NULL;
    void *p = obj->pages[idx];
    if (!p) return NULL;
    return (void *)((uint64_t)p + get_bootinfo().hhdm->offset);
}

int shmem_init(void) {
    memset(g_shmem, 0, sizeof(g_shmem));
    return 0;
}

static shmem_object_t *shmem_alloc_object(void) {
    for (int i = 0; i < SHMEM_MAX; i++) {
        if (!g_shmem[i].used) {
            memset(&g_shmem[i], 0, sizeof(shmem_object_t));
            g_shmem[i].used = true;
            return &g_shmem[i];
        }
    }
    return NULL;
}

static int shmem_read(vfs_file_t *file, void *buf, uint64_t count) {
    shmem_object_t *obj = (shmem_object_t *)file->node->fs_data;
    if (!obj) return -EBADF;
    if (file->offset >= obj->size) return 0;
    if (file->offset + count > obj->size) count = obj->size - file->offset;
    if (count == 0) return 0;

    uint8_t *out = (uint8_t *)buf;
    uint64_t off = file->offset;
    uint64_t rem = count;

    while (rem > 0) {
        uint64_t idx  = off / SHMEM_PAGE_SIZE;
        uint64_t poff = off % SHMEM_PAGE_SIZE;
        uint64_t chunk = SHMEM_PAGE_SIZE - poff;
        if (chunk > rem) chunk = rem;

        void *src = shmem_page_kva(obj, idx);
        if (src) memcpy(out, (uint8_t *)src + poff, chunk);
        else     memset(out, 0, chunk);

        out += chunk;
        off += chunk;
        rem -= chunk;
    }

    file->offset += count;
    return (int)count;
}

static int shmem_write(vfs_file_t *file, const void *buf, uint64_t count) {
    shmem_object_t *obj = (shmem_object_t *)file->node->fs_data;
    if (!obj) return -EBADF;
    if (obj->sealed) return -EPERM;
    if (file->offset + count > obj->size) {
        uint64_t needed = file->offset + count;
        if (needed > (uint64_t)SHMEM_MAX_PAGES * SHMEM_PAGE_SIZE) return -ENOMEM;
        obj->size = needed;
        file->node->size = obj->size;
    }

    const uint8_t *in = (const uint8_t *)buf;
    uint64_t off = file->offset;
    uint64_t rem = count;

    while (rem > 0) {
        uint64_t idx  = off / SHMEM_PAGE_SIZE;
        uint64_t poff = off % SHMEM_PAGE_SIZE;
        uint64_t chunk = SHMEM_PAGE_SIZE - poff;
        if (chunk > rem) chunk = rem;

        void *phys = shmem_ensure_page(obj, idx);
        if (!phys) return -ENOMEM;
        void *dst = (void *)((uint64_t)phys + get_bootinfo().hhdm->offset);
        memcpy((uint8_t *)dst + poff, in, chunk);

        in += chunk;
        off += chunk;
        rem -= chunk;
    }

    file->offset += count;
    return (int)count;
}

static int shmem_close(vfs_file_t *file) {
    (void)file;
    return 0;
}

static uint64_t shmem_lseek(vfs_file_t *file, int64_t offset, int whence) {
    shmem_object_t *obj = (shmem_object_t *)file->node->fs_data;
    if (!obj) return (uint64_t)-EBADF;
    int64_t base = 0;
    if (whence == VFS_SEEK_SET) base = 0;
    else if (whence == VFS_SEEK_CUR) base = (int64_t)file->offset;
    else if (whence == VFS_SEEK_END) base = (int64_t)obj->size;
    int64_t nv = base + offset;
    if (nv < 0) return (uint64_t)-EINVAL;
    file->offset = (uint64_t)nv;
    return (uint64_t)nv;
}

static void *shmem_mmap_ops(vfs_file_t *file, void *addr, uint64_t length,
                            int prot, int flags, uint64_t offset) {
    (void)prot;
    shmem_object_t *obj = (shmem_object_t *)file->node->fs_data;
    if (!obj) return MAP_FAILED;

    process_t *proc = get_current_process();
    if (!proc || !proc->pml4) return MAP_FAILED;

    uint64_t pages = (length + 4095) / 4096;
    uint64_t virt;

    if (addr && (flags & MAP_FIXED)) {
        virt = (uint64_t)addr;
    } else if (addr) {
        virt = (uint64_t)addr;
    } else {
        if (!proc->mmap_base) proc->mmap_base = 0x5000000000ULL;
        virt = proc->mmap_base;
        proc->mmap_base += pages * 4096;
    }

    uint64_t file_off = offset;
    uint64_t vmm_flags = PTE_PRESENT | PTE_USER | PTE_WRITABLE;

    for (uint64_t i = 0; i < pages; i++) {
        uint64_t fidx = (file_off / SHMEM_PAGE_SIZE) + i;
        void *phys = shmem_ensure_page(obj, fidx);
        if (!phys) return MAP_FAILED;
        vmm_map_page(proc->pml4, (uint64_t)phys, virt + i * 4096, vmm_flags);
    }

    return (void *)virt;
}

int sys_memfd_create_impl(const char *name, uint32_t flags) {
    (void)flags;
    process_t *p = get_current_process();
    if (!p || !p->fd_table) return -EBADF;

    shmem_object_t *obj = shmem_alloc_object();
    if (!obj) return -ENOMEM;

    if (name) {
        int i = 0;
        while (name[i] && i < 63) { obj->name[i] = name[i]; i++; }
        obj->name[i] = 0;
    }

    vfs_node_t *n = malloc_alloc(sizeof(vfs_node_t));
    if (!n) return -ENOMEM;
    memset(n, 0, sizeof(*n));
    int i = 0;
    while (obj->name[i] && i < 255) { n->name[i] = obj->name[i]; i++; }
    n->name[i] = 0;
    n->type = VFS_TYPE_FILE;
    n->size = 0;
    n->file_ops = &shmem_fops;
    n->fs_data  = obj;

    vfs_file_t *f = malloc_alloc(sizeof(vfs_file_t));
    if (!f) { malloc_free(n); return -ENOMEM; }
    memset(f, 0, sizeof(*f));
    f->node = n;
    f->flags = VFS_O_RDWR;
    f->refcount = 1;

    for (int fd = 0; fd < VFS_MAX_FD; fd++) {
        if (!p->fd_table->fds[fd]) {
            p->fd_table->fds[fd] = f;
            return fd;
        }
    }

    malloc_free(f);
    malloc_free(n);
    return -EMFILE;
}

int sys_ftruncate_impl(int fd, int64_t length) {
    process_t *p = get_current_process();
    if (!p || !p->fd_table) return -EBADF;
    if (fd < 0 || fd >= VFS_MAX_FD || !p->fd_table->fds[fd]) return -EBADF;
    if (length < 0) return -EINVAL;

    vfs_file_t *file = p->fd_table->fds[fd];
    if (!file->node) return -EBADF;

    if (file->node->file_ops != &shmem_fops) {
        file->node->size = (uint64_t)length;
        return 0;
    }

    shmem_object_t *obj = (shmem_object_t *)file->node->fs_data;
    if (!obj) return -EBADF;

    uint64_t new_size = (uint64_t)length;
    if (new_size > (uint64_t)SHMEM_MAX_PAGES * SHMEM_PAGE_SIZE) return -EFBIG;

    obj->size = new_size;
    file->node->size = new_size;
    return 0;
}