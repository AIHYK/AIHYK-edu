/* ================================================================
 * arch/x86_64/pic.c — 8259 PIC（可编程中断控制器）实现
 *
 * 【Lesson 3 核心新增】
 *
 * PC 上有两个 8259 PIC 芯片级联（Master + Slave），
 * 每个 PIC 有 8 个 IRQ 引脚，共 16 个 IRQ。
 *
 * 8259 PIC 寄存器（端口）：
 *   Master 端口 0x20 (命令) + 0x21 (数据)
 *   Slave  端口 0xA0 (命令) + 0xA1 (数据)
 *
 * ICW（Initialization Command Words）初始化命令字：
 *   ICW1: bit0=IC4(需 ICW4), bit1=SNGL(单/级联), bit4=1(初始化标志)
 *   ICW2: 中断向量基址（IRQ0 映射到哪个向量）
 *   ICW3: 级联配置
 *   ICW4: bit0=8086 模式, bit1=AEOI(自动 EOI)
 *
 * OCW（Operation Command Words）操作命令字：
 *   OCW1 = IMR（中断屏蔽字），bit=1 屏蔽对应 IRQ
 *   OCW2 = EOI（写 0x20 表示非特定 EOI）
 *
 * 【为什么必须重映射】
 *   8259 PIC 默认把 IRQ0~7 映射到 CPU 向量 8~15，
 *   但 8~15 是 CPU 异常（#DF, #TS, #NP, #SS, #GP...）。
 *   如果不重映射，IRQ1（键盘）触发会进入 #GP handler，乱套！
 *
 *   标准做法：Master IRQ0~7 → 向量 32~39（0x20~0x27）
 *             Slave  IRQ8~15 → 向量 40~47（0x28~0x2F）
 *
 * 【EOI（End of Interrupt）】
 *   PIC 收到 IRQ 后，必须等 CPU 发 EOI 才会再发下一个。
 *   如果忘了 EOI，对应 IRQ 永久屏蔽（再也收不到中断）。
 *
 *   EOI 顺序对 IRQ8~15：
 *     1. 先给 Slave 发 EOI
 *     2. 再给 Master 发 EOI
 *   否则会乱序。
 *
 * 【屏蔽所有中断时】
 *   arch_pic_init 把 IMR 全设为 1（屏蔽所有 IRQ），
 *   然后逐个 arch_pic_unmask 打开需要的 IRQ。
 *   这样初始化过程中不会被未注册的中断打断。
 * ================================================================ */

#include <arch/io.h>
#include <arch/pic.h>
#include <kernel/types.h>

/* ---------------------------------------------------------------
 * 8259 PIC 端口定义
 *
 *   Master PIC:
 *     0x20 - 命令/OCW2 端口（写 EOI、读取状态等）
 *     0x21 - 数据/OCW1 端口（读/写 IMR 中断屏蔽字）
 *
 *   Slave PIC:
 *     0xA0 - 命令端口
 *     0xA1 - 数据端口
 * --------------------------------------------------------------- */
#define PIC_MASTER_CMD   0x20
#define PIC_MASTER_DATA  0x21
#define PIC_SLAVE_CMD    0xA0
#define PIC_SLAVE_DATA   0xA1

/* EOI 命令字（OCW2: bit4=0 表示是 OCW2, bit5=0 非旋转,
 *                bit7-6=00 非特定 EOI, bit1-0=11 + bit2=0 表示"通知所有IRR中断处理完"）
 * 0x20 = 0010_0000，最简单的非特定 EOI */
#define PIC_EOI 0x20

/* ICW1: 0x11 = 0001_0001
 *   bit 4 = 1 (init flag, 必须)
 *   bit 3 = 0 (edge-triggered, 边沿触发；=1 是 level-triggered)
 *   bit 1 = 0 (cascade mode，级联模式)
 *   bit 0 = 1 (ICW4 needed，需要写 ICW4)
 *
 * 我们用边沿触发（默认），level 触发用于 PCI 等共享 IRQ */
#define PIC_ICW1 0x11

/* ICW4: 0x01 = 0000_0001
 *   bit 0 = 1 (8086 mode，使用 8086 中断向量，非 MCS-80/85)
 *   bit 1 = 0 (manual EOI，手动 EOI；=1 是 auto-EOI)
 *
 * 我们用手动 EOI（manual）：
 *   - auto-EOI 在中断进入时 PIC 自动重置 IRR，
 *     简单但失去嵌套控制（同优先级中断会嵌套进来）
 *   - manual EOI 在中断处理程序末尾手动写 EOI，
 *     更可控，Linux 也用手动 EOI */
#define PIC_ICW4 0x01

