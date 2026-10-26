#include <fs/vfs.h>
#include <mm/malloc.h>
#include <memory.h>
#include <gconsole.h>
#include <sys/mman.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <bootloader.h>
#include <scheduler/scheduler.h>
#include <kernel.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define MAX_FS_TYPES 16
#define MAX_SYMLINK_DEPTH 8

static vfs_fs_type_t *fs_types[MAX_FS_TYPES];
static int fs_type_count = 0;

static vfs_mount_t *drives[26];
static char boot_drive_letter = 'C';

void vfs_init(void) {
    memset(drives, 0, sizeof(drives));
    memset(fs_types, 0, sizeof(fs_types));
    fs_type_count = 0;
}

void vfs_set_boot_drive(char drive_letter) {
    if (drive_letter >= 'a' && drive_letter <= 'z') drive_letter -= 32;
    if (drive_letter >= 'A' && drive_letter <= 'Z') boot_drive_letter = drive_letter;
}

int vfs_register_fs(vfs_fs_type_t *fs) {
    if (fs_type_count >= MAX_FS_TYPES) return -1;
    fs_types[fs_type_count++] = fs;
    return 0;
}

int vfs_mount_fs(const char *fs_name, const char *device, char drive_letter, void *data) {
    if (drive_letter >= 'a' && drive_letter <= 'z') drive_letter -= 32;
    if (drive_letter < 'A' || drive_letter > 'Z') return -1;
    
    vfs_fs_type_t *fs = NULL;
    for (int i = 0; i < fs_type_count; i++) {
        const char *a = fs_types[i]->name;
        const char *b = fs_name;
        while (*a && *a == *b) { a++; b++; }
        if (*a == 0 && *b == 0) {
            fs = fs_types[i];
            break;
        }
    }

    if (!fs) return -2;

    vfs_mount_t *mnt = fs->mount(device, data);
    if (!mnt) {
        return -3;
    }

    mnt->drive_letter = drive_letter;
    drives[drive_letter - 'A'] = mnt;
    return 0;
}

char* normalize_path(const char *path) {
    if (!path) return NULL;
    
    char *norm = malloc_alloc(512);
    if (!norm) return NULL;
    memset(norm, 0, 512);

    if (path[0] == '/' && path[1] == 'd' && path[2] == 'e' && path[3] == 'v' && 
       (path[4] == '/' || path[4] == '\0')) {
        norm[0] = 'D';
        norm[1] = ':';
        if (path[4] == '\0') {
            norm[2] = '/';
            norm[3] = '\0';
            return norm;
        }
        int i = 4;
        int j = 2;
        while (path[i] && j < 510) norm[j++] = path[i++];
        norm[j] = '\0';
        return norm;
    }

    if (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':') {
        int i = 0;
        while (path[i] && i < 511) { norm[i] = path[i]; i++; }
        if (norm[0] >= 'a' && norm[0] <= 'z') norm[0] -= 32;
        norm[i] = '\0';
        return norm;
    }

    if (path[0] == '/') {
        norm[0] = boot_drive_letter;
        norm[1] = ':';
        int i = 0;
        int j = 2;
        while (path[i] && j < 511) norm[j++] = path[i++];
        norm[j] = '\0';
        return norm;
    }

    process_t *p = get_current_process();
    const char *cwd = (p && p->cwd[0]) ? p->cwd : "/";

    char target_drive = boot_drive_letter;

    if ((cwd[0] == '/' && cwd[1] == 'd' && cwd[2] == 'e' && cwd[3] == 'v' && (cwd[4] == '/' || cwd[4] == '\0')) ||
        ((cwd[0] == 'D' || cwd[0] == 'd') && cwd[1] == ':')) {
        target_drive = 'D';
    }

    norm[0] = target_drive;
    norm[1] = ':';
    int j = 2;

    int i = 0;
    if (cwd[0] == '/') {
        if (target_drive == 'D') {
            i = 4;
        }
        while (cwd[i] && j < 510) norm[j++] = cwd[i++];
    } else if (((cwd[0] >= 'A' && cwd[0] <= 'Z') || (cwd[0] >= 'a' && cwd[0] <= 'z')) && cwd[1] == ':') {
        i = 2;
        while (cwd[i] && j < 510) norm[j++] = cwd[i++];
    }

    if (j == 2 || norm[j - 1] != '/') {
        norm[j++] = '/';
    }

    i = 0;
    while (path[i] && j < 511) norm[j++] = path[i++];

    norm[j] = '\0';
    return norm;
}

