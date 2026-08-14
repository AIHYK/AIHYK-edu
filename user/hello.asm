; ================================================================
; user/hello.asm — AIHYK 用户态程序（ring 3）
;
; 【Lesson 8】第一个用户态程序。
; 运行在 ring 3，通过 int 0x80 系统调用请求内核服务。
;
; 精简版：打印短标识 + yield 几次 + exit
; ================================================================

bits 64

%define SYS_write   1
%define SYS_yield   2
%define SYS_exit    3
%define SYS_getpid  4
%define SYS_putchar 6

global _start
_start:
    ; 短标识
    mov rax, SYS_write
    lea rdi, [rel msg]
    mov rsi, msg_len
    int 0x80

    ; yield 3 次（演示并发调度）
    mov rcx, 3
.loop:
    mov rax, SYS_yield
    int 0x80
    dec rcx
    jnz .loop

    ; 退出
    mov rax, SYS_exit
    xor rdi, rdi
    int 0x80

.hang:
    jmp .hang

msg:     db "[user] ring-3 alive", 10
msg_len  equ $ - msg
