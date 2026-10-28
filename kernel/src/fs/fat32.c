#include <fs/fat32.h>
#include <fs/vfs.h>
#include <disk/block.h>
#include <disk/bcache.h>
#include <mm/malloc.h>
#include <mm/pmm.h>
#include <memory.h>
#include <cpu/rtc.h>

#pragma pack(push, 1)
typedef struct {
    uint8_t jmp[3];
    uint8_t oem[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t num_fats;
    uint16_t root_entries;
    uint16_t total_sectors_16;
    uint8_t media_type;
    uint16_t fat_size_16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t backup_boot;
    uint8_t reserved[12];
    uint8_t drive_number;
    uint8_t reserved1;
    uint8_t boot_signature;
    uint32_t volume_id;
    uint8_t volume_label[11];
    uint8_t fs_type[8];
} fat32_bpb_t;

typedef struct {
    char name[11];
    uint8_t attr;
    uint8_t nt_res;
    uint8_t ctime_th;
    uint16_t ctime;
    uint16_t cdate;
    uint16_t adate;
    uint16_t cluster_hi;
    uint16_t mtime;
    uint16_t mdate;
    uint16_t cluster_lo;
    uint32_t size;
} fat32_dir_entry_t;

typedef struct {
    uint8_t order;
    uint16_t name1[5];
    uint8_t attr;
    uint8_t type;
    uint8_t checksum;
    uint16_t name2[6];
    uint16_t zero;
    uint16_t name3[2];
} fat32_lfn_entry_t;
#pragma pack(pop)

typedef struct {
    char dev[32];
    uint64_t start_lba;
    uint32_t bps;
    uint8_t spc;
    uint32_t cluster_size;
    uint64_t fat_lba;
    uint64_t data_lba;
    uint32_t root_cluster;
    uint16_t fs_info_sector;
    uint8_t fat_cache[4096];
    uint64_t fat_cache_sec;
    bool fat_cache_valid;
} fat32_fs_t;

typedef struct {
    fat32_fs_t *fs;
    uint32_t first_cluster;
    uint32_t dir_cluster;
    uint32_t dir_index;
} fat32_node_t;

static void fat32_current_time(uint16_t *date, uint16_t *time, uint8_t *ms) {
    rtc_time_t t;
    rtc_get_time(&t);
    uint16_t year = t.year;
    if (year < 1980) year = 1980;
    if (year > 2107) year = 2107;
    if (date) *date = ((year - 1980) << 9) | (t.month << 5) | t.day;
    if (time) *time = (t.hour << 11) | (t.min << 5) | (t.sec / 2);
    if (ms) *ms = (t.sec % 2) * 100;
}

static void sync_fsinfo(fat32_fs_t *fs, int32_t alloc_delta, uint32_t last_alloc) {
    if (!fs->fs_info_sector) return;
    uint8_t buf[512];
    if (bcache_read(fs->dev, fs->start_lba + fs->fs_info_sector, 1, buf) != 0) return;
    uint32_t *sig1 = (uint32_t*)buf;
    uint32_t *sig2 = (uint32_t*)(buf + 484);
    if (*sig1 == 0x41615252 && *sig2 == 0x61417272) {
        uint32_t *free_cnt = (uint32_t*)(buf + 488);
        uint32_t *next_free = (uint32_t*)(buf + 492);
        if (*free_cnt != 0xFFFFFFFF) *free_cnt += alloc_delta;
        if (last_alloc != 0) *next_free = last_alloc;
        bcache_write(fs->dev, fs->start_lba + fs->fs_info_sector, 1, buf);
    }
}

static uint32_t fat32_read_fat(fat32_fs_t *fs, uint32_t cluster) {
    uint64_t offset = cluster * 4;
    uint64_t sec = fs->fat_lba + (offset / fs->bps);
    uint32_t idx = offset % fs->bps;
    if (!fs->fat_cache_valid || fs->fat_cache_sec != sec) {
        bcache_read(fs->dev, sec, (4096 + fs->bps - 1) / fs->bps, fs->fat_cache);
        fs->fat_cache_sec = sec;
        fs->fat_cache_valid = true;
    }
    uint32_t val = *(uint32_t*)(fs->fat_cache + idx);
    return val & 0x0FFFFFFF;
}

static void fat32_write_fat(fat32_fs_t *fs, uint32_t cluster, uint32_t val) {
    uint64_t offset = cluster * 4;
    uint64_t sec = fs->fat_lba + (offset / fs->bps);
    uint32_t idx = offset % fs->bps;
    if (!fs->fat_cache_valid || fs->fat_cache_sec != sec) {
        bcache_read(fs->dev, sec, (4096 + fs->bps - 1) / fs->bps, fs->fat_cache);
        fs->fat_cache_sec = sec;
        fs->fat_cache_valid = true;
    }
    uint32_t *p = (uint32_t*)(fs->fat_cache + idx);
    *p = (*p & 0xF0000000) | (val & 0x0FFFFFFF);
    bcache_write(fs->dev, sec, (4096 + fs->bps - 1) / fs->bps, fs->fat_cache);
}

static void fat32_free_chain(fat32_fs_t *fs, uint32_t cluster) {
    int freed = 0;
    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        uint32_t next = fat32_read_fat(fs, cluster);
        fat32_write_fat(fs, cluster, 0);
        cluster = next;
        freed++;
    }
    sync_fsinfo(fs, freed, 0);
}

