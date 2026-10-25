#include "kernel.h"
#include "mm/pmm.h"
#include <desc/idt.h>
#include <desc/gdt.h>
#include <stdint.h>
#include <stdbool.h>
#include <gconsole.h>
#include <cpu/pic.h>
#include <cpu/apic.h>
#include <scheduler/scheduler.h>
#include <inout/ps2.h>
#include <io.h>
#include <memory.h>

static const char *exception_names[32] = {
    "Divide-by-zero Error",
    "Debug",
    "Non-maskable Interrupt",
    "Breakpoint",
    "Overflow",
    "Bound Range Exceeded",
    "Invalid Opcode",
    "Device Not Available",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Invalid TSS",
    "Segment Not Present",
    "Stack-Segment Fault",
    "General Protection Fault",
    "Page Fault",
    "Reserved",
    "x87 Floating-Point Exception",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating-Point Exception",
    "Virtualization Exception",
    "Control Protection Exception",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Hypervisor Injection Exception",
    "VMM Communication Exception",
    "Security Exception",
    "Reserved"
};

__attribute__((aligned(0x10))) 
static idt_entry_t idt[256];
static idtr_t idtr;

static bool vectors[256];

extern void* isr_stub_table[];

void idt_set_descriptor(uint8_t vector, void* isr, uint8_t flags) {
    idt_entry_t* descriptor = &idt[vector];

    descriptor->isr_low        = (uint64_t)isr & 0xFFFF;
    descriptor->kernel_cs      = KERNEL_CS;
    descriptor->ist            = 0;
    descriptor->attributes     = flags;
    descriptor->isr_mid        = ((uint64_t)isr >> 16) & 0xFFFF;
    descriptor->isr_high       = ((uint64_t)isr >> 32) & 0xFFFFFFFF;
    descriptor->reserved       = 0;
}

void idt_reload(void) { __asm__ volatile ("lidt %0" : : "m"(idtr)); }

void idt_init(void) {
    idtr.base = (uintptr_t)&idt[0];
    idtr.limit = sizeof(idt) - 1;

    for (uint16_t vector = 0; vector < 256; vector++) {
        idt_set_descriptor((uint8_t)vector, isr_stub_table[vector], 0x8E);
        vectors[vector] = true;
    }

    idt_reload();
}

uint64_t interrput_timer = 0;

uint64_t page_fault_handler(struct interrupt_frame *frame) {
    uint64_t fault_addr;
    __asm__ volatile("mov %%cr2, %0" : "=r"(fault_addr));

    uint64_t err = frame->error_code;
    int user_fault = (err & 4) != 0;      /* U/S */
    int present    = (err & 1) != 0;      /* P */
    process_t *p   = get_current_process();
    thread_t *thr  = get_current_thread();

    if (!present &&
        fault_addr >= 0x10000ULL &&
        fault_addr <  0x00007FFFFFFFFFFFULL &&
        p && p->pml4)
    {
        uint64_t page = fault_addr & ~0xFFFULL;
        if (!vmm_is_mapped(p->pml4, page)) {
            void *phys = pmm_alloc_page_and_clear();
            if (phys) {
                vmm_map_page(p->pml4, (uint64_t)phys, page,
                             PTE_PRESENT | PTE_WRITABLE | PTE_USER);
                __asm__ volatile("invlpg (%0)" :: "r"(page) : "memory");
                return 0;
            }
        } else {
            __asm__ volatile("invlpg (%0)" :: "r"(fault_addr) : "memory");
            return 0;
        }
    }
    return 1;
}

extern uint64_t sys_exit_impl(uint64_t);

static volatile int panic_active = 0;

