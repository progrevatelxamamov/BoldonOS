#include <cpu/smp.h>
#include <cpu/apic.h>
#include <cpu/acpi.h>
#include <bootloader.h>
#include <kernel.h>
#include <desc/gdt.h>
#include <desc/idt.h>
#include <gconsole.h>
#include <io.h>
#include <memory.h>
#include <scheduler/scheduler.h>
#include <syscall/syscall.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define SMP_TIMER_VECTOR 32
#define SMP_TIMER_HZ 1000
#define SMP_AP_STACK_SIZE 16384
#define SMP_AP_TIMEOUT_MS 1000

#define IA32_GS_BASE 0xC0000101
#define IA32_KERNEL_GS_BASE 0xC0000102

#define SMP_STATE_OFFLINE 0
#define SMP_STATE_STARTING 1
#define SMP_STATE_ONLINE 2
#define SMP_STATE_FAILED 3

struct smp_cpu {
    uint32_t apic_id;
    uint32_t state;
    struct limine_mp_info *info;
};

static struct smp_cpu smp_cpus[SMP_MAX_CPUS];
static uint32_t smp_num_cpus = 0;

static uint8_t smp_ap_stacks[SMP_MAX_CPUS][SMP_AP_STACK_SIZE] __attribute__((aligned(16)));

volatile uint32_t ap_cores_booted = 0;

extern void smp_ap_entry(struct limine_mp_info *info);

_Static_assert(offsetof(struct limine_mp_info, extra_argument) == 24, "limine_mp_info layout");

static uint32_t smp_this_apic_id(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    return (ebx >> 24) & 0xFF;
}

static int smp_logical_from_apic(uint32_t apic_id) {
    for (uint32_t i = 0; i < smp_num_cpus; i++) {
        if (smp_cpus[i].apic_id == apic_id) return (int)i;
    }
    return -1;
}

int cpu_get_logical_id(void) {
    int id = smp_logical_from_apic(smp_this_apic_id());
    return id >= 0 ? id : 0;
}

uint32_t smp_cpu_count(void) {
    return smp_num_cpus;
}

uint32_t smp_online_count(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < smp_num_cpus; i++) {
        if (__atomic_load_n(&smp_cpus[i].state, __ATOMIC_ACQUIRE) == SMP_STATE_ONLINE) n++;
    }
    return n;
}

uint32_t smp_apic_id(int logical_id) {
    if (logical_id < 0 || (uint32_t)logical_id >= smp_num_cpus) return 0xFFFFFFFFu;
    return smp_cpus[logical_id].apic_id;
}

bool smp_cpu_online(int logical_id) {
    if (logical_id < 0 || (uint32_t)logical_id >= smp_num_cpus) return false;
    return __atomic_load_n(&smp_cpus[logical_id].state, __ATOMIC_ACQUIRE) == SMP_STATE_ONLINE;
}

static void percpu_init(int logical_id) {
    struct cpu_local_data *local = &cpu_locals[logical_id];
    memset(local, 0, sizeof(*local));
    local->self = local;
    local->cpu_id = logical_id;
    local->kernel_rsp = (uint64_t)smp_ap_stacks[logical_id] + SMP_AP_STACK_SIZE;
    wrmsr(IA32_GS_BASE, 0);
    wrmsr(IA32_KERNEL_GS_BASE, (uint64_t)local);
}

static void smp_set_online(int logical_id) {
    __atomic_store_n(&smp_cpus[logical_id].state, SMP_STATE_ONLINE, __ATOMIC_RELEASE);
    __atomic_add_fetch(&ap_cores_booted, 1, __ATOMIC_SEQ_CST);
}

