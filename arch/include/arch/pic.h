/* ================================================================
 * arch/pic.h — 8259 PIC（可编程中断控制器）架构抽象接口
 *
 * 【Lesson 3 核心新增】
 *
 * 8259 PIC 是什么：
 *   PC 上有两个 8259 PIC 芯片级联（Master + Slave），
 *   每个 PIC 有 8 个 IRQ 引脚，共 16 个 IRQ。
 *
 *   ┌───────────────────────────────────────────────────────────────┐
 *   │  CPU INTR ←── Master PIC (端口 0x20/0x21)                      │
 *   │                 IRQ0: 8254 PIT 定时器                          │
 *   │                 IRQ1: PS/2 键盘                                │
 *   │                 IRQ2: ←── Slave PIC 中断输出                    │
 *   │                 IRQ3: COM2                                     │
 *   │                 IRQ4: COM1                                     │
 *   │                 IRQ5: LPT2                                     │
 *   │                 IRQ6: 软盘                                     │
 *   │                 IRQ7: LPT1                                     │
 *   │                                                                │
 *   │  Slave PIC (端口 0xA0/0xA1)                                    │
 *   │                 IRQ8: CMOS RTC                                 │
 *   │                 IRQ9: 留给 PCI                                  │
 *   │                 IRQ10-11: 留给 PCI                             │
 *   │                 IRQ12: PS/2 鼠标                                │
 *   │                 IRQ13: 协处理器                                 │
 *   │                 IRQ14: IDE 主                                   │
 *   │                 IRQ15: IDE 从                                   │
 *   └───────────────────────────────────────────────────────────────┘
 *
 *   级联方式：Slave 的中断输出连到 Master 的 IRQ2。
 *     当 IRQ8~15 任一触发 → Slave 给 Master IRQ2 → Master 给 CPU
 *     所以 IRQ8~15 必须给两个 PIC 都发 EOI。
 *
 * 8259 PIC 寄存器：
 *   Master 端口 0x20 (命令) + 0x21 (数据)
 *   Slave  端口 0xA0 (命令) + 0xA1 (数据)
 *
 *   ICW（Initialization Command Words）：
 *     ICW1: bit0=IC4(需 ICW4), bit1=SNGL(单/级联), bit4=1(初始化标志)
 *     ICW2: 中断向量基址（IRQ0 映射到哪个向量）
 *     ICW3: 级联配置（Master 的 bit2=1 表示 IRQ2 连 Slave；
 *           Slave 的高5位=2 表示连到 Master 的 IRQ2）
 *     ICW4: bit0=8086 模式, bit1=AEOI(自动 EOI)
 *
 *   OCW（Operation Command Words）：
 *     OCW1 = IMR（中断屏蔽字），bit=1 屏蔽对应 IRQ
 *     OCW2 = EOI（写 0x20 表示非特定 EOI）
 *     OCW3 = 读 ISR/IRR
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
 * ================================================================ */

#ifndef ARCH_PIC_H
#define ARCH_PIC_H

#include <kernel/types.h>

/* ---------------------------------------------------------------
 * arch_pic_init — 初始化 8259 PIC（重映射 IRQ 到向量 32~47）
 *
 * 流程：
 *   1. Master/Slave 各发 ICW1（开始初始化）
 *   2. Master ICW2 = 0x20（IRQ0 → 向量 32）
 *      Slave  ICW2 = 0x28（IRQ8 → 向量 40）
 *   3. Master ICW3 = 0x04（IRQ2 连 Slave）
 *      Slave  ICW3 = 0x02（Slave 连到 Master IRQ2）
 *   4. Master/Slave ICW4 = 0x01（8086 模式，手动 EOI）
 *
 *   同时屏蔽所有 IRQ（避免初始化过程中收到未注册的中断）
 *   后续 arch_irq_enable 再逐个打开。
 * --------------------------------------------------------------- */
void arch_pic_init(void);

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
 *   - 其他向量（CPU 异常）：不发 EOI（不是 PIC 中断）
 * --------------------------------------------------------------- */
void arch_pic_eoi(int irq_no);

/* ---------------------------------------------------------------
 * arch_pic_mask — 屏蔽某个 IRQ
 * 参数：irq - IRQ 号（0~15）
 *   bit=1 在 IMR 里表示屏蔽
 * --------------------------------------------------------------- */
void arch_pic_mask(int irq);

/* ---------------------------------------------------------------
 * arch_pic_unmask — 取消屏蔽某个 IRQ
 * --------------------------------------------------------------- */
void arch_pic_unmask(int irq);

#endif /* ARCH_PIC_H */
