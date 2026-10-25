.section .text
.global smp_ap_entry
.extern smp_ap_main

smp_ap_entry:
    cli
    cld
    movq 24(%rdi), %rsp
    andq $-16, %rsp
    xorq %rbp, %rbp
    call smp_ap_main
1:
    cli
    hlt
    jmp 1b
