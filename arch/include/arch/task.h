/* ================================================================
 * arch/task.h — 任务（线程）管理的架构抽象接口
 *
 * 【Lesson 5】核心新增：
 *   - struct task_struct  — 任务控制块（TCB，Task Control Block）
 *   - arch_context_switch — 上下文切换（保存 prev 的 CPU 状态，恢复 next 的）
 *   - arch_task_stack_init — 为新任务初始化内核栈
 *
 * 【Lesson 6】扩展：
 *   - struct task_struct 增加 IPC 等待相关字段（wait_kind / wait_channel /
 *     ipc_buf / ipc_out_*  / ipc_timeout_tick / ipc_result / next_waiter）
 *   - 这些字段由 kernel/ipc.c 读写，用于实现阻塞式消息传递
 *   - 任务被阻塞在 ipc_send / ipc_recv 时，状态置为 TASK_BLOCKED，
 *     加入对应 channel 的 send_waiters / recv_waiters 链表，
 *     由 waker（ipc_recv / ipc_send）调用 sched_wake 唤醒
 *
 * 这个头文件定义"任务调度"的统一接口：
 *   - struct task_struct  — 任务控制块（TCB，Task Control Block）
 *   - arch_context_switch — 上下文切换（保存 prev 的 CPU 状态，恢复 next 的）
 *   - arch_task_stack_init — 为新任务初始化内核栈
 *
 * 抽象的设计目标（与 arch/cpu.h / arch/mem.h 一致）：
 *   - "做什么" 在这里声明
 *   - "怎么做" 在 arch/x86_64/ 里实现（task.asm + task.c）
 *   - 换架构（RISC-V / ARM）时只新增 arch/<arch>/ 实现，不改这个头
 *
 * =================================================================
 *
 * 【任务（task / kernel thread）是什么】
 *
 *   任务 = 一个独立的执行流，有自己的栈、自己的寄存器快照、自己的状态。
 *   多个任务共享同一个地址空间（内核空间）、同一份内核代码。
 *   区分任务靠 task_struct，每个任务一个。
 *
 *   和"进程"的区别：
 *     - 进程通常有独立地址空间（不同页表）
 *     - 任务（kernel thread）共享内核地址空间，只是不同的执行流
 *   L5 只做内核任务调度，用户进程在后续课程实现。
 *
 * =================================================================
 *
 * 【上下文切换（context switch）原理】
 *
 *   切换 = 保存当前 CPU 状态到 prev->saved_rsp 指向的栈，
 *          然后从 next->saved_rsp 指向的栈恢复 CPU 状态。
 *
 *   "保存 CPU 状态"指：把当前通用寄存器压栈，记下当前 RSP。
 *   "恢复 CPU 状态"指：把 RSP 改成 next 的栈，弹寄存器，ret 到 next。
 *
 *   x86-64 调用约定（System V AMD64）规定：
 *     - %rbx, %rbp, %r12, %r13, %r14, %r15 是"被调用者保存"（callee-saved）
 *       函数可以自由使用 %rax, %rcx 等而不必保存，但用 %rbx 等必须先 push
 *     - 所以 arch_context_switch 只需保存这 6 个 callee-saved 寄存器 + RSP + RFLAGS
 *     - %rax 等 caller-saved 寄存器由调用方负责，不需要 switch 保存
 *
 *   RFLAGS 也必须保存，因为：
 *     - RFLAGS 含 IF 位（中断使能）
 *     - 如果切换前 IF=1，切回来时必须 IF=1（任务期望中断是开的）
 *     - 不保存 RFLAGS 会让任务"丢中断使能状态"，调度回来后中断关着
 *
 *   ┌──────────────────────────────────────────────────────────────┐
 *   │  arch_context_switch(prev, next) 的栈操作                     │
 *   ├──────────────────────────────────────────────────────────────┤
 *   │  保存阶段（在 prev 的栈上）：                                  │
 *   │     pushfq      ; 保存 RFLAGS                                 │
 *   │     push rbp/rbx/r12-r15  ; 保存 6 个 callee-saved           │
 *   │     mov [prev->saved_rsp], rsp  ; 保存当前 RSP                │
 *   │                                                              │
 *   │  切换：                                                       │
 *   │     mov rsp, [next->saved_rsp]  ; 加载 next 的 RSP            │
 *   │                                                              │
 *   │  恢复阶段（在 next 的栈上）：                                  │
 *   │     pop r15/r14/r13/r12/rbx/rbp  ; 恢复 6 个 callee-saved   │
 *   │     popfq      ; 恢复 RFLAGS                                 │
 *   │     ret        ; 弹出"返回地址"，跳到 next 上次离开的地方      │
 *   └──────────────────────────────────────────────────────────────┘
 *
 *   关键技巧：ret 会弹出栈顶作为返回地址。
 *   - 对"老任务"恢复：ret 回到 sched_yield 的调用点（即 sched_yield 返回）
 *   - 对"新任务"恢复：ret 到 task_trampoline（由 arch_task_stack_init 预先布置）
 *
 * =================================================================
 *
 * 【新任务的栈布局】
 *
 *   新任务从未运行过，没有"上次离开的地方"。
 *   arch_task_stack_init 在它的栈上伪造一个"仿佛被切走的老任务"的样子：
 *
 *   栈顶（高地址）→
 *   ┌──────────────────┐
 *   │  (alignment pad)   │   ← 16 字节对齐占位（System V ABI 要求）
 *   ├──────────────────┤
 *   │  task_trampoline   │   ← ret 会弹这里作为 RIP（任务入口）
 *   ├──────────────────┤
 *   │  RFLAGS = 0x202    │   ← popfq 弹这里（IF=1,默认标志）
 *   ├──────────────────┤
 *   │  rbp = 0           │   ← 调试器看到 0 知道栈底
 *   ├──────────────────┤
 *   │  rbx = 0           │
 *   ├──────────────────┤
 *   │  r12 = 0           │
 *   ├──────────────────┤
 *   │  r13 = 0           │
 *   ├──────────────────┤
 *   │  r14 = 0           │
 *   ├──────────────────┤
 *   │  r15 = 0           │   ← saved_rsp 指向这一项（最低地址）
 *   └──────────────────┘
 *      ↑
 *      saved_rsp
 *
 *   当 arch_context_switch(prev, new_task) 第一次切到新任务时：
 *     1. 保存 prev 状态到 prev 栈
 *     2. mov rsp, new_task->saved_rsp  ← 指向 r15 = 0 那一项
 *     3. pop r15/r14/r13/r12/rbx/rbp  ← 全部弹出 0
 *     4. popfq  ← RFLAGS = 0x202（IF=1）
 *     5. ret  ← RIP = task_trampoline，跳过去
 *
 *   task_trampoline（kernel/sched.c 定义）做：
 *     - current->entry(current->arg)
 *     - sched_exit()（entry 返回后）
 *
 * =================================================================
 *
 * 【为什么 saved_rsp 必须是 task_struct 的第一个字段】
 *
 *   汇编里 arch_context_switch 用 `mov [rdi], rsp` 保存（rdi 是 prev 指针，
 *   偏移 0 = 第一个字段 = saved_rsp）。这让汇编代码极简：不用算偏移，
 *   也不用维护 layout 同步。
 *
 *   C 结构体第一个字段保证偏移 0（C 标准），所以这个"约定"是稳定的。
 *   不要把别的字段放到 saved_rsp 之前！
 * ================================================================ */

