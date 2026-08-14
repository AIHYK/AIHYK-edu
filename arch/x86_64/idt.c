/* ================================================================
 * arch/x86_64/idt.c — IDT（中断描述符表）实现
 *
 * 【Lesson 3 核心新增】
 *
 * 这个文件负责：
 *   1. 定义 256 项 IDT 表（静态分配）
 *   2. 提供 arch_idt_set_gate 设置 IDT 项
 *   3. arch_idt_init 初始化整个 IDT + 调用 lidt 加载
 *
 * IDT 项格式（x86-64，16 字节，见 idt.h）：
 *   offset_low  - handler 地址 [0:15]
 *   selector    - 代码段（0x08 = gdt64.code）
 *   ist         - IST 索引（0=不用）
 *   flags       - P|DPL|0|Type（0x8E = ring0 中断门）
 *   offset_mid  - handler 地址 [16:31]
 *   offset_high - handler 地址 [32:63]
 *   reserved    - 0
 *
 * 【关键陷阱】
 *   IDT 表本身在 .bss 段（uninitialized data），
 *   但 IDTR.base 必须是【物理地址】（identity map 下等于虚拟地址）。
 *   我们内核运行在 identity mapping 下，所以虚拟地址 = 物理地址，
 *   直接用 &idt[0] 作为 IDTR.base 即可。
 *
 * 【初始化顺序】（必须严格遵守）
 *   1. 主程序调用 arch_pic_init()    - 重映射 PIC（IRQ → 向量 32~47）
 *   2. 主程序调用 arch_idt_init()     - 注册所有异常 + IRQ handler
 *   3. arch_idt_load() 执行 lidt      - IDTR 指向 idt[]
 *   4. 主程序调用 arch_pit_init()     - 启动定时器（开始产生中断）
 *   5. 主程序调用 arch_pic_unmask(0)  - 允许 IRQ0（定时器）
 *   6. 主程序 arch_sti()              - 开 CPU 中断响应
 *
 *   顺序不能乱：必须先 lidt 再 sti，否则中断来了找不到 IDT → #GP
 *   必须先 PIC 重映射再 lidt，否则 IRQ0 来时 PIC 还在向量 8 → #DF
 * ================================================================ */

#include <arch/idt.h>
#include <arch/interrupts.h>
#include <arch/io.h>
#include <arch/pic.h>
#include <arch/irq.h>
#include <kernel/panic.h>
#include <kernel/types.h>

/* ---------------------------------------------------------------
 * GDT 代码段选择子（用于 IDT 项的 selector 字段）
 *
 * 和 entry.asm 里的 CODE_SEG = 0x08 一致：
 *   index 1（gdt64.code）+ TI=0 + RPL=0 = 0x08
 * --------------------------------------------------------------- */
#define KERNEL_CODE_SELECTOR 0x08

/* IDT 项 flags:
 *   0x8E = 1000_1110
 *     bit 7   P=1    (Present)
 *     bit 6-5 DPL=00 (ring 0)
 *     bit 4   0
 *     bit 3-0 Type=1110 (Interrupt Gate, 64-bit)
 *
 *   用中断门（不用陷阱门）：进入时 CPU 自动清 IF（关中断嵌套），
 *   避免 IRQ 在处理过程中被新 IRQ 嵌套，把栈打爆。 */
#define IDT_FLAG_INTERRUPT_GATE 0x8E

/* ---------------------------------------------------------------
 * IDT 表（256 项 × 16 字节 = 4096 字节）
 *
 * 用 static 避免全局命名空间污染。
 * 不用 const 因为我们要动态写入（set_gate）。
 *
 * 【为什么放 .bss 不放 .data】
 *   .bss 在内存里但不占 ELF 文件空间（启动时由 bootloader 清零），
 *   我们运行时才填值，所以放 .bss 更合适。
 *   C 的"全局变量默认 0"语义让 .bss 自动清零。
 *
 * 【align 16】
 *   IDT 没有对齐要求（CPU 不要求），
 *   但对齐能让 cache 友好（如果中断频繁）。
 * --------------------------------------------------------------- */
static struct idt_entry idt[IDT_ENTRIES] __attribute__((aligned(16)));

/* IDTR 加载结构（lidt 操作数） */
static struct idtr idtr;

/* ---------------------------------------------------------------
 * isr_table - 256 项 ISR 入口地址表
 *
 * 在 idt.asm 里用 %rep 生成，每项是一个 8 字节地址。
 * 我们 extern 进来，循环填到 IDT 里。
 *
 * 【为什么不直接 extern 每个 isrN】
 *   手写 256 个 extern + 数组初始化太痛苦，
 *   且容易写错。asm 里用 %rep 生成数组，
 *   C 里循环读，最简洁。
 * --------------------------------------------------------------- */
extern u64 isr_table[IDT_ENTRIES];

