; ================================================================
; idt.asm — 中断入口 stub + 上下文保存/恢复 + lidt 封装
;
; 【Lesson 3 核心新增】
;
; 这个文件包含四大部分：
;   1. 256 个中断入口 stub（isr0 ~ isr255），每个 stub push 自己的向量号
;   2. 通用中断处理流程（isr_common）：保存寄存器 → 调 C → 恢复
;   3. isr_table[] 数组：256 个 stub 的地址表，给 idt.c 循环填 IDT 用
;   4. lidt 指令封装（arch_idt_load）
;
; 中断处理的完整流程（x86-64 长模式）：
;
;   设备 IRQ → PIC → CPU 收到向量 N
;
;   CPU 自动做（不需要我们写代码）：
;     ┌─────────────────────────────────────────────────┐
;     │ if 特权级变化:                                   │
;     │   push SS     (中断前的栈段)                      │
;     │   push RSP    (中断前的栈指针)                    │
;     │ push RFLAGS                                        │
;     │ push CS      (中断前的代码段)                     │
;     │ push RIP     (中断前的指令指针)                   │
;     │ if 中断门: clear EFLAGS.IF (关中断)              │
;     │ if 异常且有错误码: push 错误码                    │
;     └─────────────────────────────────────────────────┘
;
;   CPU 从 IDT[N] 取出 handler 地址 → 跳过去（即 isrN）
;
;   我们的 ISR stub 做：
;     ┌─────────────────────────────────────────────────┐
;     │ if 异常无错误码: push 0  (占位，统一栈布局)      │
;     │ push N  (中断号，让 C handler 知道是哪个中断)    │
;     │ push 所有通用寄存器 (15个: rax~r15)              │
;     │ mov rdi, rsp  (frame 指针作为第1个参数)          │
;     │ call arch_irq_dispatch  (C 分发函数)             │
;     │ pop 所有通用寄存器                                │
;     │ add rsp, 16   (清理 push 的中断号+错误码)        │
;     │ iretq  (中断返回)                                │
;     └─────────────────────────────────────────────────┘
;
; 【关键设计：统一栈布局】
;
;   CPU 异常有"有错误码"和"无错误码"两种：
;     #PF (向量 14): 有错误码（CPU 自动压）
;     #DE (向量 0):  无错误码（CPU 不压）
;
;   如果不统一，stub 进 C handler 时栈布局不一致，C 代码没法处理。
;   解决：让"无错误码"的 stub 自己 push 0 占位，
;         所有 stub 进 C handler 时栈都是 [错误码][中断号][通用寄存器...]。
;
; 【为什么 push 顺序是 rax→rbx→...→r15（正序）】
;
;   x86 的 push 是"先 push rax → 后 push rbx → ... → 最后 push r15"
;   栈从高地址往低地址增长，所以栈上布局是：
;     [低地址 rsp] r15, r14, ..., rbx, rax [高地址]
;   即栈顶（低地址）是 r15，往上找 rax。
;
;   C 的 struct interrupt_frame 字段顺序：
;     r15, r14, ..., rax（从低地址到高地址）
;   这正好匹配栈布局，C 代码可以 frame->rax 直接访问到 RAX。
;
; 【为什么用 %rep 生成 256 个 stub】
;
;   每个 stub 几乎一样（只差中断号），
;   手写 256 个就是重复劳动，且容易写错。
;   用 nasm 的 %rep + %assign 生成，简洁可靠。
; ================================================================

; ---------------------------------------------------------------
; 外部符号声明
; ---------------------------------------------------------------
extern arch_irq_dispatch

; ---------------------------------------------------------------
; 段属性：可执行代码
; ---------------------------------------------------------------
section .text
bits 64

; ================================================================
; 宏定义
; ================================================================

; ---------------------------------------------------------------
; ISR_NOERROR N - 无错误码的中断入口
;
; 生成的代码：
;   isrN:
;     push 0        ; 错误码占位（CPU 没压，stub 自己压 0）
;     push N        ; 中断向量号
;     jmp isr_common
;
; 【为什么 push 0 而不是直接 push N】
;   - 第一个 push 0 是【错误码】占位（让有/无错误码的 stub 栈布局一致）
;   - 第二个 push N 是【中断号】（让 C handler 知道是哪个中断）
; ---------------------------------------------------------------
%macro ISR_NOERROR 1
isr%1:
    push 0              ; 错误码占位
    push %1             ; 中断向量号
    jmp isr_common
