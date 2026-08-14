; ================================================================
; user/crash.asm — 故意崩溃的用户程序（ring 3）
;
; 【Lesson 9】演示 fault isolation：
; ring 3 触发 #PF → 内核杀任务，系统继续运行。
;
; 精简版：直接触发 NULL 解引用
; ================================================================

bits 64

%define SYS_yield 2

global _start
_start:
    ; yield 一次让调度器稳定
    mov rax, SYS_yield
    int 0x80

    ; 触发崩溃：读地址 0 → #PF → 内核杀本任务
    xor rbx, rbx
    mov rax, [rbx]

    ; 永远到不了这里
.hang:
    jmp .hang
