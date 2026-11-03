"""Compile the production file-capability decoder against synthetic ext4 xattrs."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'kernel/src/sys/capability.c').read_text()
source='\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
code=r'''
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#define BOLDON_CAP_DAC_OVERRIDE (1ULL<<1)
#define BOLDON_CAP_SETUID (1ULL<<7)
#define BOLDON_CAP_SUPPORTED ((1ULL<<1)|(1ULL<<3)|(1ULL<<5)|(1ULL<<6)|(1ULL<<7)|(1ULL<<8)|(1ULL<<21)|(1ULL<<22)|(1ULL<<31))
typedef struct vfs_node vfs_node_t;
typedef struct { int (*getxattr)(vfs_node_t *,const char *,void *,uint64_t); } vfs_node_ops_t;
struct vfs_node { vfs_node_ops_t *ops; };
typedef struct { bool present,effective; uint64_t permitted,inheritable; } boldon_file_caps_t;
typedef struct { uint32_t euid; uint64_t cap_bounding,cap_permitted,cap_effective,
                 cap_inheritable,cap_ambient; bool no_new_privs; } process_t;
int capability_parse_file(const void *, int, boldon_file_caps_t *);
static int result=20;
static uint8_t xattr[24];
static int attr(vfs_node_t *node,const char *name,void *dst,uint64_t size) {
    (void)node;assert(!strcmp(name,"security.capability"));
    if (result < 0) return result;
    assert(size >= (uint64_t)result);
    memcpy(dst,xattr,result);return result;
}
''' + source + r'''
int main(void) {
    vfs_node_ops_t ops={attr}; vfs_node_t n={&ops};boldon_file_caps_t c;
    assert(capability_read_file(NULL,&c)==0 && !c.present);
    result=-ENODATA;
    assert(capability_read_file(&n,&c)==0 && !c.present);
    result=20;
    uint32_t magic=0x02000001U, bit=BOLDON_CAP_SETUID;
    memcpy(xattr,&magic,4);memcpy(xattr+4,&bit,4);
    assert(capability_read_file(&n,&c)==0 && c.present && c.effective);
    assert(c.permitted == BOLDON_CAP_SETUID && c.inheritable == 0);
    result=12;magic=0x01000001U;bit=1U<<31;
    memcpy(xattr,&magic,4);memcpy(xattr+4,&bit,4);
    assert(capability_read_file(&n,&c)==0 && c.permitted==(1ULL<<31));
    result=20;magic=0x02000001U;memcpy(xattr,&magic,4);
    bit=1U<<25;memcpy(xattr+4,&bit,4);
    assert(capability_read_file(&n,&c)==-EOPNOTSUPP);
    bit=BOLDON_CAP_DAC_OVERRIDE;memcpy(xattr+4,&bit,4);
    result=24;magic=0x03000000U;memcpy(xattr,&magic,4);
    uint32_t rootid=1000;memcpy(xattr+20,&rootid,4);
    assert(capability_read_file(&n,&c)==-EOPNOTSUPP);
    rootid=0;memcpy(xattr+20,&rootid,4);
    assert(capability_read_file(&n,&c)==0 && c.present && !c.effective);
    process_t proc={.euid=1000,.cap_bounding=BOLDON_CAP_SUPPORTED,
        .cap_inheritable=BOLDON_CAP_DAC_OVERRIDE,.cap_permitted=BOLDON_CAP_DAC_OVERRIDE};
    c=(boldon_file_caps_t){.present=true,.effective=true,.permitted=BOLDON_CAP_SETUID,
        .inheritable=BOLDON_CAP_DAC_OVERRIDE};
    capability_apply_exec(&proc,&c,false);
    assert(proc.cap_permitted==(BOLDON_CAP_SETUID|BOLDON_CAP_DAC_OVERRIDE));
    assert(proc.cap_effective==proc.cap_permitted);
    proc.cap_permitted=BOLDON_CAP_DAC_OVERRIDE;
    proc.no_new_privs=true;
    capability_apply_exec(&proc,&c,false);
    assert(proc.cap_permitted==BOLDON_CAP_DAC_OVERRIDE);
    c.present=false;proc.euid=0;proc.cap_permitted=BOLDON_CAP_DAC_OVERRIDE;
    capability_apply_exec(&proc,&c,false);
    assert(proc.cap_effective==BOLDON_CAP_DAC_OVERRIDE);
    proc.no_new_privs=false;
    capability_apply_exec(&proc,&c,false);
    assert(proc.cap_effective==BOLDON_CAP_SUPPORTED);
    magic=0x04000000U;memcpy(xattr,&magic,4);
    assert(capability_read_file(&n,&c)==-EINVAL);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src=Path(tmp)/'test.c';exe=Path(tmp)/'test';src.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('File capability parser, exec transitions and fail-closed xattr errors PASS')