vfs_node_t *vfs_resolve_path_ext(const char *path, bool parent_only, char *last_component) {
    if (!path) return NULL;
    char *norm = normalize_path(path);
    if (!norm) return NULL;
    //console_print_string(norm);

    if (norm[0] < 'A' || norm[0] > 'Z' || norm[1] != ':') {
        malloc_free(norm);
        return NULL;
    }

    vfs_mount_t *mnt = drives[norm[0] - 'A'];
    if (!mnt || !mnt->root) { malloc_free(norm); return NULL; }

    vfs_node_t *current = mnt->root;
    char *p = norm + 2;
    while (*p == '/') p++;

    int symlink_depth = 0;

    while (*p && current) {
        char token[256];
        int t = 0;
        while (*p && *p != '/' && t < 255) token[t++] = *p++;
        token[t] = '\0';
        while (*p == '/') p++;

        if (t == 0) break;
        if (t == 1 && token[0] == '.') continue;

        if (parent_only && *p == '\0') {
            if (last_component) {
                int k = 0; while (token[k]) { last_component[k] = token[k]; k++; }
                last_component[k] = 0;
            }
            malloc_free(norm);
            return current;
        }

        if (current->type != VFS_TYPE_DIR || !current->ops || !current->ops->lookup) {
            current = NULL; break;
        }

        vfs_node_t *next = current->ops->lookup(current, token);
        
        if (next && next->type == VFS_TYPE_SYMLINK) {
            if (++symlink_depth > MAX_SYMLINK_DEPTH) { malloc_free(norm); return NULL; }
            
            char target[512] = {0};
            int res = next->ops->readlink ? next->ops->readlink(next, target, 511) : -1;
            
            if (res >= 0) {
                if (target[0] != '/') {
                    next = current->ops->lookup(current, target);
                } else {
                    next = vfs_resolve_path(target);
                }
            } else {
                next = NULL;
            }
        }
        current = next;
    }

    malloc_free(norm);
    return current;
}

vfs_node_t *vfs_resolve_path(const char *path) {
    return vfs_resolve_path_ext(path, false, NULL);
}

vfs_node_t *vfs_resolve_path_ext_r(const char *path, bool parent_only, char *last_component, int depth) {
    (void)depth;
    return vfs_resolve_path_ext(path, parent_only, last_component);
}

int vfs_check_access(vfs_node_t *node, uint32_t uid, uint32_t gid, int req_perms) {
    if (uid == 0) return 0;
    int perms = 0;
    if (node->uid == uid) {
        perms = (node->mode >> 6) & 7;
    } else if (node->gid == gid) {
        perms = (node->mode >> 3) & 7;
    } else {
        perms = node->mode & 7;
    }
    if ((perms & req_perms) == req_perms) return 0;
    return -1;
}

vfs_file_t *vfs_open(const char *path, uint32_t flags) {
    vfs_node_t *node = vfs_resolve_path(path);

    if (!node && (flags & VFS_O_CREAT)) {
        char filename[256];
        vfs_node_t *parent = vfs_resolve_path_ext(path, true, filename);
        if (parent && parent->ops && parent->ops->create) {
            parent->ops->create(parent, filename, 0644, &node);
        }
    }

    if (!node) return NULL;

    vfs_file_t *file = malloc_alloc(sizeof(vfs_file_t));
    if (!file) return NULL;
    
    memset(file, 0, sizeof(vfs_file_t));
    file->node = node;
    file->flags = flags;
    file->offset = (flags & VFS_O_APPEND) ? node->size : 0;
    file->refcount = 1;

    if (file->node->file_ops && file->node->file_ops->open) {
        if (file->node->file_ops->open(file) < 0) {
            malloc_free(file);
            return NULL;
        }
    }

    if ((flags & VFS_O_TRUNC) && node->size > 0 && node->file_ops && node->file_ops->write) {
        node->size = 0;
    }

    return file;
}

int vfs_read(vfs_file_t *file, void *buf, uint64_t count) {
    if (!file || !file->node || !file->node->file_ops || !file->node->file_ops->read)
        return -1;
    return file->node->file_ops->read(file, buf, count);
}

int vfs_write(vfs_file_t *file, const void *buf, uint64_t count) {
    if (!file || !file->node || !file->node->file_ops || !file->node->file_ops->write)
        return -1;
    return file->node->file_ops->write(file, buf, count);
}

int vfs_readdir(vfs_file_t *file, vfs_dirent_t *dirent) {
    if (!file || !file->node || !file->node->file_ops || !file->node->file_ops->readdir) return -1;
    return file->node->file_ops->readdir(file, dirent);
}