/* ICW3:
 *   Master ICW3 = 0x04 = 0000_0100
 *     bit 2 = 1 表示 IRQ2 连了 Slave（其他位 0 = 对应 IRQ 没 Slave）
 *
 *   Slave ICW3 = 0x02
 *     高 5 位无意义（仅当级联为字节模式才有用）
 *     低 3 位 = 2 表示 Slave 连到 Master 的 IRQ2
 *
 *   【为什么是 IRQ2】
 *     IBM PC/AT 设计：Slave 的中断输出接到 Master 的 IRQ2 引脚。
 *     所以 Master 必须知道 IRQ2 是"Slave 专用"，不能给其他设备用。
 *     Slave 必须知道自己连在 Master 的 IRQ2 上（用于级联信号）。
 * --------------------------------------------------------------- */
#define PIC_MASTER_ICW3 0x04
#define PIC_SLAVE_ICW3  0x02

/* 全屏蔽字：所有 IRQ 屏蔽 */
#define PIC_ALL_MASKED 0xFF

/* IRQ 在 PIC 内部的"硬件号"
 *   IRQ0~7  → Master bit 0~7（OCW1 mask bit 0~7）
 *   IRQ8~15 → Slave bit 0~7（OCW1 mask bit 0~7）
 *
 * 8259 PIC 的 IRQ0~7 在 Master 上，
 * IRQ8~15 在 Slave 上（Slave 通过 IRQ2 进 Master）
 *
 * 屏蔽/取消屏蔽时需要按位操作 IMR 寄存器。 */
static inline u8 irq_to_master_mask_bit(int irq) {
    /* IRQ0~7 → Master bit 0~7 */
    return (u8)(1 << irq);
}

static inline u8 irq_to_slave_mask_bit(int irq) {
    /* IRQ8~15 → Slave bit 0~7 */
    return (u8)(1 << (irq - 8));
}

/* ---------------------------------------------------------------
 * arch_pic_init — 初始化 8259 PIC（重映射 IRQ 到向量 32~47）
 *
 * ICW 初始化序列（必须严格按顺序）：
 *   Master:
 *     1. outb(0x20, ICW1)   - 开始初始化
 *     2. outb(0x21, 0x20)    - ICW2: IRQ0 → 向量 32
 *     3. outb(0x21, 0x04)    - ICW3: IRQ2 连 Slave
 *     4. outb(0x21, ICW4)   - 8086 模式，手动 EOI
 *   Slave:
 *     1. outb(0xA0, ICW1)   - 开始初始化
 *     2. outb(0xA1, 0x28)   - ICW2: IRQ8 → 向量 40
 *     3. outb(0xA1, 0x02)   - ICW3: Slave 连 Master IRQ2
 *     4. outb(0xA1, ICW4)   - 8086 模式
 *
 * 初始化完成后，立即屏蔽所有 IRQ（写 0xFF 到数据端口）。
 * 后续 arch_pic_unmask 再逐个打开。
 *
 * 【为什么 ICW 之间要 io_wait】
 *   旧 ISA 总线 PIC 处理 I/O 较慢，连续写可能丢命令。
 *   现代硬件不需要，但加上无害（仅 1us 延时）。
 *   严谨实现会加 io_wait，我们简化省略（QEMU 不需要）。
 * --------------------------------------------------------------- */
void arch_pic_init(void) {
    /* --- Master PIC 初始化 --- */
    outb(PIC_MASTER_CMD, PIC_ICW1);          /* ICW1: 开始初始化 */
    outb(PIC_MASTER_DATA, 0x20);              /* ICW2: IRQ0 → 向量 0x20 (32) */
    outb(PIC_MASTER_DATA, PIC_MASTER_ICW3);  /* ICW3: IRQ2 连 Slave */
    outb(PIC_MASTER_DATA, PIC_ICW4);          /* ICW4: 8086 模式 */

    /* --- Slave PIC 初始化 --- */
    outb(PIC_SLAVE_CMD, PIC_ICW1);            /* ICW1: 开始初始化 */
    outb(PIC_SLAVE_DATA, 0x28);               /* ICW2: IRQ8 → 向量 0x28 (40) */
    outb(PIC_SLAVE_DATA, PIC_SLAVE_ICW3);     /* ICW3: 连 Master IRQ2 */
    outb(PIC_SLAVE_DATA, PIC_ICW4);           /* ICW4: 8086 模式 */

    /* --- 屏蔽所有 IRQ ---
     * 初始化过程中没有 handler 注册，
     * 万一来个 IRQ 没人处理就乱跳了。
     * 等设备驱动 init 完，再 arch_pic_unmask 打开需要的 IRQ。
     */
    outb(PIC_MASTER_DATA, PIC_ALL_MASKED);
    outb(PIC_SLAVE_DATA, PIC_ALL_MASKED);
}