static uint32_t fat32_alloc_cluster(fat32_fs_t *fs) {
    for (uint32_t i = 2; i < 0x0FFFFFF0; i++) {
        if (fat32_read_fat(fs, i) == 0) {
            fat32_write_fat(fs, i, 0x0FFFFFFF);
            uint8_t *zero = malloc_alloc(fs->cluster_size);
            memset(zero, 0, fs->cluster_size);
            bcache_write(fs->dev, fs->data_lba + (i - 2) * fs->spc, fs->spc, zero);
            malloc_free(zero);
            sync_fsinfo(fs, -1, i);
            return i;
        }
    }
    return 0;
}

static uint8_t lfn_checksum(const uint8_t *name) {
    uint8_t sum = 0;
    for (int i = 11; i; i--) sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + *name++;
    return sum;
}

static void read_chain(fat32_fs_t *fs, uint32_t cluster, uint8_t *buf, uint64_t size) {
    uint64_t off = 0;
    while (cluster >= 2 && cluster < 0x0FFFFFF8 && off < size) {
        uint64_t lba = fs->data_lba + (cluster - 2) * fs->spc;
        uint32_t chunk = fs->cluster_size;
        if (off + chunk > size) chunk = size - off;
        bcache_read(fs->dev, lba, (chunk + fs->bps - 1) / fs->bps, buf + off);
        off += chunk;
        cluster = fat32_read_fat(fs, cluster);
    }
}

static void write_chain(fat32_fs_t *fs, uint32_t cluster, uint8_t *buf, uint64_t size) {
    uint64_t off = 0;
    while (cluster >= 2 && cluster < 0x0FFFFFF8 && off < size) {
        uint64_t lba = fs->data_lba + (cluster - 2) * fs->spc;
        uint32_t chunk = fs->cluster_size;
        if (off + chunk > size) chunk = size - off;
        bcache_write(fs->dev, lba, (chunk + fs->bps - 1) / fs->bps, buf + off);
        off += chunk;
        cluster = fat32_read_fat(fs, cluster);
    }
}

static uint8_t *read_dir(fat32_fs_t *fs, uint32_t cluster, uint32_t *entries) {
    uint32_t c = cluster, cnt = 0;
    while (c >= 2 && c < 0x0FFFFFF8) { cnt++; c = fat32_read_fat(fs, c); }
    uint8_t *buf = malloc_alloc(cnt * fs->cluster_size);
    read_chain(fs, cluster, buf, cnt * fs->cluster_size);
    *entries = (cnt * fs->cluster_size) / 32;
    return buf;
}

