bits 64

extern syscall_handler
extern syscall_save_frame

global syscall_entry_stub
syscall_entry_stub:
    swapgs                    ; Переключаемся на GS ядра

    mov [gs:0x18], rsp        ; Сохраняем RSP пользователя в cpu_local.user_rsp (Offset 0x18)
    mov rsp, [gs:0x10]        ; Загружаем стек ядра из cpu_local.kernel_rsp (Offset 0x10)

    ; Формируем кадр прерывания (interrupt_frame) на стеке ядра:
    push qword 0x1B           ; SS (User Data Selector | 3)
    push qword [gs:0x18]      ; RSP (User RSP)
    push r11                  ; RFLAGS (сохранённые процессором)
    push qword 0x23           ; CS (User Code Selector | 3)
    push rcx                  ; RIP (сохранённый процессором адрес возврата)

    push qword 0              ; err_code
    push qword 0              ; int_num

    ; Сохраняем все регистры общего назначения (GPRs):
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

    cld

    ; Сохраняем кадр для планировщика задач:
    mov rdi, rsp
    call syscall_save_frame

    ; Подготовка аргументов для syscall_handler(num, a1, a2, a3, a4, a5, a6):
    mov rdi, [rsp + 0]        ; num (rax)
    mov rsi, [rsp + 40]       ; a1  (rdi)
    mov rdx, [rsp + 32]       ; a2  (rsi)
    mov rcx, [rsp + 24]       ; a3  (rdx)
    mov r8,  [rsp + 72]       ; a4  (r10)
    mov r9,  [rsp + 56]       ; a5  (r8)
    push qword [rsp + 64]     ; a6  (r9) на стек

    call syscall_handler
    add rsp, 8                ; Очищаем 6-й аргумент со стека

    ; Записываем возвращаемое значение (RAX) обратно в кадр на стеке:
    mov [rsp + 0], rax

    ; Восстанавливаем регистры общего назначения:
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

    add rsp, 16               ; Пропускаем err_code и int_num

    ; КРИТИЧЕСКАЯ СЕКЦИЯ: Запрещаем прерывания перед сменой стека и GS!
    cli

    pop rcx                   ; RIP пользователя -> в RCX (для SYSRET)
    add rsp, 8                ; Пропускаем CS (0x23)
    pop r11                   ; RFLAGS пользователя -> в R11 (для SYSRET)
    pop rsp                   ; RSP пользователя -> в RSP

    ; Устанавливаем пользовательские сегменты DS и ES:
    mov r10, 0x1B
    mov ds, r10w
    mov es, r10w

    swapgs                    ; Возвращаем User GS
    o64 sysret                  ; Безопасный возврат в Ring 3 (автоматически включит IF)