int vfs_close(vfs_file_t *file) {
    if (!file) return -1;

    if (file->refcount > 1) {
        file->refcount--;
        return 0;
    }

    int res = 0;
    if (file->node && file->node->file_ops && file->node->file_ops->close) {
        res = file->node->file_ops->close(file);
    }
    
    malloc_free(file);
    return res;
}

int64_t vfs_lseek(vfs_file_t *file, int64_t offset, int whence) {
    if (!file || !file->node) return -1;

    if (file->node->file_ops && file->node->file_ops->lseek) {
        return (int64_t)file->node->file_ops->lseek(file, offset, whence);
    }

    int64_t new_offset = file->offset;
    if (whence == VFS_SEEK_SET) new_offset = offset;
    else if (whence == VFS_SEEK_CUR) new_offset += offset;
    else if (whence == VFS_SEEK_END) new_offset = (int64_t)file->node->size + offset;

    if (new_offset < 0) return -1;

    file->offset = (uint64_t)new_offset;
    return new_offset;
}

int vfs_stat(const char *path, vfs_stat_t *st) {
    vfs_node_t *node = vfs_resolve_path(path);
    if (!node || !st) return -1;

    if (node->ops && node->ops->stat) {
        return node->ops->stat(node, st);
    }

    st->inode_num = node->inode_num;
    st->size = node->size;
    st->mode = node->mode;
    st->type = node->type;
    st->uid = node->uid;
    st->gid = node->gid;
    return 0;
}

int vfs_chmod(const char *path, uint32_t mode) {
    vfs_node_t *node = vfs_resolve_path(path);
    if (!node) return -1;
    if (node->ops && node->ops->chmod) return node->ops->chmod(node, mode);
    node->mode = mode;
    return 0;
}

int vfs_chown(const char *path, uint32_t uid, uint32_t gid) {
    vfs_node_t *node = vfs_resolve_path(path);
    if (!node) return -1;
    if (node->ops && node->ops->chown) return node->ops->chown(node, uid, gid);
    node->uid = uid;
    node->gid = gid;
    return 0;
}

int vfs_copy_file(const char *src_path, const char *dst_path) {
    vfs_file_t *src = vfs_open(src_path, VFS_O_READ);
    if (!src) return -1;

    vfs_file_t *dst = vfs_open(dst_path, VFS_O_WRITE | VFS_O_CREAT | VFS_O_TRUNC);
    if (!dst) {
        vfs_close(src);
        return -2;
    }

    uint8_t *buffer = malloc_alloc(8192);
    if (!buffer) {
        vfs_close(src);
        vfs_close(dst);
        return -3;
    }

    int bytes = 0;
    while ((bytes = vfs_read(src, buffer, 8192)) > 0) {
        vfs_write(dst, buffer, bytes);
    }

    malloc_free(buffer);
    vfs_close(src);
    vfs_close(dst);
    return 0;
}

int vfs_mkdir(const char *path, uint32_t mode) {
    if (!path) return -EINVAL;

    vfs_node_t *existing = vfs_resolve_path(path);
    if (existing) return -EEXIST;

    char dirname[256] = {0};
    vfs_node_t *parent = vfs_resolve_path_ext(path, true, dirname);
    if (!parent) return -ENOENT;
    if (parent->type != VFS_TYPE_DIR) return -ENOTDIR;

    if (parent->ops && parent->ops->mkdir) {
        return parent->ops->mkdir(parent, dirname, mode);
    }

    return -EROFS;
}

int vfs_unlink(const char *path) {
    if (!path) return -EINVAL;

    vfs_node_t *node = vfs_resolve_path(path);
    if (!node) return -ENOENT;

    if (node->type == VFS_TYPE_DIR) return -EISDIR;

    char filename[256] = {0};
    vfs_node_t *parent = vfs_resolve_path_ext(path, true, filename);
    if (!parent) return -ENOENT;
    if (parent->type != VFS_TYPE_DIR) return -ENOTDIR;

    if (parent->ops && parent->ops->unlink) {
        return parent->ops->unlink(parent, filename);
    }

    return -EROFS;
}

int vfs_rmdir(const char *path) {
    if (!path) return -EINVAL;

    vfs_node_t *node = vfs_resolve_path(path);
    if (!node) return -ENOENT;
    if (node->type != VFS_TYPE_DIR) return -ENOTDIR;

    char filename[256] = {0};
    vfs_node_t *parent = vfs_resolve_path_ext(path, true, filename);
    if (!parent) return -ENOENT;

    if (parent->ops && parent->ops->rmdir) {
        return parent->ops->rmdir(parent, filename);
    }

    return -EROFS;
}

