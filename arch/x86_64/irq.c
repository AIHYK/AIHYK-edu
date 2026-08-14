/* ================================================================
 * arch/x86_64/irq.c — IRQ 处理回调注册表 + 中断分发
 *
 * 【Lesson 3 核心新增】
 *
 * 这个文件是中断框架的"大脑"：
 *   - idt.asm 的 isr_common 调用 arch_irq_dispatch(frame)
 *   - dispatch 根据 frame->int_no 决定调谁：
 *     - 0~31  → arch_exception_handler (CPU 异常)
 *     - 32~47 → 查 irq_table[irq] → 调对应 driver callback
 *     - 其他  → 警告（未知中断，不处理）
 *
 * IRQ 处理分两层：
 *   低层：汇编 stub 保存寄存器 → 调 C 分发函数 arch_irq_dispatch
 *         → stub 恢复寄存器 → iretq
 *   高层：arch_irq_dispatch 查 IRQ 注册表，调对应 callback
 *
 * 这层抽象让：
 *   - 键盘驱动、磁盘驱动、网卡驱动可以独立注册自己的 IRQ callback
 *   - 主流程不关心具体 IRQ 内容
 *   - 添加新驱动只需 register + init，不改中断框架
 *
 * 注册表设计：
 *   - 固定 16 项数组（IRQ0~15）
 *   - 每项是一个函数指针，NULL 表示未注册
 *   - 注册时填入，注销时清 NULL
 *
 * 中断处理的"完整链"（以键盘为例）：
 *   1. 用户按键 → 键盘控制器发 IRQ1
 *   2. 8259 PIC 收到 IRQ1 → 向 CPU 发向量 33
 *   3. CPU 查 IDT[33] → 跳到 isr33 stub
 *   4. stub push 0（错误码占位）+ push 33（向量号）
 *   5. stub push 通用寄存器 → call arch_irq_dispatch
 *   6. arch_irq_dispatch 查 irq_table[1] → 调 keyboard_handler
 *   7. keyboard_handler 读 0x60 端口拿 scancode → 解码 → 显示
 *   8. keyboard_handler 返回
 *   9. arch_pic_eoi(33)（告诉 PIC 处理完了）
 *   10. stub pop 通用寄存器 → iretq → 返回被中断处
 * ================================================================ */

#include <arch/console.h>
#include <arch/interrupts.h>
#include <arch/pic.h>
#include <arch/pit.h>
#include <arch/irq.h>
#include <kernel/panic.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>
#include <kernel/types.h>
#include <kernel/util.h>

/* ---------------------------------------------------------------
 * 内部函数声明（exceptions.c 里实现）
 * --------------------------------------------------------------- */
void arch_exception_handler(struct interrupt_frame *frame);

/* ---------------------------------------------------------------
 * IRQ 回调注册表
 *
 *   irq_table[0]  → IRQ0 callback（PIT 定时器，由 pit 默认 handler 处理）
 *   irq_table[1]  → IRQ1 callback（键盘，由 keyboard 驱动注册）
 *   irq_table[2]  → IRQ2 callback（从片级联，不应该用，永远 NULL）
 *   ...
 *   irq_table[15] → IRQ15 callback（IDE 从）
 *
 * 每项是一个函数指针，NULL 表示未注册（dispatch 时直接跳过）。
 *
 * 【为什么不放在 .bss】
 *   放 .bss 默认 0（NULL 指针），arch_irq_init 还会显式清零，
 *   双重保险（防止 bootloader 没清 .bss 的情况）。
 * --------------------------------------------------------------- */
static interrupt_handler_t irq_table[16];

/* 【C8 修复】原 irq.c 本地 print_dec 已删除，统一用 <kernel/util.h> 的 kprint_dec。 */

