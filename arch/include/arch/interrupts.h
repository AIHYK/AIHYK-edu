/* ================================================================
 * arch/interrupts.h — 中断框架的架构抽象接口
 *
 * 【Lesson 3 核心新增】
 *
 * 这个头文件定义：
 *   1. CPU 异常向量号常量（0~31）
 *   2. PIC IRQ 重映射后的向量号常量（32~47）
 *   3. IDT 项总数（256）
 *   4. 中断栈帧结构体（interrupt_frame）
 *
 * 中断处理的完整流程（x86-64）：
 *   ┌────────────────────────────────────────────────────────────┐
 *   │ 1. 设备触发 IRQ → 8259 PIC                                 │
 *   │ 2. PIC 向 CPU 发 INTR 信号                                 │
 *   │ 3. CPU 检查 IF（中断标志），IF=1 则响应                    │
 *   │ 4. CPU 收到中断向量号 N（PIC 发来的）                       │
 *   │ 5. CPU 自动保存上下文到栈：                                 │
 *   │      SS, RSP, RFLAGS, CS, RIP（特权级变化才压 SS:RSP）    │
 *   │      部分异常还压一个错误码                                │
 *   │ 6. CPU 从 IDT[N] 取出门描述符                              │
 *   │      如果是中断门：清除 EFLAGS.IF（关中断）                │
 *   │      如果是陷阱门：不清 IF                                  │
 *   │ 7. CPU 跳到 IDT[N].offset（中断处理程序入口）             │
 *   │ 8. 我们在中断 stub 里保存通用寄存器 → 调 C handler         │
 *   │ 9. handler 处理完毕 → stub 恢复寄存器 → iretq             │
 *   │ 10. CPU 自动恢复 SS:RSP:RFLAGS:CS:RIP                      │
 *   └────────────────────────────────────────────────────────────┘
 *
 * 【CPU 异常 vs 外部中断】
 *   CPU 异常（0~31）：
 *     由 CPU 内部产生，例如除零、缺页、非法指令
 *     错误码：部分异常有（#PF, #GP, #DF...），部分无（#DE, #UD...）
 *
 *   外部中断（32~255）：
 *     由外部设备产生，经 PIC 发来
 *     无错误码（CPU 不会自动压错误码）
 *     IRQ0~15 由 8259 PIC 重映射到向量 32~47
 *
 * 【为什么 IRQ 要重映射到 32~47】
 *   8259 PIC 默认把 IRQ0~15 映射到 CPU 中断向量 8~15，
 *   但 8~15 是 CPU 异常（#DF, #TS, #NP, #SS, #GP, #PF...），
 *   冲突！如果 IRQ7（并口）和 #GP（向量 13）都用向量 13，
 *   CPU 收到向量 13 时分不清是异常还是中断。
 *   解决：PIC 初始化时把基址改成 32（0x20），IRQ0~15 → 32~47。
 * ================================================================ */

#ifndef ARCH_INTERRUPTS_H
#define ARCH_INTERRUPTS_H

#include <kernel/types.h>

/* ---------------------------------------------------------------
 * IDT 容量
 *
 * x86-64 支持 256 个中断向量（0~255）。
 * 我们的 IDT 表就开 256 项。
 * --------------------------------------------------------------- */
#define IDT_ENTRIES 256

/* ---------------------------------------------------------------
 * CPU 异常向量号（0~31）
 *
 * 这些是 CPU 内部异常，不是外部中断。
 * Intel SDM Vol 3, Chapter 6 定义。
 *
 * 异常有"错误码"属性：
 *   - 有错误码：CPU 自动压入一个错误码（需 iretq 前 pop 掉）
 *   - 无错误码：CPU 不压错误码
 *
 * 为了统一处理，我们让"无错误码"的异常 stub 自己 push 0
 * 占位，这样所有异常进 C handler 时栈布局一致。
 * --------------------------------------------------------------- */
#define EXC_DE   0    /* #DE Divide Error（除零）              - 无错误码 */
#define EXC_DB   1    /* #DB Debug（单步/断点）                - 无错误码 */
#define EXC_NMI  2    /* NMI Non-Maskable Interrupt             - 无错误码 */
#define EXC_BP   3    /* #BP Breakpoint（int3 指令）            - 无错误码 */
#define EXC_OF   4    /* #OF Overflow（into 指令）              - 无错误码 */
#define EXC_BR   5    /* #BR Bound Range Exceeded               - 无错误码 */
#define EXC_UD   6    /* #UD Invalid Opcode（非法指令）         - 无错误码 */
#define EXC_NM   7    /* #NM Device Not Available（无 FPU）      - 无错误码 */
#define EXC_DF   8    /* #DF Double Fault                       - 有错误码 0 */
#define EXC_TS   10   /* #TS Invalid TSS                        - 有错误码 */
#define EXC_NP   11   /* #NP Segment Not Present                - 有错误码 */
#define EXC_SS   12   /* #SS Stack-Segment Fault                - 有错误码 */
#define EXC_GP   13   /* #GP General Protection                 - 有错误码 */
#define EXC_PF   14   /* #PF Page Fault                         - 有错误码 */
#define EXC_MF   16   /* #MF x87 Floating-Point Exception       - 无错误码 */
#define EXC_AC   17   /* #AC Alignment Check                    - 有错误码 */
#define EXC_MC   18   /* #MC Machine Check                      - 无错误码 */
#define EXC_XM   19   /* #XM SIMD Floating-Point Exception      - 无错误码 */
#define EXC_VE   20   /* #VE Virtualization Exception           - 无错误码 */