#ifndef ARCH_TASK_H
#define ARCH_TASK_H

#include <kernel/types.h>

/* ---------------------------------------------------------------
 * 【Lesson 7】forward declaration of struct cspace
 *
 *   task_struct 里要嵌入一个 cspace 指针（任务的 cap 表），
 *   但 cspace 的完整定义在 <kernel/cap.h>。
 *   如果这里 include <kernel/cap.h>，会形成头文件循环依赖：
 *     task.h → cap.h → sched.h → task.h
 *
 *   C 标准允许用 forward declaration 声明 "struct cspace;"
 *   之后 task_struct 里就可以放 "struct cspace *cspace" 指针。
 *   指针大小和指向的类型无关（都是 8 字节），所以编译器
 *   看到 forward declaration 就够分配空间，不需要完整定义。
 *
 *   使用 cspace 字段时（读 slots 等）再 include <kernel/cap.h>。
 * --------------------------------------------------------------- */
struct cspace;

/* ---------------------------------------------------------------
 * 任务状态
 *
 * 任务生命周期：
 *
 *   UNUSED → READY → RUNNING ⇄ READY     (正常运行 + 调度切换)
 *                       │
 *                       ├─→ BLOCKED → READY  (sleep 后被唤醒)
 *                       │
 *                       └─→ TERMINATED       (执行完毕，等清理)
 *
 *   UNUSED       : task_struct 未使用（数组槽位空）
 *   READY        : 可运行，在 run_queue 里等 CPU
 *   RUNNING      : 正在 CPU 上执行（只有 current 一个）
 *   BLOCKED      : 阻塞中（sleep / 等待 IO），等条件满足后回到 READY
 *   TERMINATED   : 已结束，等待 task_reaper 清理（释放栈和 task_struct）
 *
 * 【为什么 TERMINATED 不立即清理】
 *   - 任务调用 sched_exit 时还在使用自己的栈
 *   - 不能在 sched_exit 里 free 自己的栈（会用到刚 free 的内存）
 *   - 简单做法：标 TERMINATED，让其他任务在 sched_create / sched_yield 时清理
 * --------------------------------------------------------------- */
