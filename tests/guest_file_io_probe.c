/* Freestanding Linux-syscall probe run as /bin/sh in a disposable BoldonOS
 * QEMU ext4 image. stderr is the serial console; stdout is intentionally
 * redirected in one test. This is NOT a shell or a Plasma test. */
#include <stddef.h>
#include <stdint.h>

static long call(long n, long a, long b, long c) {
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(n), "D"(a), "S"(b), "d"(c)
                      : "rcx", "r11", "r10", "memory");
    return ret;
}
static void say(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    call(1, 2, (long)s, n);
}
static void number(long n) {
    char b[32]; int i = 31;
    b[i--] = '\n';
    if (n < 0) { say("-"); n = -n; }
    do { b[i--] = '0' + n % 10; n /= 10; } while (n);
    call(1, 2, (long)&b[i + 1], 31 - i);
}
static int equal(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}
static void failure(const char *step, long rc) {
    say("PROBE FAIL "); say(step); say(" rc="); number(rc);
}
static int file_readback(const char *path, const char *data, size_t size) {
    char buf[128] = {0};
    long fd = call(2, (long)path, 0, 0);
    if (fd < 0) return (int)fd;
    long n = call(0, fd, (long)buf, sizeof(buf));
    call(3, fd, 0, 0);
    if (n != (long)size || !equal(buf, data, size)) return -1000;
    return 0;
}
static int file_write(const char *path, const char *data, size_t size) {
    long fd = call(2, (long)path, 1 | 0100 | 01000, 0644);
    if (fd < 0) return (int)fd;
    long n = call(1, fd, (long)data, size);
    if (n != (long)size) { call(3, fd, 0, 0); return n < 0 ? (int)n : -1001; }
    long sync = call(74, fd, 0, 0);
    long close = call(3, fd, 0, 0);
    if (sync < 0) return (int)sync;
    return (int)close;
}
static int test(void) {
    const char *path = "/write-test";
    int rc = file_readback(path, "persist-data", 12);
    if (rc == 0) say("PROBE REBOOT PASS persisted bytes\n");
    else if (rc == -2) say("PROBE FIRST BOOT\n");
    else { failure("persisted read", rc); return 1; }

    rc = file_write(path, "persist-data", 12);
    if (rc) { failure("write/fsync/close", rc); return 1; }
    rc = file_readback(path, "persist-data", 12);
    if (rc) { failure("reopen/read", rc); return 1; }
    say("PROBE PASS write-close-reopen-read bytes\n");

    /* dup(oldfd) must allocate a new fd, never overwrite stdin (fd 0). */
    long duplicate = call(32, 2, 0, 0);
    if (duplicate < 3) { failure("dup allocated fd", duplicate); return 1; }
    call(3, duplicate, 0, 0);
    say("PROBE PASS dup preserves stdin\n");

    /* Exercise precisely the shell's O_CREAT|O_TRUNC -> dup2(1) path. */
    long fd = call(2, (long)"/redirect-test", 1 | 0100 | 01000, 0644);
    if (fd < 0) { failure("redirect open", fd); return 1; }
    rc = (int)call(33, fd, 1, 0);
    if (rc != 1) { failure("dup2 stdout", rc); return 1; }
    long n = call(1, 1, (long)"redirect-data", 13);
    call(33, 2, 1, 0); /* restore original console from stderr */
    call(3, fd, 0, 0);
    if (n != 13) { failure("redirected write", n); return 1; }
    rc = file_readback("/redirect-test", "redirect-data", 13);
    if (rc) { failure("redirected readback", rc); return 1; }
    say("PROBE PASS redirected fd1 write/read\n");

    for (int i = 0; i < 32; i++) {
        rc = file_write("/repeat-test", "data", 4);
        if (!rc) rc = file_readback("/repeat-test", "data", 4);
        if (rc) { failure("repeated truncate/write/read", rc); return 1; }
    }
    say("PROBE PASS 32 repeated truncate/write/read cycles\n");

    /* The shell forks after setting up a redirected descriptor. Parent
       close must not destroy the child's inherited open file description. */
    for (int i = 0; i < 8; i++) {
        fd = call(2, (long)"/fork-write-test", 1 | 0100 | 01000, 0644);
        if (fd < 0) { failure("fork test open", fd); return 1; }
        long child = call(57, 0, 0, 0);
        if (child == 0) {
            long wrote = call(1, fd, (long)"fork-data", 9);
            call(3, fd, 0, 0);
            call(231, wrote == 9 ? 0 : 1, 0, 0);
            for (;;) {} /* exit_group must not return */
        }
        call(3, fd, 0, 0);
        if (child < 0) { failure("fork", child); return 1; }
        int status = -1;
        long waited = call(61, child, (long)&status, 0);
        if (waited != child || status != 0) {
            failure("fork child write or wait", waited == child ? status : waited);
            return 1;
        }
        rc = file_readback("/fork-write-test", "fork-data", 9);
        if (rc) { failure("fork file readback", rc); return 1; }
    }
    say("PROBE PASS 8 fork/parent-close/child-write cycles\n");
    return 0;
}
void probe_main(void) {
    say("PROBE START\n");
    if (!test()) say("PROBE ALL PASS\n");
    static const long timespec[2] = {10, 0};
    for (;;) call(35, (long)timespec, 0, 0);
}
/* The freestanding ELF entry receives no C call frame. */
__asm__(".global _start\n_start:\n andq $-16, %rsp\n call probe_main\n1: jmp 1b\n");