static void update_dirent(fat32_node_t *dn, uint32_t size, uint32_t cluster, bool update_mod) {
    if (dn->dir_cluster < 2) return;
    uint32_t cnt;
    fat32_dir_entry_t *ents = (fat32_dir_entry_t *)read_dir(dn->fs, dn->dir_cluster, &cnt);
    if (!ents) return;
    if (dn->dir_index < cnt) {
        ents[dn->dir_index].size = size;
        ents[dn->dir_index].cluster_hi = cluster >> 16;
        ents[dn->dir_index].cluster_lo = cluster & 0xFFFF;
        if (update_mod) {
            uint16_t date, time;
            fat32_current_time(&date, &time, NULL);
            ents[dn->dir_index].mdate = date;
            ents[dn->dir_index].mtime = time;
            ents[dn->dir_index].adate = date;
        }
        write_chain(dn->fs, dn->dir_cluster, (uint8_t *)ents, cnt * 32);
    }
    malloc_free(ents);
}

static void int_to_str_fat(uint32_t num, char *out, int *len) {
    char buf[12];
    int i = 0;
    if (num == 0) { buf[i++] = '0'; }
    while (num > 0) { buf[i++] = (num % 10) + '0'; num /= 10; }
    *len = i + 1;
    out[0] = '~';
    for (int j = 1; j <= i; j++) out[j] = buf[i - j];
}

static void gen_sfn(const char *name, char sfn[11], uint32_t num) {
    memset(sfn, ' ', 11);
    char num_str[12];
    int num_len = 0;
    int_to_str_fat(num, num_str, &num_len);

    int b_len = 0;
    while (name[b_len] && name[b_len] != '.') b_len++;
    
    int c_len = b_len;
    if (c_len > 8 - num_len) c_len = 8 - num_len;

    int j = 0;
    for (int i = 0; i < b_len && j < c_len; i++) {
        char c = name[i];
        if (c == ' ' || c == '.') continue;
        if (c >= 'a' && c <= 'z') c -= 32;
        sfn[j++] = c;
    }
    for (int i = 0; i < num_len; i++) sfn[j++] = num_str[i];

    int d = b_len;
    if (name[d] == '.') {
        d++;
        int k = 8;
        while (name[d] && k < 11) {
            char c = name[d++];
            if (c == ' ') continue;
            if (c >= 'a' && c <= 'z') c -= 32;
            sfn[k++] = c;
        }
    }
}

static bool sfn_exists(fat32_fs_t *fs, uint32_t dir_cluster, const char sfn[11]) {
    uint32_t cnt;
    fat32_dir_entry_t *ents = (fat32_dir_entry_t*)read_dir(fs, dir_cluster, &cnt);
    if (!ents) return false;
    for (uint32_t i = 0; i < cnt; i++) {
        if (ents[i].name[0] == 0) break;
        if (ents[i].name[0] == (char)0xE5 || ents[i].attr == 0x0F) continue;
        bool match = true;
        for (int j = 0; j < 11; j++) {
            if (ents[i].name[j] != sfn[j]) { match = false; break; }
        }
        if (match) {
            malloc_free(ents);
            return true;
        }
    }
    malloc_free(ents);
    return false;
}

static vfs_node_t *fat32_lookup(vfs_node_t *dir, const char *name);
static int fat32_read(vfs_file_t *file, void *buf, uint64_t count);
static int fat32_write(vfs_file_t *file, const void *buf, uint64_t count);
static int fat32_readdir(vfs_file_t *file, vfs_dirent_t *dirent);
static int fat32_create(vfs_node_t *dir, const char *name, uint16_t mode, vfs_node_t **out);
static int fat32_mkdir(vfs_node_t *dir, const char *name, uint16_t mode);
static int fat32_unlink(vfs_node_t *dir, const char *name);
static int fat32_rmdir(vfs_node_t *dir, const char *name);

static vfs_file_ops_t fops = { .read = fat32_read, .write = fat32_write, .readdir = fat32_readdir };
static vfs_node_ops_t nops = { .lookup = fat32_lookup, .create = fat32_create, .mkdir = fat32_mkdir, .unlink = fat32_unlink, .rmdir = fat32_rmdir };