#define TASK_UNUSED       0
#define TASK_READY        1
#define TASK_RUNNING      2
#define TASK_BLOCKED      3
#define TASK_TERMINATED   4

/* ---------------------------------------------------------------
 * 任务名最大长度（含结尾 \0）
 *
 * 16 字节够放 "task-1234" 这类短名字，
 * 也方便对齐（task_struct 里 name[16] 自然 8 字节对齐）。
 * --------------------------------------------------------------- */
#define TASK_NAME_MAX     16

/* ---------------------------------------------------------------
 * 任务内核栈大小
 *
 * 8KB = 2 个 4KB 页。够普通内核任务用（递归不深的话）。
 * 注意：这是任务的"内核栈"，不是用户栈（L5 没有用户态）。
 *
 * 【为什么 8KB 够】
 *   - 中断嵌套最多 ~5 层（IRQ0 + IRQ1 + 异常...）
 *   - 每层 ~100 字节寄存器快照 + 局部变量
 *   - 函数调用深度 ~10 层，每层 ~50 字节
 *   - 总计 ~1KB，8KB 留足冗余
 *
 * 【为什么不更小，比如 4KB】
 *   - 4KB 是一个页，但容易栈溢出（中断嵌套 + 函数调用）
 *   - Linux 早期也是 4KB/8KB，后来大部分架构用 8KB/16KB
 *   - 教学内核选 8KB 平衡安全和内存占用
 * --------------------------------------------------------------- */
#define TASK_STACK_SIZE   8192u