/* ---------------------------------------------------------------
 * arch_pic_eoi — 给 PIC 发 EOI（End of Interrupt）
 *
 * 必须在每个中断处理程序【结尾】调用，告诉 PIC "这个中断处理完了"。
 *
 * 参数：irq_no - 中断向量号（不是 IRQ 号，是重映射后的向量号 32~47）
 *
 * 规则：
 *   - 向量 32~39（IRQ0~7）：只给 Master 发 EOI
 *   - 向量 40~47（IRQ8~15）：给 Slave + Master 都发 EOI
 *
 * 【为什么用 SPECIFIC EOI 而不是 NON-SPECIFIC EOI】
 *
 *   Non-specific EOI (0x20): 清除 ISR 中【优先级最高】的 bit。
 *   Specific EOI (0x60|irq): 清除 ISR 中【指定的】bit。
 *
 *   问题场景（non-specific EOI 的 bug）：
 *
 *     1. timer ISR 运行（ISR bit 0 set），sched_tick 触发 context switch
 *     2. context switch 恢复 next task 的 RFLAGS，IF=1
 *     3. IRQ4 此时触发（COM1 收到字节），PIC 在 ISR 里同时置 bit 4
 *     4. IRQ4 handler 运行，调 arch_pic_eoi(36)
 *     5. non-specific EOI 清除 ISR 中最高优先级 bit = bit 0（timer！），
 *        而不是 bit 4（IRQ4）
 *     6. 结果：timer 被错误地 EOI 了（还没处理完就被清），
 *        IRQ4 的 ISR bit 仍 set → PIC 认为还在处理 IRQ4
 *     7. IRQ4 ISR bit 不清 → 低优先级 IRQ 被永久阻塞 → 最终系统冻结
 *
 *   Specific EOI 直接清除指定 IRQ 的 bit，不受嵌套影响，不会清错。
 *
 *   这是 8259 PIC 的经典 bug，Linux / Windows 都用 specific EOI。
 * --------------------------------------------------------------- */
void arch_pic_eoi(int irq_no) {
    /* CPU 异常（0~31）：不经过 PIC，不需要 EOI */
    if (irq_no < 32) {
        return;
    }

    /* 计算 IRQ 号（向量 32~47 → IRQ 0~15） */
    int irq = irq_no - 32;

    /* IRQ8~15（Slave）：先给 Slave 发 specific EOI */
    if (irq >= 8 && irq < 16) {
        outb(PIC_SLAVE_CMD, (u8)(0x60 | (irq - 8)));
        /* Slave 连在 Master 的 IRQ2，还要给 Master 发 IRQ2 的 EOI */
        outb(PIC_MASTER_CMD, 0x62);   /* specific EOI for IRQ2 */
        return;
    }

    /* IRQ0~7（Master）：给 Master 发 specific EOI */
    if (irq >= 0 && irq < 8) {
        outb(PIC_MASTER_CMD, (u8)(0x60 | irq));
    }
}

/* ---------------------------------------------------------------
 * arch_pic_mask — 屏蔽某个 IRQ
 *
 * 参数：irq - IRQ 号（0~15）
 *
 * 实现：把 IMR（中断屏蔽字）的对应 bit 设为 1
 *
 *   Master IMR 在 0x21，IRQ0~7 对应 bit 0~7
 *   Slave  IMR 在 0xA1，IRQ8~15 对应 bit 0~7
 *
 * 读-改-写：
 *   读出当前 IMR → 设对应 bit → 写回
 *   不能直接写（会覆盖其他 IRQ 的屏蔽状态）
 * --------------------------------------------------------------- */
void arch_pic_mask(int irq) {
    if (irq < 0 || irq >= 16) {
        return;
    }
    if (irq < 8) {
        /* Master */
        u8 mask = inb(PIC_MASTER_DATA);
        mask |= irq_to_master_mask_bit(irq);
        outb(PIC_MASTER_DATA, mask);
    } else {
        /* Slave */
        u8 mask = inb(PIC_SLAVE_DATA);
        mask |= irq_to_slave_mask_bit(irq);
        outb(PIC_SLAVE_DATA, mask);
    }
}

/* ---------------------------------------------------------------
 * arch_pic_unmask — 取消屏蔽某个 IRQ
 *
 * 实现：把 IMR 的对应 bit 清 0
 * --------------------------------------------------------------- */
void arch_pic_unmask(int irq) {
    if (irq < 0 || irq >= 16) {
        return;
    }
    if (irq < 8) {
        u8 mask = inb(PIC_MASTER_DATA);
        mask &= ~irq_to_master_mask_bit(irq);
        outb(PIC_MASTER_DATA, mask);
    } else {
        u8 mask = inb(PIC_SLAVE_DATA);
        mask &= ~irq_to_slave_mask_bit(irq);
        outb(PIC_SLAVE_DATA, mask);
    }
}