static vfs_node_t *make_node(fat32_fs_t *fs, uint32_t cluster, uint64_t size, uint8_t attr, const char *name, uint32_t d_cluster, uint32_t d_idx) {
    vfs_node_t *n = malloc_alloc(sizeof(vfs_node_t));
    memset(n, 0, sizeof(vfs_node_t));
    fat32_node_t *fn = malloc_alloc(sizeof(fat32_node_t));
    fn->fs = fs; fn->first_cluster = cluster;
    fn->dir_cluster = d_cluster; fn->dir_index = d_idx;
    n->fs_data = fn; n->inode_num = cluster; n->size = size;
    n->type = (attr & 0x10) ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    n->ops = &nops; n->file_ops = &fops;
    int i = 0; while (name[i] && i < 255) { n->name[i] = name[i]; i++; }
    return n;
}

static vfs_node_t *fat32_lookup(vfs_node_t *dir, const char *name) {
    fat32_node_t *dn = dir->fs_data;
    uint32_t cnt;
    fat32_dir_entry_t *ents = (fat32_dir_entry_t*)read_dir(dn->fs, dn->first_cluster, &cnt);
    if (!ents) return NULL;
    vfs_node_t *res = NULL;
    char lfn[256]; lfn[0] = 0;
    for (uint32_t i = 0; i < cnt; i++) {
        if (ents[i].name[0] == 0) break;
        if (ents[i].name[0] == (char)0xE5) continue;
        if (ents[i].attr == 0x0F) {
            fat32_lfn_entry_t *l = (fat32_lfn_entry_t*)&ents[i];
            int idx = ((l->order & 0x3F) - 1) * 13;
            for(int j=0;j<5;j++) lfn[idx++] = l->name1[j];
            for(int j=0;j<6;j++) lfn[idx++] = l->name2[j];
            for(int j=0;j<2;j++) lfn[idx++] = l->name3[j];
            if (l->order & 0x40) lfn[idx] = 0;
            continue;
        }
        char sfn[13]; int k=0, j=0;
        while(j<8 && ents[i].name[j] != ' ') sfn[k++] = ents[i].name[j++];
        if(ents[i].name[8] != ' ') {
            sfn[k++] = '.'; j=8;
            while(j<11 && ents[i].name[j] != ' ') sfn[k++] = ents[i].name[j++];
        }
        sfn[k] = 0;
        bool match = true;
        char *cmp = lfn[0] ? lfn : sfn;
        for (k = 0; name[k] || cmp[k]; k++) {
            char a = name[k], b = cmp[k];
            if (a>='A'&&a<='Z') a+=32; if (b>='A'&&b<='Z') b+=32;
            if (a!=b) { match=false; break; }
        }
        if (match) {
            uint32_t c = (ents[i].cluster_hi << 16) | ents[i].cluster_lo;
            res = make_node(dn->fs, c, ents[i].size, ents[i].attr, name, dn->first_cluster, i);
            break;
        }
        lfn[0] = 0;
    }
    malloc_free(ents);
    return res;
}

