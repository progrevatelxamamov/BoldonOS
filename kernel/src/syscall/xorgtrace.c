#include <syscall/xorgtrace.h>
#include <fs/devfs.h>
#include <fs/vfs.h>
#include <scheduler/spinlock.h>
#include <mm/malloc.h>
#include <gconsole.h>
#include <errno.h>
#include <memory.h>
#include <string.h>
#include <stdio.h>

#define XORGTRACE_CAP 64
#define XORGTRACE_TEXT 12288
typedef struct {
    uint64_t seq, pid, a1, a2;
    int64_t result;
    uint32_t num;
    uint8_t pending, printed;
    const char *stage; /* only static literals from the kernel */
} xorgtrace_entry_t;
static xorgtrace_entry_t entries[XORGTRACE_CAP];
static uint64_t next_seq;
static spinlock_t trace_lock = SPINLOCK_INIT;
static unsigned console_count;
static unsigned syscall_console_count;

typedef struct { uint32_t len; char text[XORGTRACE_TEXT]; } xorgtrace_snapshot_t;

int xorgtrace_exec_target(const char *path) {
    if (!path) return 0;
    const char *base = path;
    for (const char *p = path; *p; p++) if (*p == '/') base = p + 1;
    if (!strcmp(base, "Xorg")) return 2;
    if (!strcmp(base, "Xorg.wrap") || !strcmp(base, "X")) return 1;
    return 0;
}

static uint64_t append(const char *stage, uint64_t pid, uint32_t num,
                       uint64_t a1, uint64_t a2, int64_t result,
                       uint8_t pending, uint8_t printed) {
    uint64_t flags;
    spin_lock_irqsave(&trace_lock, &flags);
    uint64_t seq = ++next_seq;
    entries[(seq - 1) % XORGTRACE_CAP] = (xorgtrace_entry_t){
        .seq = seq, .pid = pid, .num = num, .stage = stage,
        .a1 = a1, .a2 = a2, .result = result, .pending = pending,
        .printed = printed
    };
    spin_unlock_irqrestore(&trace_lock, flags);
    return seq;
}

void xorgtrace_stage(const char *stage, uint64_t pid, uint64_t detail) {
    if (!stage) return;
    append(stage, pid, UINT32_MAX, detail, 0, 0, 0, 0);
    /* Stage markers also reach the screen if Xorg blocks the only shell.
       No per-syscall console prints; limit restarts too. */
    if (__atomic_fetch_add(&console_count, 1, __ATOMIC_RELAXED) < 12) {
        console_print_string("[XORGTRACE] ");
        console_print_string(stage);
        console_print_string(" pid=");
        console_print_num(pid);
        console_print_string(" detail=");
        console_print_hex(detail);
        console_print_string("\n");
    }
}

uint64_t xorgtrace_syscall_enter(uint64_t pid, uint64_t num,
                                uint64_t a1, uint64_t a2) {
    /* Diagnostic fallback when /dev/xorgtrace cannot be read because the
       shell stalls. Never print every syscall: Xorg can issue thousands. */
    unsigned sample = __atomic_fetch_add(&syscall_console_count, 1, __ATOMIC_RELAXED);
    /* Keep a sparse progress signal after the first 512 calls too: the
       guest shell may be unavailable long before Xorg stops making calls. */
    uint8_t printed = sample < 12 ||
                      (sample < 512 && (sample % 64) == 0) ||
                      (sample < 8192 && sample >= 512 && (sample % 512) == 0);
    uint64_t ticket = append("syscall", pid, (uint32_t)num, a1, a2, 0, 1, printed);
    if (printed) {
        console_print_string("[XORGTRACE] enter pid=");
        console_print_num(pid);
        console_print_string(" seq=");
        console_print_num(ticket);
        console_print_string(" num=");
        console_print_num(num);
        console_print_string("\n");
    }
    return ticket;
}

void xorgtrace_syscall_exit(uint64_t ticket, int64_t result) {
    if (!ticket) return;
    uint64_t flags;
    uint8_t printed = 0;
    spin_lock_irqsave(&trace_lock, &flags);
    xorgtrace_entry_t *e = &entries[(ticket - 1) % XORGTRACE_CAP];
    if (e->seq == ticket) {
        e->result = result; e->pending = 0;
        printed = e->printed;
    }
    spin_unlock_irqrestore(&trace_lock, flags);
    if (printed) {
        console_print_string("[XORGTRACE] exit seq=");
        console_print_num(ticket);
        console_print_string(" ret=");
        console_print_hex((uint64_t)result);
        console_print_string("\n");
    }
}

static int trace_open(vfs_file_t *f) {
    if (!f) return -EINVAL;
    xorgtrace_snapshot_t *snap = malloc_alloc(sizeof(*snap));
    if (!snap) { f->private_data = NULL; return 0; }
    xorgtrace_entry_t copy[XORGTRACE_CAP];
    uint64_t flags;
    spin_lock_irqsave(&trace_lock, &flags);
    uint64_t end = next_seq, begin = end > XORGTRACE_CAP ? end - XORGTRACE_CAP : 0;
    for (uint64_t i = begin; i < end; i++) copy[i - begin] = entries[i % XORGTRACE_CAP];
    spin_unlock_irqrestore(&trace_lock, flags);
    int n = snprintf(snap->text, sizeof(snap->text),
                     "xorgtrace seq=%llu..%llu overwritten=%llu (hex args/results; pending=1 means syscall has not returned)\n",
                     (unsigned long long)begin, (unsigned long long)end,
                     (unsigned long long)begin);
    if (n < 0 || n >= (int)sizeof(snap->text)) { malloc_free(snap); return -EOVERFLOW; }
    uint32_t used = (uint32_t)n;
    for (uint64_t i = 0; i < end - begin; i++) {
        xorgtrace_entry_t *e = &copy[i];
        n = snprintf(snap->text + used, sizeof(snap->text) - used,
            "%llu pid=%llu %s num=%u a1=%llx a2=%llx ret=%llx pending=%u\n",
            (unsigned long long)e->seq, (unsigned long long)e->pid, e->stage,
            e->num, (unsigned long long)e->a1, (unsigned long long)e->a2,
            (unsigned long long)e->result, e->pending);
        if (n < 0 || n >= (int)(sizeof(snap->text) - used)) break;
        used += (uint32_t)n;
    }
    snap->len = used;
    f->private_data = snap;
    return 0;
}
static int trace_read(vfs_file_t *f, void *buf, uint64_t count) {
    if (!f || !buf) return -EINVAL;
    xorgtrace_snapshot_t *snap = f->private_data;
    const char *text = snap ? snap->text : "xorgtrace: snapshot allocation failed\n";
    uint64_t len = snap ? snap->len : strlen(text);
    if (!count || f->offset >= len) return 0;
    if (count > len - f->offset) count = len - f->offset;
    memcpy(buf, text + f->offset, (size_t)count);
    f->offset += count;
    return (int)count;
}
static int trace_close(vfs_file_t *f) {
    if (f && f->private_data) malloc_free(f->private_data);
    if (f) f->private_data = NULL;
    return 0;
}
static vfs_file_ops_t trace_ops = {
    .open = trace_open, .read = trace_read, .close = trace_close
};
void xorgtrace_register(void) {
    if (devfs_register_device("xorgtrace", VFS_TYPE_FILE, &trace_ops, NULL))
        console_print_string("[XORGTRACE] cannot register /dev/xorgtrace\n");
}