void smp_ap_main(struct limine_mp_info *info) {
    int logical_id = smp_logical_from_apic(info->lapic_id);
    if (logical_id < 0) {
        for (;;) {
            __asm__ volatile("cli; hlt");
        }
    }

    enable_sse();
    gdt_init_cpu(logical_id);
    idt_reload();
    percpu_init(logical_id);
    syscall_init();
    apic_init_cpu(SMP_TIMER_VECTOR, SMP_TIMER_HZ);

    smp_set_online(logical_id);

    console_print_string("[SMP] AP logical=");
    console_print_num((uint64_t)logical_id);
    console_print_string(" apic=");
    console_print_num(info->lapic_id);
    console_print_string(" online\n");

    while (!scheduler_is_ready()) {
        __asm__ volatile("pause");
    }

    apic_timer_unmask_cpu();

    for (;;) {
        __asm__ volatile("sti; hlt");
    }
}

static bool smp_wait_online(int logical_id) {
    for (uint32_t t = 0; t < SMP_AP_TIMEOUT_MS; t++) {
        if (__atomic_load_n(&smp_cpus[logical_id].state, __ATOMIC_ACQUIRE) == SMP_STATE_ONLINE) {
            return true;
        }
        hpet_sleep_ms(1);
        for (int i = 0; i < 10000; i++) {
            __asm__ volatile("pause");
        }
    }
    return __atomic_load_n(&smp_cpus[logical_id].state, __ATOMIC_ACQUIRE) == SMP_STATE_ONLINE;
}

static void smp_register(struct limine_mp_info *info) {
    if (smp_num_cpus >= SMP_MAX_CPUS) return;
    if (smp_logical_from_apic(info->lapic_id) >= 0) return;
    smp_cpus[smp_num_cpus].apic_id = info->lapic_id;
    smp_cpus[smp_num_cpus].state = SMP_STATE_OFFLINE;
    smp_cpus[smp_num_cpus].info = info;
    smp_num_cpus++;
}

static void smp_init_single(void) {
    smp_num_cpus = 1;
    smp_cpus[0].apic_id = smp_this_apic_id();
    smp_cpus[0].state = SMP_STATE_STARTING;
    smp_cpus[0].info = NULL;
    percpu_init(0);
    smp_set_online(0);
    console_print_string("[SMP] MP response missing, running on 1 CPU\n");
}

void smp_init(void) {
    struct limine_mp_response *mp = get_bootinfo().mp;

    if (!mp || !mp->cpus || mp->cpu_count == 0) {
        smp_init_single();
        return;
    }

    for (uint64_t i = 0; i < mp->cpu_count; i++) {
        if (mp->cpus[i] && mp->cpus[i]->lapic_id == mp->bsp_lapic_id) {
            smp_register(mp->cpus[i]);
            break;
        }
    }
    for (uint64_t i = 0; i < mp->cpu_count; i++) {
        if (mp->cpus[i]) smp_register(mp->cpus[i]);
    }

    if (smp_num_cpus == 0) {
        smp_init_single();
        return;
    }

    percpu_init(0);
    smp_set_online(0);

    console_print_string("[SMP] CPUs detected: ");
    console_print_num(smp_num_cpus);
    console_print_string(" BSP apic=");
    console_print_num(mp->bsp_lapic_id);
    console_print_string("\n");

    for (uint32_t id = 1; id < smp_num_cpus; id++) {
        struct limine_mp_info *info = smp_cpus[id].info;
        if (!info) continue;

        __atomic_store_n(&smp_cpus[id].state, SMP_STATE_STARTING, __ATOMIC_RELEASE);
        info->extra_argument = (uint64_t)smp_ap_stacks[id] + SMP_AP_STACK_SIZE;

        limine_goto_address entry = smp_ap_entry;
        __atomic_store(&info->goto_address, &entry, __ATOMIC_SEQ_CST);

        if (!smp_wait_online((int)id)) {
            __atomic_store_n(&smp_cpus[id].state, SMP_STATE_FAILED, __ATOMIC_RELEASE);
            console_print_string("[SMP] AP logical=");
            console_print_num(id);
            console_print_string(" apic=");
            console_print_num(smp_cpus[id].apic_id);
            console_print_string(" timeout\n");
        }
    }

    console_print_string("[SMP] online: ");
    console_print_num(smp_online_count());
    console_print_string("/");
    console_print_num(smp_num_cpus);
    console_print_string("\n");
}
