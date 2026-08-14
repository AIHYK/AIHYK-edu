/* ================================================================
 * arch/idt.h — IDT（Interrupt Descriptor Table）架构抽象接口
 *
 * 【Lesson 3 核心新增】
 *
 * IDT 是什么：
 *   IDT 是一张表，共 256 项，每项 16 字节，描述"当中断 N 发生时，
 *   应该跳到哪个地址执行什么代码"。
 *
 *   类比 GDT（全局描述符表）：
 *     GDT 表描述"段选择子 → 段属性"
 *     IDT 表描述"中断向量号 → 处理程序入口"
 *
 * IDT 项格式（x86-64 长模式，16 字节）：
 *
 *   ┌─────────────────────────────────────────────────────────────┐
 *   │ 字节偏移  字段            说明                              │
 *   ├─────────────────────────────────────────────────────────────┤
 *   │ 0-1      offset_low      handler 地址 [0:15]              │
 *   │ 2-3      selector        代码段选择子（通常 0x08）         │
 *   │ 4        ist             IST 索引（0=不用 IST）           │
 *   │ 5        flags           P|DPL|0|Type                      │
 *   │ 6-7      offset_mid      handler 地址 [16:31]            │
 *   │ 8-11     offset_high     handler 地址 [32:63]             │
 *   │ 12-15    reserved        保留（必须 0）                    │
 *   └─────────────────────────────────────────────────────────────┘
 *
 *   flags 字节（8 位）：
 *     bit 7   P (Present)         = 1（有效项）
 *     bit 6-5 DPL (Descriptor Privilege Level) = 00（ring 0）
 *     bit 4   0                   （固定 0）
 *     bit 3-0 Type                 = 0xE（中断门）/ 0xF（陷阱门）
 *
 *   Type 0xE vs 0xF：
 *     0xE = Interrupt Gate：进入时 CPU 自动清除 EFLAGS.IF（关中断）
 *     0xF = Trap Gate：进入时不清 IF（中断可以嵌套）
 *
 *     CPU 异常用陷阱门（异常处理时允许调试器中断），
 *     外部中断用中断门（避免嵌套把栈打爆）。
 *
 * IST（Interrupt Stack Table）：
 *   某些严重异常（#DF, #MC, #NMI）发生时栈可能已损坏，
 *   IST 让 CPU 自动切换到一个独立栈（在 TSS 里定义）。
 *   我们目前不用 IST（=0），简化处理。
 *
 * 【IDTR 寄存器】
 *   IDTR 是 IDT 的指针寄存器，lidt 指令加载：
 *     - limit (2 bytes): IDT 大小 - 1（字节）
 *     - base  (8 bytes): IDT 物理地址
 *
 *   32 位 lidt 读 6 字节，64 位 lidt 读 10 字节。
 *
 * 【加载顺序】
 *   1. 在 C 里构造 IDT 表（每项填好 offset+selector+flags）
 *   2. 调用 arch_idt_load() 执行 lidt 指令（必须在汇编里，
 *      因为 C 不能直接执行 lidt）
 *   3. 从此 CPU 收到中断就走 IDT
 * ================================================================ */

#ifndef ARCH_IDT_H
#define ARCH_IDT_H

#include <kernel/types.h>
#include <arch/interrupts.h>

/* ---------------------------------------------------------------
 * IDT 项（Gate Descriptor，16 字节）
 *
 * 【为什么不用位域】
 *   位域在不同编译器/字节序下布局可能不同，
 *   显式位操作更可控、可移植。
 *   这个结构体只是"把 16 字节当作整体"的容器，
 *   真正填值用 set_gate 函数（位操作精确控制）。
 * --------------------------------------------------------------- */
struct idt_entry {
    u16 offset_low;     /* [0:15]  handler 地址低 16 位 */
    u16 selector;        /* 代码段选择子（0x08 = 64 位代码段） */
    u8  ist;             /* IST 索引（0=不用） */
    u8  flags;           /* P|DPL|0|Type */
    u16 offset_mid;     /* [16:31] handler 地址中 16 位 */
    u32 offset_high;    /* [32:63] handler 地址高 32 位 */
    u32 reserved;        /* 保留 0 */
} __attribute__((packed));

/* IDTR 加载结构（lidt 操作数） */
struct idtr {
    u16 limit;           /* IDT 大小 - 1（字节） */
    u64 base;            /* IDT 物理地址 */
} __attribute__((packed));

/* ---------------------------------------------------------------
 * arch_idt_init — 初始化 IDT
 *
 * 流程：
 *   1. 把 256 项全部清零（确保未用的项 P=0，触发 #GP 而不是乱跳）
 *   2. 注册 CPU 异常处理程序（0~31）
 *   3. 注册外部中断处理程序（32~47）
 *   4. 调用 arch_idt_load() 执行 lidt
 *
 * 这个函数在 C 里调用，但 lidt 指令本身在 idt.asm 里实现
 * （C 不能直接 emit lidt 指令）。
 * --------------------------------------------------------------- */
void arch_idt_init(void);

/* ---------------------------------------------------------------
 * arch_idt_load — 执行 lidt 指令加载 IDTR
 *
 * 必须在汇编里实现（idt.asm），因为 C 无法直接 emit lidt。
 * 参数：idtr 指针（指向 limit + base 结构）
 * --------------------------------------------------------------- */
void arch_idt_load(struct idtr *idtr);

/* ---------------------------------------------------------------
 * arch_idt_set_gate — 设置 IDT 项
 *
 * 参数：
 *   vec      - 中断向量号（0~255）
 *   handler  - 处理程序入口地址
 *   selector - 代码段选择子（通常 0x08）
 *   flags    - P|DPL|0|Type（0x8E = ring0 中断门）
 *   ist      - IST 索引（0=不用）
 *
 * 把 64 位 handler 地址拆成低/中/高 3 段填入 IDT 项。
 * --------------------------------------------------------------- */
void arch_idt_set_gate(int vec, void *handler, u16 selector, u8 flags, u8 ist);

#endif /* ARCH_IDT_H */