/* ---------------------------------------------------------------
 * arch_idt_set_gate — 设置一个 IDT 项
 *
 * 参数：
 *   vec      - 中断向量号（0~255）
 *   handler  - 处理程序入口地址（函数指针）
 *   selector - 代码段选择子（通常 0x08）
 *   flags    - P|DPL|0|Type（0x8E = ring0 interrupt gate）
 *   ist      - IST 索引（0=不用 IST）
 *
 * 把 64 位 handler 地址拆成低/中/高 3 段（16+16+32 位）填入 IDT 项。
 *
 * 为什么不直接 memcpy？
 *   IDT 项不是简单的连续字节，而是位域结构。
 *   显式拆分让代码意图清晰，且能逐字段设置 ist/flags/selector。
 *
 * 校验：
 *   vec 越界则 panic（说明注册代码有 bug）
 * --------------------------------------------------------------- */
void arch_idt_set_gate(int vec, void *handler, u16 selector, u8 flags, u8 ist) {
    /* 校验向量号 */
    if (vec < 0 || vec >= IDT_ENTRIES) {
        PANIC("arch_idt_set_gate: vector out of range");
    }

    /* 把 64 位 handler 地址拆成 3 段 */
    u64 addr = (u64)handler;
    idt[vec].offset_low  = (u16)(addr & 0xFFFF);          /* [0:15] */
    idt[vec].offset_mid  = (u16)((addr >> 16) & 0xFFFF);  /* [16:31] */
    idt[vec].offset_high = (u32)((addr >> 32) & 0xFFFFFFFF); /* [32:63] */

    /* 其他字段 */
    idt[vec].selector = selector;
    idt[vec].ist       = ist;
    idt[vec].flags     = flags;
    idt[vec].reserved  = 0;
}

/* ---------------------------------------------------------------
 * arch_idt_init — 初始化 IDT
 *
 * 流程：
 *   1. 把 256 项全部清零（确保未用的项 P=0，触发 #GP 而不是乱跳）
 *   2. 从 isr_table[0..255] 循环填到 IDT 表
 *   3. 设置 IDTR = { limit = sizeof(idt)-1, base = &idt }
 *   4. lidt 加载 IDTR（汇编实现）
 *
 * 【为什么所有中断都用 0x8E flags】
 *   0x8E = 中断门（Interrupt Gate）：
 *     进入时 CPU 自动清 IF（关中断嵌套）
 *     避免 IRQ 在处理过程中被新 IRQ 嵌套，把栈打爆
 *
 *   陷阱门（0x8F）保留给特殊情况（syscall、调试器），
 *   教学内核暂时全部用中断门，简化设计。
 * --------------------------------------------------------------- */
void arch_idt_init(void) {
    /* 第 1 步：清零 IDT 表
     *
     * .bss 默认是 0，但显式清零更安全（防止 bootloader 没清 .bss） */
    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt[i].offset_low  = 0;
        idt[i].selector    = 0;
        idt[i].ist          = 0;
        idt[i].flags        = 0;     /* P=0 → 触发 #GP */
        idt[i].offset_mid  = 0;
        idt[i].offset_high = 0;
        idt[i].reserved    = 0;
    }

    /* 第 2 步：循环填 256 项
     *
     * isr_table[i] 是 asm 里生成的 isr_i 函数地址（u64）。
     * 我们强转成 void* 传给 set_gate。
     * selector = 0x08（内核 64 位代码段）
     * flags = 0x8E（ring0 中断门）
     * ist = 0（不用 IST，统一用当前栈） */
    for (int i = 0; i < IDT_ENTRIES; i++) {
        void *handler = (void *)isr_table[i];
        arch_idt_set_gate(i, handler, KERNEL_CODE_SELECTOR,
                          IDT_FLAG_INTERRUPT_GATE, 0);
    }

    /* 【Lesson 8】syscall 门：IDT[0x80] = 中断门 + DPL=3
     *
     *   默认 0x8E 的 DPL=00（ring 0），ring 3 代码执行 int 0x80 会触发 #GP。
     *   syscall 必须 ring 3 能 invoke，所以把 DPL 改成 3。
     *
     *   0xEE = 1110_1110：
     *     bit 7   P=1    (Present)
     *     bit 6-5 DPL=11 (ring 3 可调用) ★关键
     *     bit 4   0
     *     bit 3-0 Type=1110 (Interrupt Gate, 64-bit)
     *
     *   用中断门（不用陷阱门 0xEF）：进入时 CPU 自动清 IF，
     *   syscall handler 运行期间不被 IRQ 打断，简化并发推理。
     *   SYS_yield 调 sched_yield 时 sched_yield 内部 arch_irq_save/restore，
     *   正确处理 IF 状态。iretq 回 ring 3 时恢复 user RFLAGS（IF=1）。
     *
     *   handler 仍是 isr_table[0x80]（isr128 stub），它 push 0+0x80 → isr_common
     *   → arch_irq_dispatch 看到 vec=0x80 路由到 syscall_handler。 */
    arch_idt_set_gate(0x80, (void *)isr_table[0x80], KERNEL_CODE_SELECTOR,
                      0xEE, 0);

    /* 第 3 步：设置 IDTR */
    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (u64)&idt[0];

    /* 第 4 步：lidt 加载 IDTR
     *
     * 必须在汇编里执行（C 没有 lidt 指令）。
     * 加载后，CPU 立即用新 IDT 处理中断。
     * 此时中断还是关的（IF=0），所以安全。 */
    arch_idt_load(&idtr);
}
