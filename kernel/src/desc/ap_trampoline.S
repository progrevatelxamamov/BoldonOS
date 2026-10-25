.section .ap_trampoline, "ax", @progbits
.global ap_trampoline_start
.global ap_trampoline_end

#define AP_BASE        0x7000
#define AP_PARAM_CR4   0x7FD8
#define AP_PARAM_CR3   0x7FE0
#define AP_PARAM_RSP   0x7FE8
#define AP_PARAM_ENTRY 0x7FF0
#define AP_PARAM_ARG   0x7FF8

.code16
ap_trampoline_start:
    cli
    cld
    xorw    %ax, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movw    %ax, %fs
    movw    %ax, %gs

    /* Загружаем GDT32 */
    lgdtl   (gdt32_ptr - ap_trampoline_start + AP_BASE)

    /* Включаем Protected Mode */
    movl    %cr0, %eax
    orl     $1, %eax
    movl    %eax, %cr0

    /* Far jump в 32-битный код */
    ljmpl   $0x08, $(prot_mode_entry - ap_trampoline_start + AP_BASE)

/* ============================================================= */
.code32
prot_mode_entry:
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movw    %ax, %fs
    movw    %ax, %gs

    /*
     * Загружаем CR4 из параметров BSP (содержит PAE, PGE и др.).
     * Гарантируем, что PAE (бит 5) включен.
     */
    movl    (AP_PARAM_CR4), %eax
    orl     $0x20, %eax
    movl    %eax, %cr4

    /* Загружаем CR3 */
    movl    (AP_PARAM_CR3), %eax
    movl    %eax, %cr3

    /*
     * EFER MSR (0xC0000080):
     *   Бит 8  = LME (Long Mode Enable)    = 0x100
     *   Бит 11 = NXE (No-Execute Enable)   = 0x800
     * Включаем ОБА бита (0x900)! Без NXE бит 63 в PTE вызовет #PF e=0008.
     */
    movl    $0xC0000080, %ecx
    rdmsr
    orl     $0x900, %eax
    wrmsr

    /* Включаем Paging (CR0.PG = 1) -> переход в long mode */
    movl    %cr0, %eax
    btsl    $31, %eax
    movl    %eax, %cr0

    /* Загружаем GDT64 */
    lgdtl   (gdt64_ptr - ap_trampoline_start + AP_BASE)

    /* Прыгаем в 64-битный код */
    ljmpl   $0x08, $(long_mode_entry - ap_trampoline_start + AP_BASE)

/* ============================================================= */
.code64
long_mode_entry:
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movw    %ax, %fs
    movw    %ax, %gs

    /* Загружаем стек */
    movabsq $AP_PARAM_RSP, %rbx
    movq    (%rbx), %rsp

    /* Выравниваем стек по 16 байт */
    andq    $-16, %rsp

    /* Аргумент (logical_id) в RDI */
    movabsq $AP_PARAM_ARG, %rbx
    movq    (%rbx), %rdi

    /* Прыгаем в C-функцию ap_entry по её виртуальному адресу */
    movabsq $AP_PARAM_ENTRY, %rbx
    movq    (%rbx), %rax
    jmpq    *%rax

/* ============================================================= */
.align 16
gdt32_start:
    .quad 0x0000000000000000
    .quad 0x00CF9A000000FFFF      /* 0x08: 32-bit code */
    .quad 0x00CF92000000FFFF      /* 0x10: 32-bit data */
gdt32_end:

gdt32_ptr:
    .word gdt32_end - gdt32_start - 1
    .long gdt32_start - ap_trampoline_start + AP_BASE

.align 16
gdt64_start:
    .quad 0x0000000000000000
    .quad 0x00209A0000000000      /* 0x08: 64-bit code */
    .quad 0x0000920000000000      /* 0x10: 64-bit data */
gdt64_end:

gdt64_ptr:
    .word gdt64_end - gdt64_start - 1
    .quad gdt64_start - ap_trampoline_start + AP_BASE

.align 16
ap_trampoline_end: