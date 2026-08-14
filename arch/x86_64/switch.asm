; ================================================================
; task.asm — 上下文切换的汇编实现
;
; 【Lesson 5 核心新增】
;
; 这个文件只做一件事：arch_context_switch(prev, next)
;
; 它是整个任务调度器的心脏：
;   - 保存 prev 的 callee-saved 寄存器 + RFLAGS 到 prev 的栈
;   - 切换 RSP 到 next 的栈
;   - 恢复 next 的 callee-saved 寄存器 + RFLAGS
;   - ret 到 next（新任务）或 next 上次离开的地方（老任务）
;
; 全部逻辑只有 ~15 条指令，但每条都关键。
;
; 【为什么必须在汇编里】
;   - C 不能直接控制 RSP（编译器管 RSP，不能让用户改）
;   - 上下文切换的核心就是改 RSP，必须 asm
;   - C 也无法精确控制 push/pop 顺序和 ret 的行为
;
; 【为什么不用内联汇编】
;   - 内联汇编容易出 bug（约束写错编译器会"优化掉"关键指令）
;   - 单独 .asm 文件更清晰，方便 GDB 单步调试
;   - 跨架构移植时，每个架构一个 task.asm 即可
; ================================================================

; ---------------------------------------------------------------
; 外部符号（C 里定义）
; ---------------------------------------------------------------
extern task_trampoline     ; kernel/sched.c，新任务的入口跳板

; ---------------------------------------------------------------
; 段属性：可执行代码
; ---------------------------------------------------------------
section .text
bits 64

; ================================================================
; arch_context_switch — 切换 CPU 上下文
;
; C 原型（arch/task.h）：
;   void arch_context_switch(struct task_struct *prev, struct task_struct *next);
;
; System V AMD64 ABI 调用约定：
;   第 1 个参数 = rdi  (prev)
;   第 2 个参数 = rsi  (next)
;
; struct task_struct 第一个字段是 saved_rsp（u64），偏移 0。
; 所以：
;   [rdi + 0]  = prev->saved_rsp
;   [rsi + 0]  = next->saved_rsp
;
; 不需要算偏移，汇编极简。
; ================================================================
global arch_context_switch
arch_context_switch:
    ; ============================================================
    ; 第 1 阶段：保存当前 CPU 状态到 prev 的栈
    ;
    ; 进入这个函数时，栈顶（RSP 指向）是 call 指令压入的返回地址：
    ;   [RSP]    = 返回到 sched_yield 调用点的 RIP
    ;
    ; 我们要做：
    ;   1. pushfq      — 保存 RFLAGS（含 IF 位）
    ;   2. push rbp/rbx/r12-r15 — 保存 6 个 callee-saved
    ;   3. 保存当前 RSP 到 prev->saved_rsp
    ; ============================================================

    ; 保存 RFLAGS（含 IF 中断使能位）
    ; 【为什么必须保存 RFLAGS】
    ;   - 切换前可能 IF=1（voluntary yield）或 IF=0（被抢占 inside IRQ）
    ;   - 切回来时必须恢复相同的 IF 状态
    ;   - 不保存会让任务"丢失中断使能状态"，调度回来后中断可能关着
    pushfq

    ; 保存 6 个 callee-saved 寄存器
    ; 【为什么只保存这 6 个】
    ;   - System V AMD64 ABI 规定这 6 个是 callee-saved
    ;     （rbx, rbp, r12, r13, r14, r15）
    ;   - 函数可以自由使用 rax/rcx/rdx/rsi/rdi/r8-r11 而不必保存
    ;     （caller 负责保存这些）
    ;   - 任务切换时，C 代码"看起来"就是从 arch_context_switch 返回
    ;     所以只需保存 callee-saved，caller-saved 由调用方处理
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    ; 保存当前 RSP 到 prev->saved_rsp（偏移 0）
    ; 此时 RSP 指向"刚 push 完 r15"的位置
    ; 这个 RSP 就是 prev 任务"被切走时的栈状态"
    mov [rdi], rsp

    ; ============================================================
    ; 第 2 阶段：切换到 next 的栈
    ;
    ; mov rsp, [rsi] 加载 next->saved_rsp 到 RSP
    ;
    ; 从这一行开始，"栈"变成了 next 的栈。
    ; 之后所有 push/pop 都在 next 的栈上操作。
    ; ============================================================
    mov rsp, [rsi]

    ; ============================================================
    ; 第 3 阶段：从 next 的栈恢复 CPU 状态
    ;
    ; next 可能是：
    ;   - 老任务：之前 arch_context_switch 把状态压在它的栈上，
    ;            现在按相反顺序 pop 出来
    ;   - 新任务：arch_task_stack_init 在它的栈上"伪造"了同样的布局
    ;            （r15..rbp = 0, RFLAGS = 0x202, RIP = task_trampoline）
    ;
    ; pop 顺序必须和 push 顺序【严格相反】（栈是 LIFO）！
    ; ============================================================
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp

    ; 恢复 RFLAGS（恢复 IF 状态、其他标志位）
    ; 【为什么 popfq 在 ret 之前】
    ;   - ret 不影响 RFLAGS（只改 RIP）
    ;   - popfq 必须在 ret 之前，否则 ret 后 RFLAGS 还是 prev 的
    ;   - 顺序：先恢复 callee-saved → 恢复 RFLAGS → ret 到 next 的入口
    popfq

    ; ============================================================
    ; 第 4 阶段：ret 到 next
    ;
    ; ret 弹出栈顶作为 RIP 并跳转：
    ;   - 老任务：弹出"上次 call arch_context_switch 时的返回地址"
    ;            → 回到 sched_yield 的 arch_context_switch 调用点之后
    ;   - 新任务：弹出 arch_task_stack_init 预放的 task_trampoline 地址
    ;            → 跳到 task_trampoline 开始执行
    ;
    ; 这就是"伪装成函数返回"的切换技巧——
    ;   切换看似是普通函数调用 + 返回，但 RSP 已经悄悄换了。
    ; ============================================================
    ret

; 声明：栈不需要执行权限（消除链接器 warning）
section .note.GNU-stack noalloc noexec nowrite progbits