static int fat32_read(vfs_file_t *f, void *buf, uint64_t count) {
    fat32_node_t *fn = f->node->fs_data;
    fat32_fs_t *fs = fn->fs;

    if (f->offset >= f->node->size) return 0;
    if (f->offset + count > f->node->size)
        count = f->node->size - f->offset;
    if (count == 0) return 0;

    uint8_t *out = (uint8_t *)buf;
    uint64_t remaining = count;
    uint64_t file_off = f->offset;

    uint32_t cluster = fn->first_cluster;
    uint64_t skip = file_off / fs->cluster_size;
    for (uint64_t i = 0; i < skip; i++) {
        if (cluster < 2 || cluster >= 0x0FFFFFF8) return -1;
        cluster = fat32_read_fat(fs, cluster);
    }

    uint32_t offset_in_cluster = file_off % fs->cluster_size;

    while (remaining > 0 && cluster >= 2 && cluster < 0x0FFFFFF8) {
        uint64_t lba = fs->data_lba + (uint64_t)(cluster - 2) * fs->spc;
        uint32_t chunk = fs->cluster_size - offset_in_cluster;
        if (chunk > remaining) chunk = (uint32_t)remaining;

        uint8_t *tmp = malloc_alloc(fs->cluster_size);
        if (!tmp) return -1;
        if (bcache_read(fs->dev, lba, fs->spc, tmp) != 0) {
            malloc_free(tmp);
            return -1;
        }
        memcpy(out, tmp + offset_in_cluster, chunk);
        malloc_free(tmp);

        out += chunk;
        remaining -= chunk;
        offset_in_cluster = 0;

        if (remaining > 0) {
            cluster = fat32_read_fat(fs, cluster);
        }
    }

    uint64_t got = count - remaining;
    f->offset += got;
    return (int)got;
}

static int fat32_write(vfs_file_t *file, const void *buf, uint64_t count) {
    fat32_node_t *fn = file->node->fs_data;
    if (file->flags & VFS_O_TRUNC && file->offset == 0 && file->node->size == 0 && fn->first_cluster >= 2) {
        fat32_free_chain(fn->fs, fn->first_cluster);
        fn->first_cluster = 0;
        update_dirent(fn, 0, 0, true);
    }
    uint32_t req = file->offset + count;
    uint32_t cur_cl = (file->node->size + fn->fs->cluster_size - 1) / fn->fs->cluster_size;
    uint32_t req_cl = (req + fn->fs->cluster_size - 1) / fn->fs->cluster_size;
    if (fn->first_cluster < 2 && req > 0) {
        fn->first_cluster = fat32_alloc_cluster(fn->fs);
        update_dirent(fn, file->node->size, fn->first_cluster, true);
        cur_cl = 1;
    }
    if (cur_cl == 0 && req_cl > 0) cur_cl = 1;
    uint32_t c = fn->first_cluster, prev = c;
    while(c >= 2 && c < 0x0FFFFFF8) { prev = c; c = fat32_read_fat(fn->fs, c); }
    while (cur_cl < req_cl) {
        uint32_t nc = fat32_alloc_cluster(fn->fs);
        fat32_write_fat(fn->fs, prev, nc);
        prev = nc; cur_cl++;
    }
    uint32_t bsz = req_cl * fn->fs->cluster_size;
    uint8_t *t = malloc_alloc(bsz);
    memset(t, 0, bsz);
    if (file->node->size > 0) read_chain(fn->fs, fn->first_cluster, t, file->node->size);
    memcpy(t + file->offset, buf, count);
    write_chain(fn->fs, fn->first_cluster, t, bsz);
    malloc_free(t);
    file->offset += count;
    if (file->offset > file->node->size) {
        file->node->size = file->offset;
    }
    update_dirent(fn, file->node->size, fn->first_cluster, true);
    return count;
}

static int fat32_readdir(vfs_file_t *f, vfs_dirent_t *d) {
    fat32_node_t *fn = f->node->fs_data;
    uint32_t cnt;
    fat32_dir_entry_t *ents = (fat32_dir_entry_t*)read_dir(fn->fs, fn->first_cluster, &cnt);
    char lfn[256]; lfn[0] = 0;
    for (uint32_t i = f->offset; i < cnt; i++) {
        if (ents[i].name[0] == 0) break;
        if (ents[i].name[0] == (char)0xE5) { f->offset++; continue; }
        if (ents[i].attr == 0x0F) {
            fat32_lfn_entry_t *l = (fat32_lfn_entry_t*)&ents[i];
            int idx = ((l->order & 0x3F) - 1) * 13;
            for(int j=0;j<5;j++) lfn[idx++] = l->name1[j];
            for(int j=0;j<6;j++) lfn[idx++] = l->name2[j];
            for(int j=0;j<2;j++) lfn[idx++] = l->name3[j];
            if (l->order & 0x40) lfn[idx] = 0;
            f->offset++; continue;
        }
        char sfn[13]; int k=0, j=0;
        while(j<8 && ents[i].name[j] != ' ') sfn[k++] = ents[i].name[j++];
        if(ents[i].name[8] != ' ') {
            sfn[k++] = '.'; j=8;
            while(j<11 && ents[i].name[j] != ' ') sfn[k++] = ents[i].name[j++];
        }
        sfn[k] = 0;
        char *cmp = lfn[0] ? lfn : sfn;
        for (k=0; cmp[k]; k++) d->name[k] = cmp[k];
        d->name[k] = 0;
        d->inode_num = (ents[i].cluster_hi << 16) | ents[i].cluster_lo;
        d->size = ents[i].size;
        d->type = (ents[i].attr & 0x10) ? VFS_TYPE_DIR : VFS_TYPE_FILE;
        f->offset = i + 1;
        malloc_free(ents);
        return 0;
    }
    malloc_free(ents);
    return -1;
}

