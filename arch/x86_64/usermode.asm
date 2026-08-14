; ================================================================
; arch/x86_64/user.asm — 用户态（ring 3）相关的汇编实现
;
; 【Lesson 8 核心新增】
;
; 这个文件实现两个不能在 C 里写的函数：
;   1. arch_enter_user_mode — ring 0 → ring 3 的 iretq 跳转
;   2. arch_tss_load        — ltr 指令加载 TR 寄存器
;
; 【为什么不能在 C 里写】
;   - arch_enter_user_mode 要精确控制栈布局（IRETQ 帧）+ iretq 指令，
;     编译器管 RSP，C 无法保证栈上 5 个 qword 的精确顺序
;   - ltr 指令没有内建函数（C 不能直接生成 ltr）
;
; 【arch_enter_user_mode 的 IRETQ 栈帧】
;
;   我们在当前（内核）栈上构造 5 个 qword 的 IRETQ 帧，然后 iretq：
;
;     [rsp+0]  RIP    = user 入口
;     [rsp+8]  CS     = 0x23 (user code | RPL3)
;     [rsp+16] RFLAGS = 0x202 (IF=1)
;     [rsp+24] RSP    = user 栈顶
;     [rsp+32] SS     = 0x2B (user data | RPL3)
;
;   iretq 执行时：
;     1. pop RIP ← user 入口
;     2. pop CS  ← 0x23（RPL=3 ≠ 当前 CPL 0）→ 触发特权级切换
;     3. pop RFLAGS ← 0x202（IF=1，user 能被中断抢占）
;     4. 因为特权级变化，再 pop RSP ← user 栈顶
;     5. pop SS ← 0x2B
;     6. CPU 现在在 ring 3，RIP=user 入口，RSP=user 栈顶
; ================================================================

; ---------------------------------------------------------------
; 外部符号
; ---------------------------------------------------------------
extern tss               ; entry.asm 定义的 TSS 结构（104 字节，.bss）

section .text
bits 64

; ================================================================
; arch_enter_user_mode(u64 user_rip, u64 user_rsp, u64 user_arg)
;
;   System V AMD64 ABI:
;     rdi = user_rip
;     rsi = user_rsp
;     rdx = user_arg（要传给 user 程序，作为它的 rdi）
;
;   行为：构造 IRETQ 帧，iretq 到 ring 3，永不返回。
; ================================================================
global arch_enter_user_mode
arch_enter_user_mode:
    ; 保存调用方 rbp（虽然永不返回，但保持 ABI 一致性，避免奇怪问题）
    push rbp
    mov rbp, rsp

    ; 先加载 DS / ES 为 user 数据段（SS 会被 iretq 覆盖，这里设 DS/ES）
    ;   user 数据段选择子 0x2B（user data | RPL3）
    mov ax, 0x2B
    mov ds, ax
    mov es, ax
    ; FS / GS 在 64 位下用于 TLS / per-CPU，这里不动（内核设过的留着）

    ; 构造 IRETQ 帧（5 个 qword，从高地址往低地址 push，所以顺序：SS, RSP, RFLAGS, CS, RIP）
    ;   注意：push 顺序与弹出的关系——后 push 的在低地址 = 先 pop。
    ;   iretq 弹出顺序：RIP, CS, RFLAGS, RSP, SS
    ;   所以 push 顺序（逆序）：SS, RSP, RFLAGS, CS, RIP

    mov rax, 0x2B              ; SS = user data | RPL3
    push rax                  ; [栈] SS

    push rsi                  ; [栈] RSP = user 栈顶

    mov rax, 0x202            ; RFLAGS = bit1(reserved1) | bit9(IF=1)
    push rax                  ; [栈] RFLAGS

    mov rax, 0x23             ; CS = user code | RPL3
    push rax                  ; [栈] CS

    push rdi                  ; [栈] RIP = user 入口

    ; 把 user_arg（rdx）放到 rdi，让 user 程序第一个参数拿到它
    mov rdi, rdx

    ; 此时栈布局（低→高）：RIP, CS, RFLAGS, RSP, SS, [旧 rbp], [返回地址]
    ; iretq 会弹 5 个 qword，跳到 ring 3 执行 user 代码
    ; rdi 已设为 user_arg，user 程序读 rdi 拿到参数
    iretq

    ; 永远到不了这里
.hang:
    cli
    hlt
    jmp .hang


; ================================================================
; arch_tss_load(u16 selector) — ltr 指令加载 TR
;
;   C 原型: void arch_tss_load(u16 selector);
;   rdi = selector（如 0x30）
;
;   ltr 把 selector 加载到 TR，CPU 从 GDT 读 TSS 描述符缓存到 TR。
;   之后任何 ring 3 → ring 0 切换都从 TSS.sp0 取栈。
; ================================================================
global arch_tss_load
arch_tss_load:
    ltr di
    ret


; 声明：栈不需要执行权限（消除链接器 warning）
section .note.GNU-stack noalloc noexec nowrite progbits
