/* Static ELF guest smoke test. Build with: cc -static -O2 -o sh guest_cap_inotify_probe.c
 * Place it at /bin/sh in an ext4 partition named BOLDON_ROOT (GPT name),
 * with a writable /tmp. Boot the kernel with a Limine base revision 6 loader.
 * The init runner executes /bin/sh /etc/init.sh; this probe ignores argv.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/syscall.h>
#include <unistd.h>

static int failed;
static void result(const char *name, long got, long expected) {
    char line[160];
    int error = errno;
    if (got != expected) failed++;
    int n = snprintf(line, sizeof(line), "[GUEST-PROBE] %s: %ld (expected %ld), errno=%d\n",
                     name, got, expected, error);
    write(1, line, n);
    errno = 0;
}

int main(void) {
    struct __user_cap_header_struct hdr = { .version = 0x20080522, .pid = 0 };
    struct __user_cap_data_struct caps[2] = {0};
    result("capget", syscall(SYS_capget, &hdr, caps), 0);
    if (!(caps[0].effective & (1U << 31))) failed++;

    int fd = syscall(SYS_inotify_init1, IN_NONBLOCK);
    if (fd < 0) failed++;
    int wd = syscall(SYS_inotify_add_watch, fd, "/tmp", IN_CREATE | IN_MOVED_TO);
    if (wd < 0) failed++;
    int src = syscall(SYS_open, "/tmp/src", O_CREAT | O_RDWR, 0644);
    if (src < 0) failed++;
    if (src >= 0) {
        result("write source", syscall(SYS_write, src, "source", 6), 6);
        syscall(SYS_close, src);
    }
    int dst = syscall(SYS_open, "/tmp/dst", O_CREAT | O_RDWR, 0644);
    if (dst < 0) failed++;
    if (dst >= 0) {
        result("write target", syscall(SYS_write, dst, "target", 6), 6);
        syscall(SYS_close, dst);
    }
    char events[512];
    if (syscall(SYS_read, fd, events, sizeof(events)) <= 0) failed++;

    unsigned char filecap[20] = {1, 0, 0, 2, 2}; /* v2 effective + DAC_OVERRIDE */
    unsigned char readback[24] = {0};
    result("setxattr", syscall(SYS_setxattr, "/tmp/src", "security.capability",
                               filecap, sizeof(filecap), 0), 0);
    result("getxattr", syscall(SYS_getxattr, "/tmp/src", "security.capability",
                               readback, sizeof(readback)), 20);
    if (memcmp(filecap, readback, 20)) failed++;
    result("replace regular file", syscall(SYS_rename, "/tmp/src", "/tmp/dst"), 0);
    dst = syscall(SYS_open, "/tmp/dst", O_RDONLY, 0);
    char contents[7] = {0};
    if (dst < 0) failed++;
    else {
        result("read replacement", syscall(SYS_read, dst, contents, 6), 6);
        if (strcmp(contents, "source")) failed++;
        result("fsync", syscall(SYS_fsync, dst), 0);
        syscall(SYS_close, dst);
    }
    result("mkdir A", syscall(SYS_mkdir, "/tmp/dirA", 0755), 0);
    result("mkdir B", syscall(SYS_mkdir, "/tmp/dirB", 0755), 0);
    result("replace empty directory", syscall(SYS_rename, "/tmp/dirA", "/tmp/dirB"), 0);
    result("mkdir after block reuse", syscall(SYS_mkdir, "/tmp/dirB/sub", 0755), 0);
    result("access new subdirectory", syscall(SYS_access, "/tmp/dirB/sub", 0), 0);
    result("mkdir nonempty target", syscall(SYS_mkdir, "/tmp/occupied", 0755), 0);
    int occupied_file = syscall(SYS_open, "/tmp/occupied/child", O_CREAT | O_RDWR, 0644);
    if (occupied_file < 0) failed++;
    else syscall(SYS_close, occupied_file);
    long nonempty = syscall(SYS_rename, "/tmp/dirB", "/tmp/occupied");
    if (nonempty != -1 || errno != ENOTEMPTY) failed++;
    result("reject nonempty directory", nonempty, -1);
    long cycle = syscall(SYS_rename, "/tmp/dirB", "/tmp/dirB/sub/cycle");
    if (cycle != -1 || errno != EINVAL) failed++;
    result("reject descendant move", cycle, -1);
    result("move directory between parents",
           syscall(SYS_rename, "/tmp/dirB/sub", "/tmp/occupied/moved"), 0);
    result("access moved directory", syscall(SYS_access, "/tmp/occupied/moved", 0), 0);
    result("fsync directory", syscall(SYS_fsync, 1), 0);

    memset(caps, 0, sizeof(caps));
    result("drop capabilities", syscall(SYS_capset, &hdr, caps), 0);
    result("capget after drop", syscall(SYS_capget, &hdr, caps), 0);
    if (caps[0].effective || caps[0].permitted) failed++;
    long denied = syscall(SYS_open, "/bin/forbidden", O_CREAT | O_RDWR, 0644);
    if (errno != EACCES) failed++;
    result("deny create in non-writable /bin", denied, -1);

    /* Only syscall behavior is checked here; inspect the ext4 image with
       e2fsck after stopping the VM before claiming filesystem correctness. */
    const char *verdict = failed ? "[GUEST-PROBE] FAIL\n" :
                                   "[GUEST-PROBE] RUNTIME PASS (fsck required)\n";
    write(1, verdict, strlen(verdict));
    for (;;) syscall(SYS_sched_yield);
}