static int append_dir_entries(fat32_fs_t *fs, uint32_t pclust, fat32_dir_entry_t *new_ents, uint32_t count, uint32_t *out_idx) {
    uint32_t c = pclust, prev = 0, cnt = 0;
    while (c >= 2 && c < 0x0FFFFFF8) { prev = c; cnt++; c = fat32_read_fat(fs, c); }
    uint32_t bsz = cnt * fs->cluster_size;
    uint8_t *buf = malloc_alloc(bsz + fs->cluster_size);
    read_chain(fs, pclust, buf, bsz);
    fat32_dir_entry_t *ents = (fat32_dir_entry_t*)buf;
    uint32_t max = bsz / 32;
    uint32_t f_start = 0, f_cnt = 0;
    bool found = false;
    for (uint32_t i = 0; i < max; i++) {
        if (ents[i].name[0] == 0 || ents[i].name[0] == (char)0xE5) {
            if (f_cnt == 0) f_start = i;
            f_cnt++;
            if (f_cnt == count) { found = true; break; }
        } else f_cnt = 0;
    }
    if (found) {
        memcpy(&ents[f_start], new_ents, count * 32);
        write_chain(fs, pclust, buf, bsz);
        if (out_idx) *out_idx = f_start + count - 1;
        malloc_free(buf);
        return 0;
    }
    uint32_t nc = fat32_alloc_cluster(fs);
    if (!nc) { malloc_free(buf); return -1; }
    fat32_write_fat(fs, prev, nc);
    memset(buf + bsz, 0, fs->cluster_size);
    memcpy(buf + bsz, new_ents, count * 32);
    block_write(fs->dev, fs->data_lba + (nc - 2) * fs->spc, fs->spc, buf + bsz);
    if (out_idx) *out_idx = max + count - 1;
    malloc_free(buf);
    return 0;
}