static void dump_code_bytes(uint64_t rip, uint64_t cs, process_t *proc) {
    uint64_t page = rip & ~0xFFFULL;
    uint64_t phys = 0;

    if (cs & 3) {
        if (proc && proc->pml4) phys = vmm_get_phys(proc->pml4, page);
    } else {
        phys = vmm_get_phys(get_kernel_pml4(), page);
    }

    if (!phys) {
        console_print_string("\nCODE: <unmapped>\n");
        return;
    }

    uint8_t *code = (uint8_t *)(phys + get_bootinfo().hhdm->offset);
    uint64_t off = rip & 0xFFFULL;
    uint64_t start = (off >= 8) ? (off - 8) : 0;
    uint64_t end = start + 24;
    if (end > 4096) end = 4096;

    console_print_string("\nCODE:");
    for (uint64_t i = start; i < end; i++) {
        if (i == off) console_print_string(" >>");
        console_print_hex(code[i]);
        console_print_string(" ");
    }
    console_print_string("\n");
}

static void dump_frame_raw(struct interrupt_frame *frame) {
    uint64_t w[22];
    memcpy(w, frame, sizeof(w));
    console_print_string("\nFRAME:");
    for (int i = 0; i < 22; i++) {
        console_print_string(" ");
        console_print_hex(w[i]);
    }
    console_print_string("\n");
}

void execption_handlerer(struct interrupt_frame *frame) {
    if (frame->isr_number > 31) return;
    if (frame->isr_number == 22 || frame->isr_number == 23) return;

    if (frame->isr_number == 14) {
        if (page_fault_handler(frame) == 0) return;
    }

    process_t *proc = get_current_process();
    bool kernel_proc = proc && proc->is_kernel_process;
    bool user_mode = (frame->cs & 3) == 3;
    bool exit_process = user_mode || !kernel_proc;

    if (__atomic_exchange_n(&panic_active, 1, __ATOMIC_SEQ_CST)) {
        console_print_string("\n[PANIC] nested exception vector=");
        console_print_num(frame->isr_number);
        console_print_string(" rip=");
        console_print_hex(frame->rip);
        console_print_string("\n");
        while (1) {
            __asm__ volatile ("cli; hlt");
        }
    }

    uint64_t fs  = rdmsr(0xC0000100);
    uint64_t gs  = rdmsr(0xC0000101);
    uint64_t kgs = rdmsr(0xC0000102);

    uint64_t fault_addr;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(fault_addr));

    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));

    uint64_t omaigat;
    __asm__ volatile("mov %%cr4, %0" : "=r"(omaigat));

    thread_t *thr = get_current_thread();
    int thr_index = -1;
    if (proc) {
        for (uint64_t i = 0; i < 256; i++) {
            if (thr == proc->threads[i]) {
                thr_index = i;
                break;
            }
        }
    }

    if (kernel_proc) console_print_string("\nKernel panic!\nExecption: ");
    else console_print_string("\nUser panic!\nExecption: ");
    console_print_string(exception_names[frame->isr_number]);
    console_print_string("\nMode:"); console_print_string(user_mode ? "user" : "kernel");
    console_print_string("\nRIP:"); console_print_hex(frame->rip);
    console_print_string("\nCR2:"); console_print_hex(fault_addr);
    console_print_string("\nCR4:"); console_print_hex(omaigat);
    console_print_string("\nERR:"); console_print_hex(frame->error_code);
    console_print_string("\nRAX:"); console_print_hex(frame->rax);
    console_print_string("\nRBX:"); console_print_hex(frame->rbx);
    console_print_string("\nRCX:"); console_print_hex(frame->rcx);
    console_print_string("\nRDX:"); console_print_hex(frame->rdx);
    console_print_string("\nRSI:"); console_print_hex(frame->rsi);
    console_print_string("\nRDI:"); console_print_hex(frame->rdi);
    console_print_string("\nRBP:"); console_print_hex(frame->rbp);
    console_print_string("\nRSP:"); console_print_hex(frame->rsp);
    console_print_string("\nFS_BASE:"); console_print_hex(fs);
    console_print_string("\nGS_BASE:"); console_print_hex(gs);
    console_print_string("\nKERNEL_GS_BASE:"); console_print_hex(kgs);
    console_print_string("\nCR3:");    console_print_hex(cr3);
    console_print_string("\nCS:");  console_print_hex(frame->cs);
    console_print_string("\nSS:");  console_print_hex(frame->ss);
    console_print_string("\nRFLAGS:"); console_print_hex(frame->rflags);
    console_print_string("\nProcess name: ");
    if (proc && proc->name) console_print_string(proc->name);
    else console_print_string("<null>");
    if ( thr_index != -1) console_print_string("\nThread num: "); console_print_num(thr_index);
    console_print_string("\nCpu num: "); console_print_num(get_cpu_id());
    console_print_string("\nStack guard: ");
    if (thr && thr->kernel_stack) {
        if (thread_stack_guard_ok(thr)) console_print_string("OK");
        else console_print_string("BROKEN");
    } else {
        console_print_string("N/A");
    }
    if (thr && thr->regs) {
        console_print_string("\nSaved regs rip: "); console_print_hex(thr->regs->rip);
        console_print_string(" cs: "); console_print_hex(thr->regs->cs);
        console_print_string(" rsp: "); console_print_hex(thr->regs->rsp);
        console_print_string(" rflags: "); console_print_hex(thr->regs->rflags);
    }
    if (thr && thr->kernel_stack) {
        uint64_t rsp_now;
        __asm__ volatile("mov %%rsp, %0" : "=r"(rsp_now));
        console_print_string("\nKstack: "); console_print_hex(thr->kernel_stack);
        console_print_string(" - "); console_print_hex(thr->kernel_stack_top);
        console_print_string("\nCur rsp: "); console_print_hex(rsp_now);
    }
    dump_frame_raw(frame);
    dump_code_bytes(frame->rip, frame->cs, proc);

    if (exit_process && proc) {
        __atomic_store_n(&panic_active, 0, __ATOMIC_SEQ_CST);
        sys_exit_impl(139); // 128 + SIGSEGV (11) = 139
        schedule_yield();
        return;
    }

    console_print_string("\n");
    console_print_string("\nHalted.");
    while (1) {
        __asm__ volatile ( "cli; hlt" );
    }
}