/* ---------------------------------------------------------------
 * struct task_struct — 任务控制块
 *
 * 【字段顺序约束】
 *   saved_rsp 必须是【第一个】字段（C 标准保证偏移 0）。
 *   arch/x86_64/task.asm 的 arch_context_switch 假定 prev/next 的
 *   偏移 0 就是 saved_rsp。改字段顺序会破坏汇编！
 *
 * 【字段说明】
 *   saved_rsp       : 上下文切换时保存的内核栈指针
 *                     新任务时由 arch_task_stack_init 设置
 *                     老任务时由 arch_context_switch 更新
 *
 *   state           : 当前状态（TASK_READY 等）
 *                     volatile 因为会被中断处理程序修改
 *
 *   task_id         : 唯一 ID（从 0 开始递增）
 *                     init task 固定 ID=0，其他任务 ID>=1
 *                     Lesson 6: IPC 用 task_id 作为发送者标识
 *
 *   name            : 人类可读名字（最长 15 字符 + \0）
 *                     调试时打印方便定位
 *
 *   entry           : 任务入口函数指针（首次调度时被 task_trampoline 调用）
 *                     init task 的 entry 为 NULL（init 用 kernel_main 当入口）
 *
 *   arg             : 传给 entry 的参数
 *
 *   stack           : 任务栈底（kmalloc 分配的虚拟地址）
 *                     init task 的 stack 为 NULL（用 entry.asm 的 boot 栈）
 *
 *   stack_size      : 栈大小（字节）
 *
 *   cpu_time_ticks  : 累计 CPU 时间（tick 数）
 *                     每 tick（10ms）+1，反映任务用 CPU 多久
 *
 *   wakeup_tick     : sleep 时的唤醒时间点（tick 数）
 *                     BLOCKED 状态下，sched_tick 检查 arch_pit_get_tick_count()
 *                     >= wakeup_tick 时把任务回到 READY
 *
 *   time_slice      : 剩余时间片（tick 数）
 *                     sched_tick 每次递减；归 0 时触发切换
 *
 *   next            : 单链表 next 指针
 *                     用于 run_queue（READY 任务链表）
 *                     一个任务同时只在一个队列里
 *
 * ─── 【Lesson 6 新增】IPC 等待相关字段 ───────────────────────
 *
 *   next_waiter     : 等待队列（channel 的 send_waiters / recv_waiters）链表
 *                     一个任务同时只能等一个 channel，所以只用一个 next 指针
 *                     不在等待队列时为 NULL
 *
 *   wait_channel    : 当前阻塞在哪个 channel（NULL 表示不是 IPC 等待）
 *                     调试用，可以在 panic 时看到"卡在哪个 channel"
 *                     用 void* 而非 struct ipc_channel* 避免头文件循环依赖
 *
 *   wait_kind       : 等待类型
 *                       0 = 不在 IPC 等待
 *                       1 = 在 ipc_send 中等接收者（IPC_WAIT_SEND）
 *                       2 = 在 ipc_recv 中等发送者（IPC_WAIT_RECV）
 *                     wake_sleeping_tasks 用这个字段判断是否 IPC 超时
 *
 *   ipc_buf         : ipc_recv 调用方提供的缓冲区指针
 *                     waker 把消息直接拷到这里（避免双缓冲）
 *                     阻塞前由 ipc_recv 设置；唤醒后 ipc_recv 读出数据
 *
 *   ipc_buf_cap     : ipc_buf 的容量（调用方传入的最大字节数）
 *                     waker 写入时不能超过这个数（否则截断）
 *
 *   ipc_out_type    : 唤醒时填入的"消息类型"（recv 路径用）
 *                     由发送方在唤醒接收者时填，唤醒后 ipc_recv 返回给调用方
 *
 *   ipc_out_sender  : 唤醒时填入的"发送者 ID"（recv 路径用）
 *                     接收方据此知道消息来自哪个任务（可做 RPC 回复）
 *
 *   ipc_out_len     : 唤醒时填入的"实际消息字节数"（recv 路径用）
 *                     0 表示空消息；> ipc_buf_cap 时被截断
 *
 *   ipc_result      : 唤醒原因（0 = 收到消息，-IPC_ERR_TIMEDOUT = 超时 等）
 *                     ipc_recv 返回前读这个字段决定返回值
 *
 *   ipc_timeout_tick: IPC 等待的超时时刻（0 = 永不超时）
 *                     wake_sleeping_tasks 检查 now >= ipc_timeout_tick 时
 *                     把任务唤醒（设 ipc_result = -IPC_ERR_TIMEDOUT）
 * --------------------------------------------------------------- */
struct task_struct {
    u64 saved_rsp;                  /* 【偏移 0，必须第一个】保存的内核栈指针 */

    volatile int state;             /* 任务状态 */
    u64 task_id;                     /* 唯一 ID */
    char name[TASK_NAME_MAX];        /* 任务名 */

    void (*entry)(void *arg);        /* 入口函数 */
    void *arg;                       /* 入口参数 */

    void *stack;                     /* 栈底（kmalloc 地址） */
    usize_t stack_size;              /* 栈大小 */

    u64 cpu_time_ticks;             /* 累计 CPU 时间 */
    u64 wakeup_tick;                 /* sleep 唤醒时间 */
    int time_slice;                 /* 剩余时间片 */

    struct task_struct *next;        /* 链表 next（run_queue 用） */

    /* ---------- Lesson 6: IPC 等待状态 ---------- */
    struct task_struct *next_waiter;  /* 等待队列 next 指针 */
    void *wait_channel;               /* 阻塞在哪个 channel（void* 避免循环依赖） */
    int   wait_kind;                  /* 0=none, 1=send, 2=recv */
    void *ipc_buf;                    /* recv 调用方的缓冲区指针 */
    u64   ipc_buf_cap;                /* recv 缓冲区容量 */
    u64   ipc_out_type;               /* 唤醒时填入的消息 type */
    u64   ipc_out_sender;             /* 唤醒时填入的 sender task_id */
    u64   ipc_out_len;                /* 唤醒时填入的消息字节数 */
    int   ipc_result;                 /* 唤醒结果（0 / -TIMEDOUT 等） */
    u64   ipc_timeout_tick;            /* IPC 超时时刻（0 = 永久等待） */

