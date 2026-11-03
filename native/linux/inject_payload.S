.intel_syntax noprefix
.text
.global v8_remote_begin
.global v8_remote_end
.type v8_remote_begin,@function
v8_remote_begin:
    push rbx
    mov rbx, rdi
    lea rdi, [rbx+64]
    mov esi, 2
    call QWORD PTR [rbx]
    mov [rbx+40], rax
    test rax, rax
    jz .Lload_failed
    mov rdi, rax
    lea rsi, [rbx+8256]
    call QWORD PTR [rbx+8]
    test rax, rax
    jz .Lsymbol_failed
    lea rdi, [rbx+4160]
    call rax
    mov [rbx+16], rax
    jmp .Ldone
.Lload_failed:
    mov QWORD PTR [rbx+16], 1
    jmp .Ldone
.Lsymbol_failed:
    mov QWORD PTR [rbx+16], 2
.Ldone:
    mov QWORD PTR [rbx+24], 1
    xor eax, eax
    pop rbx
    ret
v8_remote_end:
.size v8_remote_begin, v8_remote_end-v8_remote_begin
.section .note.GNU-stack,"",@progbits