uint64_t g_ticks_ms;

void exception_handler(struct interrupt_frame *frame) {
    wrmsr(0xC0000102, (uint64_t)get_current_local_data());
    // console_print_string("!");
    // console_print_hex(frame->rip);
    // console_print_string(";");
    // console_print_num(get_cpu_id());
    // console_print_string(";");
    // console_print_num(frame->isr_number);
    // console_print_string("!");
    execption_handlerer(frame);

    if (frame->isr_number == 23) {
        interrput_timer = 0;
        schedule(frame);
        return; 
    }

    if (frame->isr_number == 22) {
        create_task_kernel((void*)frame->rip, "kernel");
        return;
    }

    if (frame->isr_number == 0xFD) { // Reschedule IPI
        apic_send_eoi();
        schedule(frame);
        return;
    }

    if (frame->isr_number == 0xFC) { // TLB Flush IPI
        apic_send_eoi();
        // Полный сброс TLB (перезагрузка текущего CR3)
        uint64_t cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
        return;
    }

    if (get_pic_state()) {
        PIC_sendEOI(frame->isr_number - 32);
    } else {
        apic_send_eoi();
    }

    // ВЫРАВНИВАНИЕ ТАЙМЕРА ДЛЯ SMP:
    if (frame->isr_number == 32) {
        int cpu = get_cpu_id();
        
        // Только главное ядро (BSP) увеличивает системные часы g_ticks_ms
        if (cpu == 0) {
            g_ticks_ms++;
            interrput_timer++;
            if (interrput_timer >= 2) {
                interrput_timer = 0;
                schedule(frame);
            }
        } else {
            // Второстепенные ядра (AP) просто делают квантование планировщика
            schedule(frame);
        }
    }

    if (frame->isr_number == 33) {
        ps2_irq_keyboard();
    }

    if (frame->isr_number == 44) {
        ps2_irq_mouse();
    }
}