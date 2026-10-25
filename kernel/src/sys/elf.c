#include "gconsole.h"
#include "kernel.h"
#include <sys/elf.h>
#include <fs/vfs.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <mm/malloc.h>
#include <desc/gdt.h>
#include <scheduler/scheduler.h>
#include <memory.h>
#include <errno.h>
#include <io.h>

#define STACK_SIZE (2 * 1024 * 1024)
#define STACK_TOP  0x7FFFF000000ULL

#define IA32_FS_BASE         0xC0000100
#define IA32_GS_BASE         0xC0000101
#define IA32_KERNEL_GS_BASE  0xC0000102

#define EM_X86_64 62

extern void scheduler_update_pml4(page_table_t *new_pml4);

static int map_segment(page_table_t *pml4, vfs_file_t *f,
                       Elf64_Phdr *phdr, uint64_t load_bias)
{
    uint64_t vaddr  = phdr->p_vaddr + load_bias;
    uint64_t memsz  = phdr->p_memsz;
    uint64_t filesz = phdr->p_filesz;
    uint64_t offset = phdr->p_offset;

    if (memsz == 0) return 0;
    if (filesz > memsz) return -1;

    uint64_t start_page = vaddr & ~0xFFFULL;
    uint64_t end_page   = (vaddr + memsz + 0xFFFULL) & ~0xFFFULL;
    uint64_t pages      = (end_page - start_page) / 4096;

    uint64_t prot = PTE_PRESENT | PTE_USER;
    if (phdr->p_flags & PF_W) prot |= PTE_WRITABLE;
    if (!(phdr->p_flags & PF_X)) prot |= PTE_NX;

    for (uint64_t i = 0; i < pages; i++) {
        uint64_t page_vaddr = start_page + i * 4096;
        if (!vmm_is_mapped(pml4, page_vaddr)) {
            void *phys = pmm_alloc_page_and_clear();
            if (!phys) return -1;
            vmm_map_page(pml4, (uint64_t)phys, page_vaddr, prot);
        }
    }

    if (filesz > 0) {
        uint64_t done = 0;
        while (done < filesz) {
            uint64_t cur   = vaddr + done;
            uint64_t poff  = cur & 0xFFF;
            uint64_t chunk = 4096 - poff;
            if (chunk > filesz - done) chunk = filesz - done;

            uint64_t phys = vmm_get_phys(pml4, cur & ~0xFFFULL);
            if (!phys) return -1;

            void *dst = (void *)(phys + get_bootinfo().hhdm->offset + poff);
            vfs_lseek(f, (int64_t)(offset + done), VFS_SEEK_SET);
            int got = vfs_read(f, dst, chunk);
            if (got <= 0) return -1;
            done += (uint64_t)got;
        }
    }

    if (memsz > filesz) {
        uint64_t done = filesz;
        while (done < memsz) {
            uint64_t cur   = vaddr + done;
            uint64_t poff  = cur & 0xFFF;
            uint64_t chunk = 4096 - poff;
            if (chunk > memsz - done) chunk = memsz - done;

            uint64_t phys = vmm_get_phys(pml4, cur & ~0xFFFULL);
            if (!phys) return -1;

            void *dst = (void *)(phys + get_bootinfo().hhdm->offset + poff);
            memset(dst, 0, chunk);
            done += chunk;
        }
    }

    return 0;
}

static int load_interp(page_table_t *pml4, const char *interp_path,
                       uint64_t interp_base, uint64_t *entry_out)
{
    vfs_file_t *ifile = vfs_open(interp_path, VFS_O_READ);
    if (!ifile) return -ENOENT;

    Elf64_Ehdr iehdr;
    if (vfs_read(ifile, &iehdr, sizeof(iehdr)) != sizeof(iehdr)) {
        vfs_close(ifile);
        return -ENOEXEC;
    }
    if (iehdr.e_ident[0] != 0x7F || iehdr.e_ident[1] != 'E' ||
        iehdr.e_ident[2] != 'L'  || iehdr.e_ident[3] != 'F') {
        vfs_close(ifile);
        return -ENOEXEC;
    }

    uint64_t phnum = iehdr.e_phnum;
    Elf64_Phdr *iphdrs = malloc_alloc(phnum * sizeof(Elf64_Phdr));
    if (!iphdrs) { vfs_close(ifile); return -ENOMEM; }

    vfs_lseek(ifile, iehdr.e_phoff, VFS_SEEK_SET);
    if (vfs_read(ifile, iphdrs, phnum * sizeof(Elf64_Phdr)) !=
        (int)(phnum * sizeof(Elf64_Phdr))) {
        malloc_free(iphdrs);
        vfs_close(ifile);
        return -ENOEXEC;
    }

    for (uint64_t i = 0; i < phnum; i++) {
        if (iphdrs[i].p_type == PT_LOAD) {
            if (map_segment(pml4, ifile, &iphdrs[i], interp_base) != 0) {
                malloc_free(iphdrs);
                vfs_close(ifile);
                return -ENOEXEC;
            }
        }
    }

    *entry_out = iehdr.e_entry + interp_base;
    malloc_free(iphdrs);
    vfs_close(ifile);
    return 0;
}