/* ---------------------------------------------------------------
 * pit_irq_handler - 默认 PIT 定时器处理（IRQ0）
 *
 * 【Lesson 3】每 100 次 tick（1 秒）打印一次心跳。
 * 【Lesson 5】每次 tick 调用 sched_tick 驱动任务调度。
 *
 * 每次定时器中断：
 *   1. 增加 tick 计数
 *   2. 调用 sched_tick（递减当前任务时间片，可能触发切换）
 *   3. 每 100 次 tick 打印一次心跳（观察调度器活着）
 *
 * 【为什么 sched_tick 在心跳打印之前】
 *   sched_tick 可能触发上下文切换，切换走后 prev 任务"挂起"。
 *   等切回来时才执行后续代码。
 *   把心跳放在 sched_tick 之后，会让 prev 任务切回来时打印（
 *   而非切换时的"当前任务"），逻辑稍乱。
 *   放在 sched_tick 之前，心跳明确属于"切换前的当前任务"。
 *
 * 【切换后这里会发生什么】
 *   假设 IRQ0 触发时 current = A，sched_tick 把 A 切换到 B：
 *     1. sched_tick → sched_yield → arch_context_switch(A, B)
 *     2. CPU 跳到 B 的代码（B 在它上次 sched_yield 的下一行继续）
 *     3. B 执行直到下次 sched_yield 或被抢占
 *     4. 等下次切回 A，A 从 arch_context_switch 返回，继续 sched_yield，
 *        再返回 sched_tick，再返回到 pit_irq_handler 这里
 *
 *   也就是说，pit_irq_handler 的"后半段"（sched_tick 之后的代码）
 *   是在【A 被切回来之后】才执行的。
 *   所以心跳打印属于 A，而不是切换时的 B。这是符合预期的。
 * --------------------------------------------------------------- */
static volatile u64 pit_print_counter = 0;

static void pit_irq_handler(struct interrupt_frame *frame) {
    (void)frame;            /* PIT 不需要 frame 信息 */

    /* 增加 tick 计数 */
    arch_pit_increment_tick();

    /* Timer tick print removed for clean output */

    /* 【Lesson 5】驱动任务调度器
     * 可能触发上下文切换（时间片用完时） */
    sched_tick();
}

/* ---------------------------------------------------------------
 * arch_irq_init — 初始化 IRQ 注册表
 *
 * 流程：
 *   1. 清空 irq_table[16] 全部为 NULL
 *   2. 注册默认的 PIT handler（IRQ0）
 *
 * 之后外部驱动调用 arch_irq_register 注册自己的 callback
 * （例如键盘驱动注册 IRQ1 callback）。
 * --------------------------------------------------------------- */
void arch_irq_init(void) {
    /* 清空注册表 */
    for (int i = 0; i < 16; i++) {
        irq_table[i] = NULL;
    }

    /* 注册 PIT 默认 handler */
    irq_table[0] = pit_irq_handler;
}

/* ---------------------------------------------------------------
 * arch_irq_register — 注册 IRQ 处理回调
 *
 * 参数：
 *   irq      - IRQ 号（0~15）
 *   handler  - 处理函数指针
 *
 * 如果该 IRQ 已注册，覆盖（不返回错误，简化设计）。
 * 教学内核不需要复杂的 IRQ 共享机制。
 * --------------------------------------------------------------- */
void arch_irq_register(int irq, interrupt_handler_t handler) {
    if (irq < 0 || irq >= 16) {
        return;
    }
    irq_table[irq] = handler;
}

/* ---------------------------------------------------------------
 * arch_irq_unregister — 注销 IRQ 处理回调
 * --------------------------------------------------------------- */
void arch_irq_unregister(int irq) {
    if (irq < 0 || irq >= 16) {
        return;
    }
    irq_table[irq] = NULL;
}