static int create_entry(vfs_node_t *dir, const char *name, uint8_t attr, uint32_t cluster, vfs_node_t **out) {
    fat32_node_t *dn = dir->fs_data;
    int len = 0; while (name[len]) len++;
    int lfn_count = (len + 12) / 13;
    int total_ents = lfn_count + 1;
    fat32_dir_entry_t *ents = malloc_alloc(total_ents * 32);
    memset(ents, 0, total_ents * 32);

    uint32_t sfn_num = 1;
    char sfn[11];
    while (sfn_num < 999999) {
        gen_sfn(name, sfn, sfn_num);
        if (!sfn_exists(dn->fs, dn->first_cluster, sfn)) break;
        sfn_num++;
    }

    uint8_t chk = lfn_checksum((uint8_t*)sfn);
    for (int i = 0; i < lfn_count; i++) {
        fat32_lfn_entry_t *lfn = (fat32_lfn_entry_t *)&ents[lfn_count - 1 - i];
        lfn->order = (i + 1) | ((i == lfn_count - 1) ? 0x40 : 0);
        lfn->attr = 0x0F; lfn->checksum = chk;
        int b = i * 13;
        for(int c=0; c<5; c++) lfn->name1[c] = (b+c < len) ? name[b+c] : ((b+c == len) ? 0 : 0xFFFF);
        for(int c=0; c<6; c++) lfn->name2[c] = (b+5+c < len) ? name[b+5+c] : ((b+5+c == len) ? 0 : 0xFFFF);
        for(int c=0; c<2; c++) lfn->name3[c] = (b+11+c < len) ? name[b+11+c] : ((b+11+c == len) ? 0 : 0xFFFF);
    }
    fat32_dir_entry_t *sfn_ent = &ents[lfn_count];
    memcpy(sfn_ent->name, sfn, 11);
    sfn_ent->attr = attr;
    sfn_ent->cluster_hi = cluster >> 16;
    sfn_ent->cluster_lo = cluster & 0xFFFF;
    sfn_ent->size = 0;

    uint16_t date, time;
    uint8_t ms;
    fat32_current_time(&date, &time, &ms);
    sfn_ent->ctime = time;
    sfn_ent->cdate = date;
    sfn_ent->ctime_th = ms;
    sfn_ent->mtime = time;
    sfn_ent->mdate = date;
    sfn_ent->adate = date;

    uint32_t d_idx;
    int r = append_dir_entries(dn->fs, dn->first_cluster, ents, total_ents, &d_idx);
    malloc_free(ents);
    if (r == 0 && out) *out = make_node(dn->fs, cluster, 0, attr, name, dn->first_cluster, d_idx);
    return r;
}

static int fat32_create(vfs_node_t *dir, const char *name, uint16_t mode, vfs_node_t **out) {
    fat32_node_t *dn = dir->fs_data;
    uint32_t nc = fat32_alloc_cluster(dn->fs);
    if (!nc) return -1;
    return create_entry(dir, name, 0, nc, out);
}

static int fat32_mkdir(vfs_node_t *dir, const char *name, uint16_t mode) {
    fat32_node_t *dn = dir->fs_data;
    uint32_t nc = fat32_alloc_cluster(dn->fs);
    if (!nc) return -1;
    vfs_node_t *n;
    if (create_entry(dir, name, 0x10, nc, &n) != 0) return -1;

    uint16_t date, time;
    fat32_current_time(&date, &time, NULL);

    fat32_dir_entry_t ents[2];
    memset(ents, 0, 64);
    memset(ents[0].name, ' ', 11); ents[0].name[0]='.'; ents[0].attr=0x10; 
    ents[0].cluster_hi=nc>>16; ents[0].cluster_lo=nc&0xFFFF;
    ents[0].mtime = time; ents[0].mdate = date; ents[0].cdate = date; ents[0].ctime = time; ents[0].adate = date;

    memset(ents[1].name, ' ', 11); ents[1].name[0]='.'; ents[1].name[1]='.'; ents[1].attr=0x10; 
    ents[1].cluster_hi=dn->first_cluster>>16; ents[1].cluster_lo=dn->first_cluster&0xFFFF;
    ents[1].mtime = time; ents[1].mdate = date; ents[1].cdate = date; ents[1].ctime = time; ents[1].adate = date;

    append_dir_entries(dn->fs, nc, ents, 2, NULL);
    return 0;
}