    /* ---------- Lesson 7: Capability 字段 ----------
     *
     *   cspace            : 本任务的 Capability Space 指针
     *                       cap.c 用它管理 task 持有的所有 cap
     *                       sched_create_task 时分配，task_reaper 时清理
     *
     *   ipc_recv_cap_*   : 收到的 cap 快照（cap_recv_with_cap 用）
     *
     *   cap 随消息传递的"快照"模型：
     *     - ipc_send_with_cap 把 cap snap 塞进消息
     *     - 接收方在 deliver_to_waiter / dequeue 路径上把快照
     *       拷贝到 recv_task->ipc_recv_cap_* 字段
     *     - cap_recv_with_cap 醒来后读 current->ipc_recv_cap_*
     *       安装到自己的 CSpace 一个空 slot
     *
     *   【has_cap 字段】
     *     - 1 表示本消息附带 cap，0 表示没有
     *     - 消息结构里也有 has_cap（ipc_message）
     *     - 接收路径上把 has_cap + cap 快照一起拷到 task 字段
     *
     *   【为什么用 task 字段而不是返回值】
     *     - ipc_recv_on_channel 的签名里 in_out_cap 是字节数，
     *       不好塞 cap 快照
     *     - 用 task 字段"传出"是 L6 已有的模式（ipc_out_sender 等）
     *     - 一致性 > 接口纯粹性 */
    struct cspace *cspace;             /* 本任务的 Capability Space */
    int   ipc_recv_cap_has_cap;        /* 本次 recv 是否附带 cap */
    u32   ipc_recv_cap_type;           /* 收到的 cap 快照：type */
    u32   ipc_recv_cap_rights;         /* 收到的 cap 快照：rights */
    void *ipc_recv_cap_object;         /* 收到的 cap 快照：object 指针 */
    u64   ipc_recv_cap_lineage;         /* 收到的 cap 快照：lineage */

    /* ---------- Lesson 8: 用户态（ring 3）字段 ----------
     *
     *   is_user          : 1 = 本任务在 ring 3 运行用户代码
     *                       0 = 普通 ring 0 内核任务（L1-L7 的任务）
     *
     *   kstack_top       : 本任务内核栈顶（用于 TSS.sp0）
     *                       每次 context switch 前，sched_yield 调
     *                       arch_tss_set_sp0(next->kstack_top) 更新 TSS。
     *                       init task 用 boot 栈顶（stack_top_64）。
     *
     *   user_rip         : 用户代码入口虚拟地址（arch_enter_user_mode 用）
     *   user_rsp         : 用户栈顶虚拟地址
     *   user_code_vma    : 用户代码页起始 VMA（task_reaper 清理用）
     *   user_code_pages  : 用户代码页数（task_reaper 清理用）
     *   user_stack_vma   : 用户栈页起始 VMA（= user_rsp - USER_STACK_PAGES*4K）
     *
     *   【生命周期】
     *     sched_create_user_task 设这些字段 + 映射 user 页
     *     task_trampoline → user_task_main → arch_enter_user_mode → iretq → ring 3
     *     user 跑完调 sys_exit → sched_exit → 标 TERMINATED → task_reaper
     *     task_reaper 对 is_user 任务额外 unmap+free user 页
     *
     *   【为什么 kstack_top 不和 saved_rsp 共用】
     *     saved_rsp 是"当前切走时的栈指针"（动态变化）。
     *     kstack_top 是"栈的最高地址"（固定，分配时确定）。
     *     TSS.sp0 要的是栈顶（最高地址），让中断帧从栈顶往下压。 */
    int   is_user;
    u64   kstack_top;
    u64   user_rip;
    u64   user_rsp;
    u64   user_code_vma;
    u64   user_code_pages;
    u64   user_stack_vma;

    /* ---------- Lesson 9: Crash Recovery ----------
     *
     *   【核心思想】混合/微内核的精髓：用户任务崩溃 ≠ 内核崩溃。
     *   ring-3 任务的异常（#PF, #GP, #UD 等）只杀该任务，内核继续运行。
     *   ring-0 的异常仍然 panic（内核 bug 不能恢复）。
     *
     *   exit_code       : 退出码。0=正常退出，>0=用户自定义，<0=fault 信号
     *                    正常 sys_exit(0) 时为 0；
     *                    被异常杀死时为负数（类似 Unix 信号）
     *
     *   fault_type      : 杀死本任务的异常向量号（EXC_PF, EXC_GP 等）
     *                    0 表示不是被异常杀死的（正常退出）
     *
     *   fault_rip       : 异常发生时的 RIP（出错指令地址）
     *
     *   fault_addr      : #PF 时为 CR2（缺页地址），其他异常时为 0
     *
     *   parent_task_id  : 创建者的 task_id（supervisor 通知用）
     *                    SYS_waitpid 通过它找到父任务 */
    int   exit_code;        /* 0=normal, >0=user code, <0=fault signal */
    int   fault_type;       /* Exception vector (EXC_PF etc), 0=not a fault */
    u64   fault_rip;        /* RIP at fault time */
    u64   fault_addr;       /* CR2 for #PF, 0 for others */
    u64   parent_task_id;   /* Creator's task_id (for supervisor notification) */
};

