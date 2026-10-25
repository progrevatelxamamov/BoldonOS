bits 64

extern exception_handler

%macro isr_no_err_stub 1
global isr_stub_%+%1
isr_stub_%+%1:
    push qword 0
    push qword %1
    jmp isr_common_stub
%endmacro

%macro isr_err_stub 1
global isr_stub_%+%1
isr_stub_%+%1:
    push qword %1
    jmp isr_common_stub
%endmacro

isr_common_stub:
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rbp
    push rdi
    push rsi
    push rdx
    push rcx
    push rbx
    push rax

    test qword [rsp + 144], 3
    jz .from_kernel
    swapgs
.from_kernel:

    mov rdi, rsp
    call exception_handler

    test qword [rsp + 144], 3
    jz .to_kernel
    swapgs
.to_kernel:

    pop rax
    pop rbx
    pop rcx
    pop rdx
    pop rsi
    pop rdi
    pop rbp
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15

    add rsp, 16
    iretq

%assign i 0
%rep 256
    %if i == 8 || i == 10 || i == 11 || i == 12 || i == 13 || i == 14 || i == 17 || i == 21 || i == 29 || i == 30
        isr_err_stub i
    %else
        isr_no_err_stub i
    %endif
%assign i i+1
%endrep

global isr_stub_table
isr_stub_table:
%assign i 0
%rep 256
    dq isr_stub_%+i
%assign i i+1
%endrep