int sys_execve(const char *pathname, char *const argv[], char *const envp[])
{
    if (!pathname) { errno = EFAULT; return -1; }

    vfs_file_t *f = vfs_open(pathname, VFS_O_READ);
    if (!f) { errno = ENOENT; return -1; }

    Elf64_Ehdr ehdr;
    if (vfs_read(f, &ehdr, sizeof(ehdr)) != sizeof(ehdr)) {
        vfs_close(f); errno = ENOEXEC; return -1;
    }
    if (ehdr.e_ident[0] != 0x7F || ehdr.e_ident[1] != 'E' ||
        ehdr.e_ident[2] != 'L'  || ehdr.e_ident[3] != 'F') {
        vfs_close(f); errno = ENOEXEC; return -1;
    }
    if (ehdr.e_machine != EM_X86_64) { vfs_close(f); errno = ENOEXEC; return -1; }
    if (ehdr.e_phentsize != sizeof(Elf64_Phdr)) { vfs_close(f); errno = ENOEXEC; return -1; }
    if (ehdr.e_phnum == 0) { vfs_close(f); errno = ENOEXEC; return -1; }

    void *pml4_phys = pmm_alloc_page_and_clear();
    if (!pml4_phys) { vfs_close(f); errno = ENOMEM; return -1; }

    page_table_t *new_pml4 = (page_table_t *)
        ((uint64_t)pml4_phys + get_bootinfo().hhdm->offset);

    page_table_t *old_pml4 = get_kernel_pml4();
    for (int i = 256; i < 512; i++) new_pml4->entries[i] = old_pml4->entries[i];

    Elf64_Phdr *phdrs = malloc_alloc(ehdr.e_phnum * sizeof(Elf64_Phdr));
    if (!phdrs) { vfs_close(f); errno = ENOMEM; return -1; }
    vfs_lseek(f, ehdr.e_phoff, VFS_SEEK_SET);
    if (vfs_read(f, phdrs, ehdr.e_phnum * sizeof(Elf64_Phdr)) !=
        (int)(ehdr.e_phnum * sizeof(Elf64_Phdr))) {
        malloc_free(phdrs); vfs_close(f); errno = ENOEXEC; return -1;
    }

    char     interp_path[256] = {0};
    bool     has_interp       = false;
    uint64_t phdr_vaddr       = 0;
    uint64_t entry_point      = ehdr.e_entry;
    uint64_t load_bias        = 0;
    uint64_t brk_start        = 0;

    for (int i = 0; i < ehdr.e_phnum; i++) {
        if (phdrs[i].p_type == PT_INTERP) {
            uint64_t len = phdrs[i].p_filesz;
            if (len >= sizeof(interp_path)) len = sizeof(interp_path) - 1;
            vfs_lseek(f, phdrs[i].p_offset, VFS_SEEK_SET);
            if (vfs_read(f, interp_path, len) < 0) {
                malloc_free(phdrs); vfs_close(f); errno = ENOEXEC; return -1;
            }
            interp_path[sizeof(interp_path) - 1] = 0;
            has_interp = true;
        } else if (phdrs[i].p_type == PT_PHDR) {
            phdr_vaddr = phdrs[i].p_vaddr;
        }
    }

    if (ehdr.e_type == 3) {
        load_bias = 0x4000000000ULL;
        entry_point += load_bias;
        if (phdr_vaddr) phdr_vaddr += load_bias;
    }

// for (int i = 0; i < ehdr.e_phnum; i++) {
//     console_print_string("[PHDR] i=");
//     console_print_num(i);
//     console_print_string(" type=");
//     console_print_num(phdrs[i].p_type);
//     console_print_string(" vaddr=");
//     console_print_hex(phdrs[i].p_vaddr);
//     console_print_string(" filesz=");
//     console_print_num(phdrs[i].p_filesz);
//     console_print_string(" memsz=");
//     console_print_num(phdrs[i].p_memsz);
//     console_print_string("\n");
// }

    if (phdr_vaddr == 0) {
        for (int i = 0; i < ehdr.e_phnum; i++) {
            if (phdrs[i].p_type != PT_LOAD) continue;
            if (phdrs[i].p_offset <= ehdr.e_phoff &&
                ehdr.e_phoff < phdrs[i].p_offset + phdrs[i].p_filesz) {
                phdr_vaddr = phdrs[i].p_vaddr
                           + (ehdr.e_phoff - phdrs[i].p_offset)
                           + load_bias;
                break;
            }
        }
        if (phdr_vaddr == 0) {
            for (int i = 0; i < ehdr.e_phnum; i++) {
                if (phdrs[i].p_type == PT_LOAD) {
                    phdr_vaddr = phdrs[i].p_vaddr + ehdr.e_phoff + load_bias;
                    break;
                }
            }
        }
    }

    for (int i = 0; i < ehdr.e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        if (map_segment(new_pml4, f, &phdrs[i], load_bias) != 0) {
            malloc_free(phdrs); vfs_close(f);
            errno = ENOEXEC;
            return -1;
        }
        uint64_t end = phdrs[i].p_vaddr + load_bias + phdrs[i].p_memsz;
        if (end > brk_start) brk_start = (end + 0xFFF) & ~0xFFFULL;
    }

    // uint64_t sym_addr = 0x401088;
    // uint64_t sym_phys = vmm_get_phys(new_pml4, sym_addr & ~0xFFFULL);
    // if (sym_phys) {
    //     uint64_t val = *(uint64_t *)(sym_phys
    //                     + get_bootinfo().hhdm->offset
    //                     + (sym_addr & 0xFFF));
    //     console_print_string("[EXEC] stdout=");
    //     console_print_hex(val);
    //     console_print_string(" phys_ok\n");
    // } else {
    //     console_print_string("[EXEC] stdout page not mapped!\n");
    // }

    uint64_t interp_base  = 0x40000000ULL;
    uint64_t interp_entry = 0;
    if (has_interp) {
        int rc = load_interp(new_pml4, interp_path, interp_base, &interp_entry);
        if (rc != 0) {
            malloc_free(phdrs); vfs_close(f);
            errno = -rc;
            return -1;
        }
    }

    void *stack_phys = pmm_alloc_pages(STACK_SIZE / 4096);
    if (!stack_phys) { malloc_free(phdrs); vfs_close(f); errno = ENOMEM; return -1; }

    uint64_t stack_virt = STACK_TOP - STACK_SIZE;
    for (uint64_t i = 0; i < STACK_SIZE / 4096; i++) {
        vmm_map_page(new_pml4, (uint64_t)stack_phys + i * 4096,
                     stack_virt + i * 4096,
                     PTE_PRESENT | PTE_WRITABLE | PTE_USER);
    }

    uint64_t stack_kern_top = (uint64_t)stack_phys
                            + get_bootinfo().hhdm->offset
                            + STACK_SIZE;
    uint8_t *stack_ptr = (uint8_t *)stack_kern_top;

    int argc = 0, envc = 0;
    while (argv && argv[argc]) argc++;
    while (envp && envp[envc]) envc++;

    if (argc > 255) argc = 255;
    if (envc > 255) envc = 255;

    uint64_t *str_ptrs = malloc_alloc(256 * sizeof(uint64_t));
    uint64_t *env_ptrs = malloc_alloc(256 * sizeof(uint64_t));
    if (!str_ptrs || !env_ptrs) {
        if (str_ptrs) malloc_free(str_ptrs);
        if (env_ptrs) malloc_free(env_ptrs);
        malloc_free(phdrs); vfs_close(f); errno = ENOMEM; return -1;
    }

    for (int i = 0; i < 256; i++) { str_ptrs[i] = 0; env_ptrs[i] = 0; }

    for (int i = argc - 1; i >= 0; i--) {
        size_t len = 0; while (argv[i][len]) len++;
        stack_ptr -= (len + 1);
        memcpy(stack_ptr, argv[i], len + 1);
        str_ptrs[i] = STACK_TOP - ((uint64_t)stack_kern_top - (uint64_t)stack_ptr);
    }
    for (int i = envc - 1; i >= 0; i--) {
        size_t len = 0; while (envp[i][len]) len++;
        stack_ptr -= (len + 1);
        memcpy(stack_ptr, envp[i], len + 1);
        env_ptrs[i] = STACK_TOP - ((uint64_t)stack_kern_top - (uint64_t)stack_ptr);
    }

    uint8_t random_bytes[16];
    for (int i = 0; i < 16; i++) random_bytes[i] = (uint8_t)(i * 31 + 17);
    stack_ptr -= 16;
    memcpy(stack_ptr, random_bytes, 16);
    uint64_t rand_ptr = STACK_TOP - ((uint64_t)stack_kern_top - (uint64_t)stack_ptr);

    Elf64_auxv_t auxv[] = {
        {AT_PHDR,   {phdr_vaddr}},
        {AT_PHENT,  {ehdr.e_phentsize}},
        {AT_PHNUM,  {ehdr.e_phnum}},
        {AT_PAGESZ, {4096}},
        {AT_BASE,   {has_interp ? interp_base : 0}},
        {AT_FLAGS,  {0}},
        {AT_ENTRY,  {entry_point}},
        {AT_UID,    {0}},
        {AT_EUID,   {0}},
        {AT_GID,    {0}},
        {AT_EGID,   {0}},
        {AT_SECURE, {0}},
        {AT_RANDOM, {rand_ptr}},
        {AT_EXECFN, {str_ptrs[0]}},
        {AT_NULL,   {0}}
    };
    const int auxv_count = (int)(sizeof(auxv) / sizeof(Elf64_auxv_t));

    uint64_t bytes_after_strings =
        (uint64_t)auxv_count * sizeof(Elf64_auxv_t)
        + (uint64_t)(3 + argc + envc) * 8;

    uint64_t strings_size = (uint64_t)stack_kern_top - (uint64_t)stack_ptr;
    uint64_t pad = (16 - ((strings_size + bytes_after_strings) % 16)) % 16;
    stack_ptr -= pad;
    for (uint64_t i = 0; i < pad; i++) stack_ptr[i] = 0;

    stack_ptr -= auxv_count * sizeof(Elf64_auxv_t);
    memcpy(stack_ptr, auxv, auxv_count * sizeof(Elf64_auxv_t));

    stack_ptr -= sizeof(uint64_t); *(uint64_t *)stack_ptr = 0;
    for (int i = envc - 1; i >= 0; i--) {
        stack_ptr -= sizeof(uint64_t);
        *(uint64_t *)stack_ptr = env_ptrs[i];
    }

    stack_ptr -= sizeof(uint64_t); *(uint64_t *)stack_ptr = 0;
    for (int i = argc - 1; i >= 0; i--) {
        stack_ptr -= sizeof(uint64_t);
        *(uint64_t *)stack_ptr = str_ptrs[i];
    }

    stack_ptr -= sizeof(uint64_t);
    *(uint64_t *)stack_ptr = (uint64_t)argc;

    strings_size = (uint64_t)stack_kern_top - (uint64_t)stack_ptr;
    
    uint64_t current_stack_val = STACK_TOP - strings_size;
    uint64_t aligned_stack_val = current_stack_val & ~0xFULL;
    
    stack_ptr -= (current_stack_val - aligned_stack_val);

    uint64_t final_rsp = STACK_TOP - ((uint64_t)stack_kern_top - (uint64_t)stack_ptr);
    final_rsp &= ~0xFULL; 

    process_t *p = get_current_process();
    if (!p) {
        malloc_free(str_ptrs); malloc_free(env_ptrs);
        malloc_free(phdrs); vfs_close(f); errno = ESRCH; return -1;
    }

    if (f->node) {
        if (f->node->mode & 04000 /* SUID */) {
            p->euid = f->node->uid;
        }
        if (f->node->mode & 02000 /* SGID */) {
            p->egid = f->node->gid;
        }
    }

    p->pml4        = new_pml4;
    p->brk_start   = brk_start;
    p->brk_current = brk_start;
    p->mmap_base   = 0x5000000000ULL;
    p->is_kernel_process = false;

    thread_t *thr = p->threads[0];
    if (!thr) {
        malloc_free(str_ptrs); malloc_free(env_ptrs);
        malloc_free(phdrs); vfs_close(f); errno = ESRCH; return -1;
    }

    __asm__ volatile("cli");
    thr->regs->rip    = has_interp ? interp_entry : entry_point;
    thr->regs->rsp    = final_rsp;
    thr->regs->cs     = USER_CODE | 3;
    thr->regs->ss     = USER_DATA | 3;
    thr->regs->rflags = 0x202;
    thr->fs_base      = 0;

    get_current_local_data()->kernel_rsp = thr->kernel_stack_top;
    tss_set_stack(thr->kernel_stack_top);

    vfs_close(f);
    malloc_free(phdrs);

    // console_print_string("[EXEC] e_type=");   console_print_num(ehdr.e_type);
    // console_print_string(" e_entry=");        console_print_hex(ehdr.e_entry);
    // console_print_string(" load_bias=");      console_print_hex(load_bias);
    // console_print_string("\n[EXEC] AT_PHDR=");  console_print_hex(phdr_vaddr);
    // console_print_string(" AT_ENTRY=");        console_print_hex(entry_point);
    // console_print_string(" AT_BASE=");         console_print_hex(has_interp ? interp_base : 0);
    // console_print_string("\n[EXEC] RIP=");     console_print_hex(thr->regs->rip);
    // console_print_string(" RSP=");             console_print_hex(thr->regs->rsp);
    // console_print_string(" RSP%16=");          console_print_num(thr->regs->rsp & 0xF);
    // console_print_string(" argc=");            console_print_num(argc);
    // console_print_string(" envc=");            console_print_num(envc);
    // console_print_string("\n");

    scheduler_update_pml4(p->pml4);
    vmm_switch_address_space((uint64_t)p->pml4 - get_bootinfo().hhdm->offset);

//     console_print_string("[EXEC] before-iret FS_BASE=");
//     console_print_hex(rdmsr(0xC0000100));
//     console_print_string(" STACK_TOP=");
//     console_print_hex(STACK_TOP);
//     console_print_string(" STACK_SIZE=");
//     console_print_hex(STACK_SIZE);
//     console_print_string("\n");

// console_print_string("[RSPDBG] top=");
// console_print_hex(stack_kern_top);
// console_print_string(" ptr=");
// console_print_hex((uint64_t)stack_ptr);
// console_print_string(" used=");
// console_print_hex((uint64_t)stack_kern_top - (uint64_t)stack_ptr);
// console_print_string(" STACK_TOP=");
// console_print_hex(STACK_TOP);
// console_print_string(" final=");
// console_print_hex(final_rsp);
// console_print_string("\n");

    // console_print_string("[RSPDBG] stack_kern_top=");
    // console_print_hex(stack_kern_top);
    // console_print_string(" stack_ptr=");
    // console_print_hex((uint64_t)stack_ptr);
    // console_print_string(" used=");
    // console_print_hex((uint64_t)stack_kern_top - (uint64_t)stack_ptr);
    // console_print_string(" final=");
    // console_print_hex(final_rsp);
    // console_print_string("\n");
    
    wrmsr(IA32_FS_BASE, 0);

    uint64_t user_ds = USER_DATA | 3;
    uint64_t user_cs = USER_CODE | 3;
    uint64_t target_entry = has_interp ? interp_entry : entry_point;

    if (p->pid == 0) p->pid = 1;
    if (thr->tid == 0) thr->tid = 1;

    void *fpu_ptr = thread_get_fpu_state(thr);
    if (fpu_ptr) {
        memset(fpu_ptr, 0, 512);
        *(uint16_t *)((uint8_t *)fpu_ptr + 0)  = 0x037F;
        *(uint32_t *)((uint8_t *)fpu_ptr + 24) = 0x1F80;
    }

    if (thr) {
        thr->fs_base = 0;
        wrmsr(IA32_FS_BASE, 0);
    }

    malloc_free(str_ptrs);
    malloc_free(env_ptrs);

    wrmsr(IA32_GS_BASE, 0);
    wrmsr(IA32_KERNEL_GS_BASE, (uint64_t)get_current_local_data());

    __asm__ volatile (
        "cli\n\t"
        "mov %0, %%ds\n\t"
        "mov %0, %%es\n\t"
        "pushq %0\n\t"
        "pushq %1\n\t"
        "pushq $0x202\n\t"
        "pushq %2\n\t"
        "pushq %3\n\t"
        "xor %%rax, %%rax\n\t"
        "xor %%rbx, %%rbx\n\t"
        "xor %%rcx, %%rcx\n\t"
        "xor %%rdx, %%rdx\n\t"
        "xor %%rsi, %%rsi\n\t"
        "xor %%rdi, %%rdi\n\t"
        "xor %%rbp, %%rbp\n\t"
        "xor %%r8,  %%r8\n\t"
        "xor %%r9,  %%r9\n\t"
        "xor %%r10, %%r10\n\t"
        "xor %%r11, %%r11\n\t"
        "xor %%r12, %%r12\n\t"
        "xor %%r13, %%r13\n\t"
        "xor %%r14, %%r14\n\t"
        "xor %%r15, %%r15\n\t"
        "iretq\n\t"
        :
        : "r"(user_ds), "r"(final_rsp), "r"(user_cs), "r"(target_entry)
        : "memory"
    );

    __builtin_unreachable();
}