/* ---------------------------------------------------------------
 * arch_irq_dispatch — 中断分发（核心）
 *
 * 由 idt.asm 的 isr_common 调用，frame 参数是中断栈帧。
 *
 * 流程：
 *   1. 从 frame->int_no 判断中断类型：
 *      - 0~31  → CPU 异常，调 arch_exception_handler（panic）
 *      - 32~47 → 外部中断，查 irq_table[irq]
 *      - 其他  → 未知中断，警告
 *   2. 【关键】先给 PIC 发 EOI（early EOI），再调 handler
 *   3. 调用对应 handler（如果注册了）
 *
 * 【为什么 EOI 必须在 handler 之前（early EOI）】
 *
 *   原代码是 "handler → EOI"，handler 内部可能 sched_yield →
 *   arch_context_switch 切到另一个任务。这种情况下 handler 永不返回，
 *   EOI 永远不发 → PIC 认为 IRQ0 还在处理 → 阻塞所有后续中断 →
 *   timer 停 → 系统冻结。
 *
 *   典型场景：
 *     1. file-srv 调 sched_exit → sched_yield
 *     2. sched_yield 发现 prev=TERMINATED、run_queue 空（init 在 sleep）
 *        → 进入 halt loop
 *     3. timer IRQ 触发 → pit_irq_handler → sched_tick → sched_yield（重入）
 *     4. 重入的 sched_yield 唤醒 init（sleep 到期）→ arch_context_switch
 *     5. 切到 init，file-srv 的栈被抛弃（TERMINATED）
 *     6. IRQ handler 永不返回 → EOI 永不发 → timer 永久停
 *     7. init 跑完 L7 demo 进入 idle loop → hlt → 永远不醒
 *
 *   early EOI 把"通知 PIC 处理完"提到 handler 之前，即使 handler
 *   context-switch 不返回，PIC 也已经 ready 接收下一个中断。
 *
 * 【early EOI 安全性】
 *   - 中断门（interrupt gate）进入时 CPU 自动清 IF，handler 期间不再嵌套
 *     同 CPU 的中断，所以不会出现"同号 IRQ 重入"
 *   - PIC 可以立刻发下一个 IRQ 到 CPU，但 CPU 因 IF=0 暂不响应，等
 *     iretq 恢复 IF 后才处理（不会丢，PIC IRR 会缓存）
 *   - 边沿触发（8259 默认）：每个 IRQ 边沿只触发一次，不会 storm
 *   - 这是 Linux 2.6+ / Windows 等真实 OS 处理 8259 PIC 的标准做法
 *
 * 【异常为什么不需要 EOI】
 *   CPU 异常不经过 PIC，PIC 的 IRR 没置位，发 EOI 没意义。
 *   arch_pic_eoi 内部对 < 32 的向量直接返回。
 * --------------------------------------------------------------- */
void arch_irq_dispatch(struct interrupt_frame *frame) {
    int vec = (int)frame->int_no;

    /* --- CPU 异常 0~31 --- */
    if (vec < 32) {
        arch_exception_handler(frame);
        /* arch_exception_handler 内部 panic，不返回 */
        return;
    }

    /* --- 【Lesson 8】syscall: int 0x80 ---
     *
     *   这是软件中断（user 主动 int 0x80），不是 PIC 设备中断。
     *   不需要 EOI（PIC 的 IRR 没置位，发 EOI 没意义且可能干扰）。
     *   直接调 syscall_handler，返回后 isr_common 恢复寄存器 + iretq 回 ring 3。
     *
     *   frame 含 user 进入 syscall 时的寄存器：
     *     rax=号, rdi/rsi/rdx=参数, rip=user 返回地址, cs=0x23, rsp=user 栈
     *   syscall_handler 把返回值写 frame->rax。 */
    if (vec == 0x80) {
        syscall_handler(frame);
        return;
    }

    /* --- 外部中断 32~47（IRQ0~15） --- */
    if (vec < 48) {
        int irq = vec - 32;

        /* 【关键修复】early EOI：在调用 handler 之前先通知 PIC
         *   这样即使 handler 内部 sched_yield → context_switch 永不返回，
         *   PIC 也能继续接收下一个中断。
         *   必须在 handler 之前发，否则 context-switch 后 EOI 永远丢失。 */
        arch_pic_eoi(vec);

        /* 调用注册的 handler
         *   注意：handler 可能 sched_yield 切走且永不返回（TERMINATED 场景），
         *   以下代码只有在 handler 正常返回时才执行 */
        if (irq_table[irq] != NULL) {
            irq_table[irq](frame);
        } else {
            /* 未注册的 IRQ，警告（EOI 已发，不影响后续中断） */
            arch_console_set_color(CON_COLOR_YELLOW);
            arch_console_print("\n[WARN] Unhandled IRQ #");
            arch_console_set_color(CON_COLOR_DEFAULT);
            kprint_dec((u64)irq);
            arch_console_print(" (vector ");
            kprint_dec((u64)vec);
            arch_console_print(")\n");
        }
        return;
    }

    /* --- 未知中断（48~255）---
     * 可能是软件中断（int 0x60）或 APIC 中断。
     * 教学内核不处理，警告即可。
     * 不发 EOI（不是 PIC 中断，发了也没用）。 */
    arch_console_set_color(CON_COLOR_YELLOW);
    arch_console_print("\n[WARN] Unknown interrupt vector ");
    arch_console_set_color(CON_COLOR_DEFAULT);
    kprint_dec((u64)vec);
    arch_console_print("\n");
}