/* ---------------------------------------------------------------
 * 【Lesson 9】信号式退出码
 *
 *   当 ring-3 用户任务被异常杀死时，exit_code 设为负数，
 *   类似 Unix 信号的语义（但简化为单一 int）。
 *
 *   值的选择参考 Linux signal 编号（但取负，和正常退出码区分）：
 *     -11 = SIGSEGV（段错误：#PF / #GP 在用户态）
 *     -4  = SIGILL （非法指令：#UD 在用户态）
 *     -8  = SIGFPE （浮点异常：#DE 除零在用户态）
 *     -7  = SIGBUS （总线错误：#AC / #SS / #TS 在用户态）
 *     -6  = SIGABRT（放弃：#DF / #BP 在用户态）
 * --------------------------------------------------------------- */
#define TASK_EXIT_NORMAL  0
#define TASK_SIG_SEGV   -11   /* #PF, #GP from user → segfault */
#define TASK_SIG_ILL     -4   /* #UD, #NM from user → illegal instruction */
#define TASK_SIG_FPE     -8   /* #DE (divide error) from user */
#define TASK_SIG_BUS    -7   /* #AC, #SS, #TS from user → bus error */
#define TASK_SIG_ABORT  -6   /* #DF, #BP from user → abort */

/* ---------------------------------------------------------------
 * arch_context_switch — 切换 CPU 上下文
 *
 * 参数：
 *   prev — 当前任务（要切走）
 *   next — 下一个任务（要切到）
 *
 * 行为：
 *   1. 保存 callee-saved 寄存器 + RFLAGS 到 prev 的栈
 *   2. 保存当前 RSP 到 prev->saved_rsp
 *   3. 加载 next->saved_rsp 到 RSP
 *   4. 从 next 的栈恢复 callee-saved + RFLAGS
 *   5. ret 到 next 上次离开的地方（或新任务的 task_trampoline）
 *
 * 【重要约束】
 *   - 调用前必须关中断（arch_irq_save），防止切换过程中被中断打断
 *   - 切换是"原子"语义：要么 prev 切走，要么 next 切到，不能中间状态
 *   - 但 x86-64 单核上，关中断 + 简单寄存器操作就够原子了
 *
 * 【返回时机】
 *   - arch_context_switch 不会立即"返回到调用者"——它"返回到 next 任务"
 *   - 对 prev 来说，arch_context_switch 直到【未来某次被切回来】才"返回"
 *   - 切回来时，prev 的 RSP 被恢复，callee-saved + RFLAGS 被恢复，
 *     ret 跳回 sched_yield 的 arch_context_switch 调用点之后
 * --------------------------------------------------------------- */
void arch_context_switch(struct task_struct *prev, struct task_struct *next);

/* ---------------------------------------------------------------
 * arch_task_stack_init — 初始化新任务的内核栈
 *
 * 参数：
 *   task — 已分配好 stack 的任务（task->stack 必须非 NULL）
 *
 * 行为：
 *   1. 计算 stack_top = task->stack + task->stack_size（16 对齐）
 *   2. 在栈顶往下伪造一个"被切走的老任务"的栈布局：
 *        - alignment pad（8 字节，让 16 对齐）
 *        - task_trampoline 地址（ret 弹这里作为 RIP）
 *        - RFLAGS = 0x202（IF=1，默认 flags）
 *        - rbp/rbx/r12-r15 全 0（首次执行没历史值）
 *   3. 设置 task->saved_rsp 指向 r15 那一项（最低地址）
 *
 *   之后第一次 arch_context_switch(prev, task) 就会：
 *     pop r15..rbp（全 0）→ popfq（IF=1）→ ret（到 task_trampoline）
 *
 * 返回值：
 *   0  — 成功
 *   -1 — 失败（task 或 stack 不合法）
 * --------------------------------------------------------------- */
int arch_task_stack_init(struct task_struct *task);

#endif /* ARCH_TASK_H */
