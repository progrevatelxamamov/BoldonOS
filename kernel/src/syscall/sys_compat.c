/* Linux ABI calls needed by userspace startup. Do not report success for
   memory operations whose effects BoldonOS does not implement. */
#include <cpu/smp.h>
#include <errno.h>
#include <scheduler/scheduler.h>
#include <stdint.h>
#include <stdbool.h>
#include <memory.h>

extern process_t *tasks[];

static bool user_range_ok(uint64_t ptr, uint64_t size) {
    uint64_t end;
    return ptr >= 0x10000 && !__builtin_add_overflow(ptr, size, &end) &&
           end <= 0x00007FFFFFFFFFFFULL;
}

uint64_t sys_sched_getaffinity_impl(uint64_t pid, uint64_t size, uint64_t mask_ptr) {
    process_t *current = get_current_process();
    if (!current) return (uint64_t)-ESRCH;
    if (pid != 0 && pid != current->pid) {
        bool found = false;
        for (int i = 1; i < 512; i++) {
            process_t *p = tasks[i];
            if (p && p->pid == pid && p->state != TASK_DEAD &&
                p->state != TASK_ZOMBIE) { found = true; break; }
        }
        if (!found) return (uint64_t)-ESRCH;
    }

    uint32_t count = smp_cpu_count();
    if (count > SMP_MAX_CPUS) count = SMP_MAX_CPUS;
    /* Linux returns the size of its CPU mask in bytes, not the caller's
       buffer size. A small system needs at least one unsigned long. */
    uint32_t bytes = ((count + 63) / 64) * sizeof(uint64_t);
    if (bytes < sizeof(uint64_t)) bytes = sizeof(uint64_t);
    if (size < bytes) return (uint64_t)-EINVAL;
    if (!user_range_ok(mask_ptr, bytes)) return (uint64_t)-EFAULT;

    uint8_t *out = (uint8_t *)mask_ptr;
    memset(out, 0, bytes);
    for (uint32_t i = 0; i < count; i++)
        if (smp_cpu_online((int)i)) out[i / 8] |= (uint8_t)(1u << (i % 8));
    return bytes;
}

uint64_t sys_madvise_impl(uint64_t addr, uint64_t len, uint64_t advice) {
    if ((addr & 4095) != 0) return (uint64_t)-EINVAL;
    if (len && !user_range_ok(addr, len)) return (uint64_t)-ENOMEM;
    /* These are performance hints, so ignoring them preserves memory and
       contents. MADV_DONTNEED/FREE/REMOVE must not be faked: callers may
       rely on pages being discarded or zero-filled on the next access. */
    switch (advice) {
    case 0: /* MADV_NORMAL */
    case 1: /* MADV_RANDOM */
    case 2: /* MADV_SEQUENTIAL */
    case 3: /* MADV_WILLNEED */
        return 0;
    default:
        return (uint64_t)-ENOSYS;
    }
}
