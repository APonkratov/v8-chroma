.text
.global _ZN4core9V8LogFontC1ERK11tagLOGFONTW
.type _ZN4core9V8LogFontC1ERK11tagLOGFONTW,@function
_ZN4core9V8LogFontC1ERK11tagLOGFONTW:
    movups 0x60(%rsi), %xmm0
    movups %xmm0, 0x60(%rdi)
    movups 0x50(%rsi), %xmm0
    movups %xmm0, 0x50(%rdi)
    movups 0x40(%rsi), %xmm0
    movups %xmm0, 0x40(%rdi)
    movups 0x30(%rsi), %xmm0
    movups %xmm0, 0x30(%rdi)
    movups 0x20(%rsi), %xmm0
    movups %xmm0, 0x20(%rdi)
    movups 0x10(%rsi), %xmm0
    movups %xmm0, 0x10(%rdi)
    movups (%rsi), %xmm0
    movups %xmm0, (%rdi)
    ret
.section .note.GNU-stack,"",@progbits
.text
.global _ZN4core4FontC1Eil
.type _ZN4core4FontC1Eil,@function
_ZN4core4FontC1Eil:
    push %rbx
    push %r14
    mov %rdi, %rbx
    mov %rdx, %r14
    mov %esi, (%rbx)
    mov %r14, 0x80(%rbx)
    movq $0, 0xa0(%rbx)
    pop %r14
    pop %rbx
    ret
    .fill 128,1,0xcc