static int fat32_remove(vfs_node_t *dir, const char *name, bool is_dir) {
    fat32_node_t *dn = dir->fs_data;
    uint32_t cnt;
    fat32_dir_entry_t *ents = (fat32_dir_entry_t*)read_dir(dn->fs, dn->first_cluster, &cnt);
    if (!ents) return -1;
    char lfn[256]; lfn[0] = 0; int l_start = -1;
    for (uint32_t i = 0; i < cnt; i++) {
        if (ents[i].name[0] == 0) break;
        if (ents[i].name[0] == (char)0xE5) { l_start = -1; continue; }
        if (ents[i].attr == 0x0F) {
            if (l_start == -1) l_start = i;
            fat32_lfn_entry_t *l = (fat32_lfn_entry_t*)&ents[i];
            int idx = ((l->order & 0x3F) - 1) * 13;
            for(int j=0;j<5;j++) lfn[idx++] = l->name1[j];
            for(int j=0;j<6;j++) lfn[idx++] = l->name2[j];
            for(int j=0;j<2;j++) lfn[idx++] = l->name3[j];
            if (l->order & 0x40) lfn[idx] = 0;
            continue;
        }
        if (l_start == -1) l_start = i;
        char sfn[13]; int k=0, j=0;
        while(j<8 && ents[i].name[j] != ' ') sfn[k++] = ents[i].name[j++];
        if(ents[i].name[8] != ' ') {
            sfn[k++] = '.'; j=8;
            while(j<11 && ents[i].name[j] != ' ') sfn[k++] = ents[i].name[j++];
        }
        sfn[k] = 0;
        bool match = true;
        char *cmp = lfn[0] ? lfn : sfn;
        for (k = 0; name[k] || cmp[k]; k++) {
            char a = name[k], b = cmp[k];
            if (a>='A'&&a<='Z') a+=32; if (b>='A'&&b<='Z') b+=32;
            if (a!=b) { match=false; break; }
        }
        if (match) {
            bool is_d = (ents[i].attr & 0x10) != 0;
            if (is_d != is_dir) { malloc_free(ents); return -1; }
            uint32_t tc = (ents[i].cluster_hi << 16) | ents[i].cluster_lo;
            if (is_dir && tc >= 2) {
                uint32_t dcnt;
                fat32_dir_entry_t *dents = (fat32_dir_entry_t*)read_dir(dn->fs, tc, &dcnt);
                bool empty = true;
                for(uint32_t q=0; q<dcnt; q++) {
                    if (dents[q].name[0] == 0) break;
                    if (dents[q].name[0] == (char)0xE5 || dents[q].attr == 0x0F || dents[q].name[0] == '.') continue;
                    empty = false; break;
                }
                malloc_free(dents);
                if (!empty) { malloc_free(ents); return -2; }
            }
            for (int q = l_start; q <= i; q++) ents[q].name[0] = (char)0xE5;
            write_chain(dn->fs, dn->first_cluster, (uint8_t*)ents, cnt * 32);
            if (tc >= 2) fat32_free_chain(dn->fs, tc);
            malloc_free(ents);
            return 0;
        }
        lfn[0] = 0; l_start = -1;
    }
    malloc_free(ents);
    return -1;
}

static int fat32_unlink(vfs_node_t *dir, const char *name) { return fat32_remove(dir, name, false); }
static int fat32_rmdir(vfs_node_t *dir, const char *name) { return fat32_remove(dir, name, true); }

static vfs_mount_t* fat32_mount(const char *dev, void *data) {
    uint8_t buf[512];
    if (bcache_read(dev, (uint64_t)data, 1, buf)) return NULL;
    fat32_bpb_t *b = (fat32_bpb_t*)buf;
    fat32_fs_t *f = malloc_alloc(sizeof(fat32_fs_t));
    memset(f, 0, sizeof(fat32_fs_t));
    int i = 0; while (dev[i] && i < 31) { f->dev[i] = dev[i]; i++; }
    f->start_lba = (uint64_t)data; f->bps = b->bytes_per_sector; f->spc = b->sectors_per_cluster;
    f->cluster_size = f->bps * f->spc; f->root_cluster = b->root_cluster;
    f->fs_info_sector = b->fs_info;
    f->fat_lba = f->start_lba + b->reserved_sectors;
    f->data_lba = f->fat_lba + b->num_fats * b->fat_size_32;
    vfs_mount_t *m = malloc_alloc(sizeof(vfs_mount_t));
    m->fs_data = f;
    m->root = make_node(f, f->root_cluster, 0, 0x10, "/", 0, 0);
    m->root->mount = m;
    return m;
}

static int fat32_unmount(vfs_mount_t *mnt) {
    (void)mnt;
    return 0;
}

static vfs_fs_type_t fat32_type = { .name = "fat32", .mount = fat32_mount, .unmount = fat32_unmount };
void fat32_vfs_init(void) { vfs_register_fs(&fat32_type); }