/* ---------------------------------------------------------------
 * 外部中断向量号（IRQ 重映射后）
 *
 * 8259 PIC 初始化时把基址设为 0x20（=32），
 * IRQ0~7  → 中断向量 32~39（0x20~0x27）
 * IRQ8~15 → 中断向量 40~47（0x28~0x2F）
 *
 * 常用 IRQ：
 *   IRQ0  - 8254 PIT 定时器（每秒 N 次中断，N 由分频决定）
 *   IRQ1  - PS/2 键盘（每次按键/松键触发）
 *   IRQ3  - COM2 串口
 *   IRQ4  - COM1 串口
 *   IRQ6  - 软盘
 *   IRQ8  - CMOS 实时钟
 *   IRQ12 - PS/2 鼠标
 *   IRQ14 - IDE 主控制器
 *   IRQ15 - IDE 从控制器
 * --------------------------------------------------------------- */
#define IRQ0  32    /* 8254 PIT 定时器 */
#define IRQ1  33    /* PS/2 键盘 */
#define IRQ2  34    /* 8259 从片级联（不可用，内部用） */
#define IRQ3  35    /* COM2 */
#define IRQ4  36    /* COM1 */
#define IRQ5  37    /* LPT2 */
#define IRQ6  38    /* 软盘 */
#define IRQ7  39    /* LPT1 */
#define IRQ8  40    /* CMOS RTC */
#define IRQ9  41    /* 留给 PCI */
#define IRQ10 42
#define IRQ11 43
#define IRQ12 44    /* PS/2 鼠标 */
#define IRQ13 45    /* 协处理器 */
#define IRQ14 46    /* IDE 主 */
#define IRQ15 47    /* IDE 从 */

/* ---------------------------------------------------------------
 * 中断栈帧（interrupt_frame）
 *
 * CPU 进入中断时【自动】压入下面这些寄存器（如果特权级变化，
 * 才压 SS:RSP；同特权级不压 SS:RSP，但 iretq 永远弹出它们，
 * 所以 stub 里为了统一处理，统一压 SS:RSP）。
 *
 * 注意：以下顺序是 CPU 压栈的顺序（从高地址到低地址）：
 *   SS     ← 高地址（最先压入）
 *   RSP
 *   RFLAGS
 *   CS
 *   RIP    ← 低地址（最后压入）
 *
 * 在栈上的布局（从低地址往上）就是反序：
 *   RIP, CS, RFLAGS, RSP, SS
 *
 * 通用寄存器（RAX, RBX, ..., R15）由我们的中断 stub【手动】压入。
 * 在 C 代码里这个结构体作为参数访问，字段顺序必须和压栈顺序匹配。
 * --------------------------------------------------------------- */
struct interrupt_frame {
    u64 r15;     /* stub 手动压入的通用寄存器（反序） */
    u64 r14;
    u64 r13;
    u64 r12;
    u64 r11;
    u64 r10;
    u64 r9;
    u64 r8;
    u64 rbp;
    u64 rdi;
    u64 rsi;
    u64 rdx;
    u64 rcx;
    u64 rbx;
    u64 rax;     /* 最后压入 RAX（stub 里第一个 push） */

    /* 下面是 stub 统一压入的"中断号 + 错误码" */
    u64 int_no;  /* 中断向量号（stub push 的） */
    u64 err_code;/* 错误码（CPU 压的或 stub 压 0） */

    /* 下面是 CPU 自动压入的 */
    u64 rip;     /* 中断时的指令指针 */
    u64 cs;      /* 中断时的代码段 */
    u64 rflags;  /* 中断时的标志寄存器 */
    u64 rsp;     /* 中断时的栈指针 */
    u64 ss;      /* 中断时的栈段 */
} __attribute__((packed));

/* ---------------------------------------------------------------
 * 中断处理程序函数原型
 *
 * 参数：
 *   frame - 指向中断栈帧的指针（包含所有寄存器快照）
 *
 * 为什么传 frame 而不是单独参数？
 *   - 不同中断需要的信息不同（异常需要 err_code，IRQ 不需要）
 *   - 一个统一的 frame 指针最简洁
 *   - 调试时可以 dump 整个 frame
 *
 * 返回值：无（中断处理程序不返回值）
 * --------------------------------------------------------------- */
typedef void (*interrupt_handler_t)(struct interrupt_frame *frame);

#endif /* ARCH_INTERRUPTS_H */