%endmacro

; ---------------------------------------------------------------
; ISR_ERROR N - 有错误码的中断入口
;
; 生成的代码：
;   isrN:
;     push N        ; 中断号（错误码 CPU 已压，不再压 0）
;     jmp isr_common
;
; 【哪些异常有错误码】
;   8  (#DF Double Fault): 错误码恒为 0
;   10 (#TS Invalid TSS): 错误码含选择子
;   11 (#NP Segment Not Present): 错误码含选择子
;   12 (#SS Stack Fault): 错误码含选择子
;   13 (#GP General Protection): 错误码含选择子或 0
;   14 (#PF Page Fault): 错误码含缺页信息（bit0=P, bit1=W, bit2=U, bit4=I）
;   17 (#AC Alignment Check): 错误码恒为 0
;   21 (#CP Control Protection): 错误码含控制保护类型
; ---------------------------------------------------------------
%macro ISR_ERROR 1
isr%1:
    push %1             ; 中断向量号（错误码 CPU 已压）
    jmp isr_common
%endmacro

; ================================================================
; 生成 256 个中断入口 stub
;
; 用 %rep + %assign 生成 0~255 全部 stub。
; 对"有错误码"的异常（8,10,11,12,13,14,17,21）用 ISR_ERROR，
; 其余用 ISR_NOERROR。
; ================================================================
%assign i 0
%rep 256
    %if i == 8 || i == 10 || i == 11 || i == 12 || i == 13 || i == 14 || i == 17 || i == 21
        ISR_ERROR i
    %else
        ISR_NOERROR i
    %endif
%assign i i+1
%endrep

; ================================================================
; isr_common - 通用中断处理流程
;
; 进入这里时栈布局（从低到高）：
;   [rsp+0]    中断号     ← stub push 的
;   [rsp+8]    错误码     ← stub push 的（占位 0 或 CPU 压的真实错误码）
;   [rsp+16]   RIP        ← CPU 压的
;   [rsp+24]   CS         ← CPU 压的
;   [rsp+32]   RFLAGS     ← CPU 压的
;   [rsp+40]   RSP        ← CPU 压的（仅特权级变化时）
;   [rsp+48]   SS         ← CPU 压的
;
; 这里要：
;   1. 保存所有通用寄存器（让 C handler 不破坏调用者状态）
;   2. 对齐栈到 16 字节（System V AMD64 ABI 要求）
;   3. 把 rsp 作为 frame 指针传给 arch_irq_dispatch
;   4. 调 C handler
;   5. 恢复栈指针（C 函数可能修改了 rsp）
;   6. 恢复寄存器
;   7. 清理栈上的中断号+错误码
;   8. iretq 中断返回
; ================================================================
isr_common:
    ; -----------------------------------------------------------
    ; 保存所有通用寄存器
    ;
    ; 【push 顺序】
    ;   push rax → rbx → rcx → rdx → rsi → rdi → rbp → r8 ~ r15
    ;   栈从高地址向低地址增长，所以 push rax 后栈顶是 rax
    ;   再 push rbx 后栈顶是 rbx（rax 在上面）
    ;
    ; 【最终栈布局】（从低地址 rsp 起向上）：
    ;   r15  ← rsp（栈顶）
    ;   r14
    ;   r13
    ;   ...
    ;   rax  ← 高地址
    ;   int_no    (stub push 的)
    ;   err_code  (stub push 的)
    ;   RIP (CPU 压的)
    ;   CS
    ;   RFLAGS
    ;   RSP
    ;   SS
    ;
    ; 这正好匹配 struct interrupt_frame 字段顺序！
    ; C 代码用 frame->rax / frame->rip 等直接访问。
    ; -----------------------------------------------------------
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; -----------------------------------------------------------
    ; 把当前栈指针作为 frame 参数传给 C 函数
    ;
    ; System V AMD64 ABI:
    ;   第 1 个参数 = rdi
    ;   rdi = rsp → C 函数 frame 参数就是栈上数据
    ;
    ; 【为什么是 mov rdi, rsp】
    ;   rsp 此时指向 r15（栈顶），C struct interrupt_frame 的第一个字段就是 r15。
    ;   所以 frame = rsp，C 代码用 frame->r15 读到的就是中断时的 R15 值。
    ; -----------------------------------------------------------
    mov rdi, rsp

    ; -----------------------------------------------------------
    ; 对齐栈到 16 字节
    ;
    ; System V AMD64 ABI 要求 call 指令之前 rsp 必须 16 字节对齐。
    ; 进入 isr_common 时栈被多次 push，可能不对齐。
    ;
    ; 【为什么需要保存 rsp】
    ;   C 函数可能用自己的栈，回来时 rsp 可能改变了。
    ;   保存到 rbp（callee-saved 寄存器，C 不会破坏），
    ;   调用后用 mov rsp, rbp 恢复。
    ;
    ; 【为什么用 and rsp, -16】
    ;   -16 = 0xFFFFFFFFFFFFFFF0（二进制 ...11110000）
    ;   and 后 rsp 末 4 位清 0 → 16 字节对齐
    ;   这是"向下对齐"（不会增加 rsp，只减或不变）
    ; -----------------------------------------------------------
    mov rbp, rsp            ; 保存原始 rsp 到 rbp
    and rsp, -16            ; 栈对齐到 16 字节

    ; -----------------------------------------------------------
    ; 调用 C 分发函数
    ;
    ; arch_irq_dispatch(frame):
    ;   - 检查 frame->int_no：
    ;     - 0~31  → CPU 异常 → 调 arch_exception_handler
    ;     - 32~47 → IRQ → 查 irq_table → 调 callback + EOI
    ;     - 其他  → 未知中断，警告 + EOI 保险
    ;
    ; rdi 已经设好（frame 指针），符合调用约定
    ; -----------------------------------------------------------
    call arch_irq_dispatch

    ; -----------------------------------------------------------
    ; 恢复栈指针（C 函数可能修改了 rsp）
    ; -----------------------------------------------------------
    mov rsp, rbp

    ; -----------------------------------------------------------
    ; 恢复所有通用寄存器
    ;
    ; 【pop 顺序】
    ;   必须和 push 顺序【反序】！
    ;   push 顺序：rax, rbx, ..., r15
    ;   pop 顺序：r15, r14, ..., rax
    ; -----------------------------------------------------------
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ; -----------------------------------------------------------
    ; 清理栈上的中断号+错误码
    ;
    ; 这两个是我们 stub 自己 push 的，CPU 不会自动 pop。
    ; 用 add rsp, 16 把它们丢弃（不需要恢复，只是占位用）。
    ; -----------------------------------------------------------
    add rsp, 16

    ; -----------------------------------------------------------
    ; 中断返回
    ;
    ; iretq（64 位版本）从栈依次弹出：
    ;   RIP, CS, RFLAGS, RSP, SS
    ; 恢复中断前的执行状态。
    ;
    ; 注意：iretq 弹 RFLAGS 会恢复 IF 位
    ;   - 如果中断前 IF=1，iretq 后自动开中断
    ;   - 如果中断前 IF=0，iretq 后仍关中断
    ;   这正是我们想要的（恢复中断前状态）。
    ; -----------------------------------------------------------
    iretq

; ================================================================
; isr_table - 256 项 ISR 地址表（给 idt.c 用）
;
; 用 %rep 生成 dq isr0, dq isr1, ..., dq isr255
; 每项 8 字节（64 位地址）。
;
; 【为什么用 %rep + macro】
;   nasm 的 %rep 里不能直接写 `dq isr%assign`，
;   必须用 macro 包装，让 macro 参数替换。
; ================================================================
%macro ISR_DQ 1
    dq isr%1
%endmacro

section .rodata
global isr_table
isr_table:
%assign i 0
%rep 256
    ISR_DQ i
%assign i i+1
%endrep

; ================================================================
; arch_idt_load - 执行 lidt 指令加载 IDTR
;
; C 函数原型（idt.h）:
;   void arch_idt_load(struct idtr *idtr);
;
; 实现：
;   lidt [rdi]   ; rdi 是第1个参数（idtr 指针）
;   ret
;
; 【为什么不能在 C 里直接写 lidt】
;   C 没有 lidt 指令的内建函数（不像 cli/sti 有 __asm__ 内联汇编），
;   用内联汇编 __asm__ volatile("lidt %0"::"m"(*idtr)) 也可以，
;   但放汇编里更清晰，且方便用 GDB 单步调试。
; ================================================================
global arch_idt_load
arch_idt_load:
    ; rdi = idtr 指针（第 1 个参数，System V AMD64 ABI）
    ; lidt [rdi] 从内存读 2+8 字节：limit + base
    lidt [rdi]
    ret

; 声明：栈不需要执行权限（消除链接器 warning）
section .note.GNU-stack noalloc noexec nowrite progbits
