/* ================================================================
 * arch/irq.h — IRQ 处理回调注册架构抽象接口
 *
 * 【Lesson 3 核心新增】
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
 *   ┌─────────────────────────────────────────────────────────────┐
 *   │ 1. 用户按键 → 键盘控制器发 IRQ1                              │
 *   │ 2. 8259 PIC 收到 IRQ1 → 向 CPU 发向量 33                    │
 *   │ 3. CPU 查 IDT[33] → 跳到 irq1 stub                          │
 *   │ 4. stub push 0（错误码占位）+ push 33（向量号）             │
 *   │ 5. stub push 通用寄存器 → call arch_irq_dispatch            │
 *   │ 6. arch_irq_dispatch 查 irq_table[1] → 调 keyboard_handler  │
 *   │ 7. keyboard_handler 读 0x60 端口拿 scancode → 解码 → 显示   │
 *   │ 8. keyboard_handler 返回                                    │
 *   │ 9. arch_pic_eoi(33)（告诉 PIC 处理完了）                    │
 *   │ 10. stub pop 通用寄存器 → iretq → 返回被中断处             │
 *   └─────────────────────────────────────────────────────────────┘
 * ================================================================ */

#ifndef ARCH_IRQ_H
#define ARCH_IRQ_H

#include <kernel/types.h>
#include <arch/interrupts.h>

/* ---------------------------------------------------------------
 * arch_irq_init — 初始化 IRQ 注册表
 *
 * 把 irq_table[16] 全清 NULL，确保未注册的 IRQ 不会调野指针。
 * --------------------------------------------------------------- */
void arch_irq_init(void);

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
void arch_irq_register(int irq, interrupt_handler_t handler);

/* ---------------------------------------------------------------
 * arch_irq_unregister — 注销 IRQ 处理回调
 * --------------------------------------------------------------- */
void arch_irq_unregister(int irq);

/* ---------------------------------------------------------------
 * arch_irq_dispatch — 中断分发
 *
 * 由 idt.asm 的 isr_common 调用，frame 参数是中断栈帧。
 *
 * 流程：
 *   1. 从 frame->int_no 减去 IRQ_BASE（32）得到 IRQ 号
 *   2. 如果在 0~15 范围内，查 irq_table[irq]
 *   3. 如果有 handler，调用它
 *   4. 给 PIC 发 EOI
 *
 * 注意：这里 EOI 必须在 handler 之后（handler 里可能还要读 I/O 端口，
 *       那时设备可能还没准备好；EOI 之前 PIC 不会再发同号中断，
 *       所以 EOI 在 handler 末尾发是安全的）。
 * --------------------------------------------------------------- */
void arch_irq_dispatch(struct interrupt_frame *frame);

#endif /* ARCH_IRQ_H */
