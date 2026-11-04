"""Compile the production block cache with a fake block device (write-miss regression)."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'kernel/src/disk/bcache.c').read_text()
source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
code = r'''
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define BCACHE_SLOTS 1024
#define BCACHE_PAGE_SECTORS 8
typedef struct { int unused; } spinlock_t;
#define SPINLOCK_INIT {0}
static void spin_lock_irqsave(spinlock_t *l, uint64_t *f) { (void)l; *f=0; }
static void spin_unlock_irqrestore(spinlock_t *l, uint64_t f) { (void)l; (void)f; }
typedef struct block_device {
    char name[32];
    uint32_t sector_size;
    void *local_device_struct;
    int (*read)(void *,uint64_t,uint32_t,void *);
    int (*write)(void *,uint64_t,uint32_t,void *);
    int (*flush)(void *);
} block_device_t;
static uint8_t disk[16*512];
static int reads, writes, fail_read, fail_write, fail_flush;
static int device_read(void *x,uint64_t lba,uint32_t count,void *out) {
    (void)x; reads++;
    if (fail_read) return -1;
    assert(lba+count<=16); memcpy(out,disk+lba*512,count*512); return 0;
}
static int device_write(void *x,uint64_t lba,uint32_t count,void *in) {
    (void)x; writes++;
    if (fail_write) return -1;
    assert(lba+count<=16); memcpy(disk+lba*512,in,count*512); return 0;
}
static int device_flush(void *x) { (void)x; return fail_flush ? -1 : 0; }
static block_device_t device={"disk",512,NULL,device_read,device_write,device_flush};
static block_device_t *find_device(const char *name) {
    return !strcmp(name,"disk") ? &device : NULL;
}
static void *pmm_alloc_page(void) { return malloc(4096); }
struct bootinfo { struct { uint64_t offset; } *hhdm; };
static struct bootinfo get_bootinfo(void) {
    static struct { uint64_t offset; } hhdm={0};
    return (struct bootinfo){ .hhdm=(void *)&hhdm };
}
''' + source + r'''
int main(void) {
    bcache_init();
    uint8_t full[4096], got[4096];
    memset(full,0xa5,sizeof full);
    assert(bcache_write("disk",0,8,full)==0);
    assert(bcache_read("disk",0,8,got)==0 && !memcmp(got,full,4096));
    assert(reads==0 && writes==0); /* the read must hit the write-miss entry */
    fail_write=1;
    assert(bcache_flush("disk")<0 && writes==1);
    fail_write=0;
    assert(bcache_flush("disk")==0 && writes==2 && !memcmp(disk,full,4096));
    fail_flush=1;
    assert(bcache_flush("disk")<0);
    fail_flush=0;
    assert(bcache_flush("disk")==0);
    fail_read=1;
    uint8_t one[512]={7};
    assert(bcache_write("disk",8,1,one)<0);
    fail_read=0;
    assert(bcache_write("disk",8,1,one)==0);
    assert(bcache_read("disk",8,1,got)==0 && got[0]==7);
    assert(bcache_flush("disk")==0 && disk[8*512]==7);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src=Path(tmp)/'bcache_test.c'
    exe=Path(tmp)/'bcache_test'
    src.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('Block cache write-miss coherency, persistence and I/O failures PASS')