int sys_open(process_fd_table_t *table, const char *path, uint32_t flags) {
    if (!table) return -EINVAL;
    
    int req_perms = 0;
    if (flags & VFS_O_READ) req_perms |= VFS_PERM_READ;
    if (flags & VFS_O_WRITE) req_perms |= VFS_PERM_WRITE;

    vfs_node_t *node = vfs_resolve_path(path);
    if (node && vfs_check_access(node, table->uid, table->gid, req_perms) != 0) {
        return -EACCES; 
    }

    vfs_file_t *file = vfs_open(path, flags);
    if (!file) return -ENOENT;

    for (int fd = 0; fd < VFS_MAX_FD; fd++) {
        if (table->fds[fd] == NULL) {
            table->fds[fd] = file;
            return fd;
        }
    }

    vfs_close(file);
    return -EMFILE;
}

int sys_read(process_fd_table_t *table, int fd, void *buf, uint64_t count) {
    if (!table || fd < 0 || fd >= VFS_MAX_FD || !table->fds[fd]) return -1;
    return vfs_read(table->fds[fd], buf, count);
}

int sys_write(process_fd_table_t *table, int fd, const void *buf, uint64_t count) {
    if (!table || fd < 0 || fd >= VFS_MAX_FD || !table->fds[fd]) return -1;
    return vfs_write(table->fds[fd], buf, count);
}

int sys_close(process_fd_table_t *table, int fd) {
    if (!table || fd < 0 || fd >= VFS_MAX_FD || !table->fds[fd]) return -1;
    int res = vfs_close(table->fds[fd]);
    table->fds[fd] = NULL;
    return res;
}

int vfs_link(const char *oldpath, const char *newpath) {
    if (!oldpath || !newpath) return -EINVAL;

    vfs_node_t *old_node = vfs_resolve_path(oldpath);
    if (!old_node) return -ENOENT;

    if (old_node->type == VFS_TYPE_DIR) return -EPERM;

    vfs_node_t *existing = vfs_resolve_path(newpath);
    if (existing) return -EEXIST;

    char filename[256] = {0};
    vfs_node_t *parent = vfs_resolve_path_ext(newpath, true, filename);
    if (!parent) return -ENOENT;
    if (parent->type != VFS_TYPE_DIR) return -ENOTDIR;

    if (parent->ops && parent->ops->link) {
        return parent->ops->link(parent, old_node, filename);
    }

    return 0;
}

int vfs_fchmod(vfs_file_t *file, uint32_t mode) {
    if (!file || !file->node) return -EBADF;

    if (file->node->ops && file->node->ops->chmod) {
        return file->node->ops->chmod(file->node, mode);
    }

    file->node->mode = mode;
    return 0;
}

int sys_ioctl(process_fd_table_t *table, int fd, uint64_t request, void *arg) {
    if (!table || fd < 0 || fd >= VFS_MAX_FD || !table->fds[fd]) return -1;
    vfs_file_t *file = table->fds[fd];
    if (file->node && file->node->file_ops && file->node->file_ops->ioctl) {
        return file->node->file_ops->ioctl(file, request, arg);
    }
    return -1;
}

void *sys_mmap(process_fd_table_t *table, void *addr, uint64_t length,
               int prot, int flags, int fd, uint64_t offset) {
    process_t *proc = get_current_process();
    if (!proc || !proc->pml4 || length == 0) return MAP_FAILED;

    if (!(flags & MAP_ANONYMOUS) && fd >= 0 && fd < VFS_MAX_FD
        && table && table->fds[fd]) {
        vfs_file_t *file = table->fds[fd];
        if (file->node && file->node->file_ops && file->node->file_ops->mmap) {
            return file->node->file_ops->mmap(file, addr, length, prot, flags, offset);
        }
    }

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

    uint64_t vmm_flags = PTE_PRESENT | PTE_USER | PTE_WRITABLE;

    void *phys = pmm_alloc_pages(pages);
    if (!phys) return MAP_FAILED;
    
    uint64_t phys_addr = (uint64_t)phys;
    uint64_t hhdm = get_bootinfo().hhdm->offset;
    memset((void *)(phys_addr + hhdm), 0, pages * 4096);

    for (uint64_t i = 0; i < pages; i++) {
        vmm_map_page(proc->pml4, phys_addr + i * 4096, virt + i * 4096, vmm_flags);
    }

    if (!(flags & MAP_ANONYMOUS) && fd >= 0 && fd < VFS_MAX_FD && table && table->fds[fd]) {
        vfs_file_t *file = table->fds[fd];
        vfs_lseek(file, offset, VFS_SEEK_SET);
        vfs_read(file, (void *)(phys_addr + hhdm), length);
    }

    return (void *)virt;
}