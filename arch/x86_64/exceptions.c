/* ================================================================
 * arch/x86_64/exceptions.c — CPU 异常处理程序
 *
 * 【Lesson 3 核心新增】
 *
 * CPU 异常（0~31）由 CPU 内部产生，例如：
 *   #DE (0):  除零
 *   #UD (6):  非法指令
 *   #DF (8):  双重异常（异常里又异常了）
 *   #GP (13): 通用保护异常（最常见的崩溃原因）
 *   #PF (14): 缺页异常
 *
 * 【Lesson 9 核心修改】异常恢复：用户态 fault 只杀任务，不 panic
 *
 *   关键判断：frame->cs & 0x3 告诉我们 fault 时的 CPL：
 *     - CS & 3 == 3：fault 来自 ring 3（用户态）→ 杀任务，系统存活
 *     - CS & 3 == 0：fault 来自 ring 0（内核态）→ 仍然 panic（内核 bug）
 *
 *   这是混合/微内核的精髓：fault isolation（故障隔离）。
 *   用户任务崩溃不应该拖垮整个系统。
 *
 *   用户态 fault 处理流程：
 *     1. 记录 fault 信息到 current->fault_type/fault_rip/fault_addr
 *     2. 根据 exception 向量号设置 exit_code（SIGSEGV/SIGILL 等）
 *     3. 打印黄色 USER FAULT 消息（非致命，不是红色 panic）
 *     4. 调用 sched_exit_with_code() → 任务死亡，系统继续
 *
 * 【错误码解读】
 *   异常的错误码（err_code）含义因异常而异：
 *
 *   #PF (向量 14) 错误码 bit 含义：
 *     bit 0 P (Present):     0=缺页（页不存在）, 1=保护违反
 *     bit 1 W/R:             0=读, 1=写
 *     bit 2 U/S:             0=内核态, 1=用户态
 *     bit 3 RSVD:            1=保留位被设了
 *     bit 4 I/D:             1=取指（指令缺页）
 *
 *   #GP/#SS/#TS/#NP 错误码格式：
 *     bit 15:3  段选择子索引
 *     bit 2     TI (0=GDT, 1=LDT)
 *     bit 1:0   IDT/GDT/LDT 来源
 *
 *   #DF (向量 8) 错误码恒为 0
 *   #AC (向量 17) 错误码恒为 0
 *
 * 【寄存器快照的价值】
 *   异常发生时 CPU 把所有寄存器压栈（见 idt.asm），
 *   frame 指针指向这些寄存器的快照。
 *   打印出来方便定位：
 *     - RIP: 出错指令地址（哪条指令崩溃了）
 *     - RSP: 栈指针（看栈是否溢出）
 *     - CR2: #PF 时是访问的地址（哪个地址缺页了）
 * ================================================================ */

#include <arch/console.h>
#include <arch/interrupts.h>
#include <arch/io.h>
#include <kernel/panic.h>
#include <kernel/sched.h>
#include <kernel/types.h>

/* ---------------------------------------------------------------
 * 异常名称表
 *
 * 把异常向量号映射到人类可读的名字。
 * 用于错误信息显示。
 *
 * Intel SDM Vol 3, Table 6-1 定义。
 * 0~21 是已定义异常，22~31 是保留（"Reserved"）。
 * --------------------------------------------------------------- */
static const char *const exception_names[] = {
    "#DE Divide Error",              /* 0 */
    "#DB Debug",                      /* 1 */
    "NMI Non-Maskable Interrupt",     /* 2 */
    "#BP Breakpoint",                 /* 3 */
    "#OF Overflow",                  /* 4 */
    "#BR Bound Range Exceeded",      /* 5 */
    "#UD Invalid Opcode",            /* 6 */
    "#NM Device Not Available",      /* 7 */
    "#DF Double Fault",               /* 8 */
    "#MF x87 Segment Overrun",       /* 9（已废弃）*/
    "#TS Invalid TSS",               /* 10 */
    "#NP Segment Not Present",       /* 11 */
    "#SS Stack Fault",               /* 12 */
    "#GP General Protection",        /* 13 */
    "#PF Page Fault",                /* 14 */
    "Reserved",                      /* 15 */
    "#MF x87 FPU Error",             /* 16 */
    "#AC Alignment Check",           /* 17 */
    "#MC Machine Check",             /* 18 */
    "#XM SIMD Exception",            /* 19 */
    "#VE Virtualization Exception", /* 20 */
    "#CP Control Protection",        /* 21 */
};

/* ---------------------------------------------------------------
 * 异常向量号 → 名称字符串
 * --------------------------------------------------------------- */
static const char *exc_name(int vec) {
    if (vec >= 0 && vec < (int)(sizeof(exception_names) / sizeof(exception_names[0]))) {
        return exception_names[vec];
    }
    if (vec < 32) {
        return "Reserved";
    }
    return "Unknown";
}

/* ---------------------------------------------------------------
 * print_hex_u64 — 打印 64 位十六进制数
 *
 * panic 用，但本文件单独写一份避免依赖 panic.c 的 static 函数。
 * --------------------------------------------------------------- */
static void print_hex_u64(u64 v) {
    char buf[17];
    int i = 0;
    const char *hex = "0123456789ABCDEF";

    if (v == 0) {
        arch_console_print("0x0");
        return;
    }

    /* 从最高位开始转换 */
    while (v > 0 && i < 16) {
        buf[i++] = hex[v & 0xF];
        v >>= 4;
    }
    arch_console_print("0x");
    while (i > 0) {
        arch_console_putchar(buf[--i]);
    }
}

/* ---------------------------------------------------------------
 * print_dec_u64 — 打印无符号十进制数
 * --------------------------------------------------------------- */
static void print_dec_u64(u64 v) {
    char buf[21];
    int i = 0;
    if (v == 0) {
        arch_console_putchar('0');
        return;
    }
    while (v > 0 && i < 20) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i > 0) {
        arch_console_putchar(buf[--i]);
    }
}

/* ---------------------------------------------------------------
 * print_dec_s64 — 打印有符号十进制数
 * --------------------------------------------------------------- */
static void __attribute__((unused)) print_dec_s64(s64 v) {
    if (v < 0) {
        arch_console_putchar('-');
        print_dec_u64((u64)(-v));
    } else {
        print_dec_u64((u64)v);
    }
}

/* ---------------------------------------------------------------
 * read_cr2 — 读取 CR2 寄存器（#PF 时是缺页地址）
 *
 * CR2 是 #PF 专用寄存器：异常发生时，CPU 把触发缺页的虚拟地址写入 CR2。
 * 打印 CR2 能直接看到"哪个地址访问失败"。
 *
 * 必须用内联汇编读 CR2（没有 C 内建）。
 * --------------------------------------------------------------- */
static u64 read_cr2(void) {
    u64 v;
    __asm__ volatile ("movq %%cr2, %0" : "=r"(v));
    return v;
}

/* ---------------------------------------------------------------
 * dump_frame — 打印中断栈帧（寄存器快照）
 *
 * 用于异常发生时调试：把所有寄存器值打印出来，
 * 可以看出崩溃现场（哪个寄存器异常、RSP 是否溢出等）。
 * --------------------------------------------------------------- */
static void dump_frame(const struct interrupt_frame *frame) {
    arch_console_set_color(CON_COLOR_CYAN);
    arch_console_print("\n  Register dump:\n");
    arch_console_set_color(CON_COLOR_DEFAULT);

    arch_console_print("    RAX="); print_hex_u64(frame->rax);
    arch_console_print("  RBX="); print_hex_u64(frame->rbx);
    arch_console_print("  RCX="); print_hex_u64(frame->rcx);
    arch_console_print("  RDX="); print_hex_u64(frame->rdx);
    arch_console_print("\n");
    arch_console_print("    RSI="); print_hex_u64(frame->rsi);
    arch_console_print("  RDI="); print_hex_u64(frame->rdi);
    arch_console_print("  RBP="); print_hex_u64(frame->rbp);
    arch_console_print("  RSP="); print_hex_u64(frame->rsp);
    arch_console_print("\n");
    arch_console_print("    R8 ="); print_hex_u64(frame->r8);
    arch_console_print("  R9 ="); print_hex_u64(frame->r9);
    arch_console_print("  R10="); print_hex_u64(frame->r10);
    arch_console_print("  R11="); print_hex_u64(frame->r11);
    arch_console_print("\n");
    arch_console_print("    R12="); print_hex_u64(frame->r12);
    arch_console_print("  R13="); print_hex_u64(frame->r13);
    arch_console_print("  R14="); print_hex_u64(frame->r14);
    arch_console_print("  R15="); print_hex_u64(frame->r15);
    arch_console_print("\n");
    arch_console_print("    RIP="); print_hex_u64(frame->rip);
    arch_console_print("  CS ="); print_hex_u64(frame->cs);
    arch_console_print("  RFLAGS="); print_hex_u64(frame->rflags);
    arch_console_print("\n");
    arch_console_print("    SS ="); print_hex_u64(frame->ss);
    arch_console_print("  ERR=0x"); print_hex_u64(frame->err_code);
    arch_console_print("  INT#="); print_dec_u64(frame->int_no);
    arch_console_print("\n");

    /* #PF 特殊：打印 CR2 */
    if (frame->int_no == EXC_PF) {
        arch_console_print("    CR2="); print_hex_u64(read_cr2());
        arch_console_print(" (faulting address)\n");
    }
}

/* ---------------------------------------------------------------
 * dump_page_fault — 打印 #PF 详细信息（Lesson 4 新增）
 *
 * #PF 错误码 bit 含义（Intel SDM Vol 3, Table 6-9）：
 *   bit 0 P (Present)  — 0 = 缺页（页不存在）；1 = 保护违反
 *   bit 1 W/R          — 0 = 读；1 = 写
 *   bit 2 U/S          — 0 = 内核态；1 = 用户态
 *   bit 3 RSVD         — 1 = 页表保留位被设了（坏页表）
 *   bit 4 I/D          — 1 = 取指（指令缺页，通常 NX bit 触发）
 *
 * 配合 CR2（缺页地址），能精确定位崩溃原因：
 *   "用户态写 0x12345678 缺页，因为页不存在"
 *   "内核态读 0xB8000 保护违反（页只读）"
 * --------------------------------------------------------------- */
static void dump_page_fault(const struct interrupt_frame *frame) {
    u64 err = frame->err_code;
    u64 cr2 = read_cr2();

    arch_console_set_color(CON_COLOR_CYAN);
    arch_console_print("  Page Fault Details:\n");
    arch_console_set_color(CON_COLOR_DEFAULT);

    /* 缺页地址 */
    arch_console_print("    Faulting address (CR2): ");
    print_hex_u64(cr2);
    arch_console_print("\n");

    /* 触发操作的类型 */
    arch_console_print("    Access type:     ");
    if (err & 0x10) {
        arch_console_print("Instruction Fetch (NX violation?)");
    } else if (err & 0x2) {
        arch_console_print("Write");
    } else {
        arch_console_print("Read");
    }
    arch_console_print("\n");

    /* 触发操作的特权级 */
    arch_console_print("    Access mode:     ");
    if (err & 0x4) {
        arch_console_print("User-mode");
    } else {
        arch_console_print("Kernel-mode");
    }
    arch_console_print("\n");

    /* 缺页原因 */
    arch_console_print("    Cause:           ");
    if (err & 0x1) {
        arch_console_print("Protection violation (page present, but access denied)\n");
        if (err & 0x8) {
            arch_console_print("    Reserved bit set in page table (corrupt page table?)\n");
        }
    } else {
        arch_console_print("Page not present (no physical page mapped here)\n");
    }

    /* 给出可能的 bug 原因（教学内核常见的几种） */
    arch_console_set_color(CON_COLOR_YELLOW);
    arch_console_print("    Possible cause: ");
    if ((err & 0x7) == 0x0) {
        /* 内核态读不存在 */
        arch_console_print("NULL pointer deref / accessing unmapped address\n");
    } else if ((err & 0x7) == 0x2) {
        /* 内核态写不存在 */
        arch_console_print("Writing to unmapped / read-only memory\n");
    } else if ((err & 0x7) == 0x3) {
        /* 内核态写已存在但只读 */
        arch_console_print("Writing to read-only page (e.g. .rodata)\n");
    } else if (err & 0x4) {
        /* 用户态（L9 后用户态 fault 不 panic，只杀任务） */
        arch_console_print("User-mode access fault (task will be killed)\n");
    } else {
        arch_console_print("Unknown\n");
    }
    arch_console_set_color(CON_COLOR_DEFAULT);
}

/* ---------------------------------------------------------------
 * exc_to_signal — 把异常向量号映射到信号式退出码
 *
 *   【Lesson 9 核心】用户态异常 → 类 Unix 信号
 *
 *   映射规则：
 *     #PF (14), #GP (13) → SIGSEGV (-11) 段错误
 *       这两个最常见：空指针解引用 / 访问越界 / 权限不足
 *     #UD (6), #NM (7)   → SIGILL  (-4)  非法指令
 *       执行了 CPU 不认识的指令
 *     #DE (0)             → SIGFPE  (-8)  算术异常
 *       除零
 *     #AC (17), #SS (12), #TS (10) → SIGBUS (-7) 总线错误
 *       对齐 / 栈 / TSS 问题
 *     #DF (8), #BP (3)    → SIGABRT (-6) 放弃
 *       双重异常 / 断点（int3）
 *     其他               → SIGABRT (-6) 未知异常
 *
 *   【为什么 #BP 映射到 SIGABRT 而不是 SIGTRAP】
 *     int3 是调试断点。在教学内核里，用户态 int3 没有调试器接，
 *     视为异常终止更合理。映射到 SIGABRT 和 Unix 的 abort() 行为一致。
 * --------------------------------------------------------------- */
static int exc_to_signal(int vec) {
    switch (vec) {
    case EXC_PF:                          /* #PF Page Fault */
    case EXC_GP:                          /* #GP General Protection */
        return TASK_SIG_SEGV;

    case EXC_UD:                          /* #UD Invalid Opcode */
    case EXC_NM:                          /* #NM Device Not Available */
        return TASK_SIG_ILL;

    case EXC_DE:                          /* #DE Divide Error */
        return TASK_SIG_FPE;

    case EXC_AC:                          /* #AC Alignment Check */
    case EXC_SS:                          /* #SS Stack Fault */
    case EXC_TS:                          /* #TS Invalid TSS */
        return TASK_SIG_BUS;

    case EXC_DF:                          /* #DF Double Fault */
    case EXC_BP:                          /* #BP Breakpoint */
    default:
        return TASK_SIG_ABORT;
    }
}

/* ---------------------------------------------------------------
 * signal_name — 信号退出码 → 可读名称
 * --------------------------------------------------------------- */
static const char *signal_name(int sig) {
    switch (sig) {
    case TASK_SIG_SEGV:  return "SIGSEGV";
    case TASK_SIG_ILL:   return "SIGILL";
    case TASK_SIG_FPE:   return "SIGFPE";
    case TASK_SIG_BUS:   return "SIGBUS";
    case TASK_SIG_ABORT: return "SIGABRT";
    default:             return "SIG???";
    }
}

/* ---------------------------------------------------------------
 * arch_exception_handler — CPU 异常处理
 *
 * 由 arch_irq_dispatch 调用（int_no 在 0~31 范围内时）。
 *
 * 【Lesson 9 核心修改】区分用户态 / 内核态 fault
 *
 *   frame->cs & 0x3 提取 CS 的 RPL（Requested Privilege Level），
 *   即 fault 发生时的 CPL：
 *     - 0 = ring 0（内核态）：仍然 panic，内核 bug 不可恢复
 *     - 3 = ring 3（用户态）：杀任务，系统继续运行
 *
 *   用户态 fault 路径：
 *     1. 记录 fault 信息（fault_type / fault_rip / fault_addr）
 *     2. 计算信号退出码（exc_to_signal）
 *     3. 打印黄色 USER FAULT 消息
 *     4. sched_exit_with_code → 任务死亡，调度器选下一个任务
 *
 *   内核态 fault 路径（不变）：
 *     1. 打印红色 !!! CPU EXCEPTION !!! 横幅
 *     2. dump 寄存器 + #PF 详情
 *     3. panic 永久停机
 *
 *   【为什么 frame->cs & 0x3 可靠判断 CPL】
 *     CPU 进入异常时把 CS 压栈，CS 的低 2 位是 RPL/CPL。
 *     ring 0 代码的 CS = 0x08（RPL=0），ring 3 的 CS = 0x23（RPL=3）。
 *     这是硬件行为，软件无法伪造。
 *
 *   【为什么 #PF 还要额外读 CR2】
 *     CR2 保存触发缺页的虚拟地址，对诊断至关重要。
 *     "segfault at 0x0" 比 "segfault at ???" 有用得多。
 * --------------------------------------------------------------- */
void arch_exception_handler(struct interrupt_frame *frame) {
    /* 【Lesson 9】判断 fault 来自 ring 0 还是 ring 3 */
    int cpl = (int)(frame->cs & 0x3);

    if (cpl == 3) {
        /* ============================================================
         * 用户态 fault：杀任务，不 panic
         *
         *   这是混合/微内核的核心特性：fault isolation。
         *   用户任务的 bug（空指针、非法指令等）只杀该任务，
         *   不影响内核和其他任务。
         * ============================================================ */
        int vec = (int)frame->int_no;
        int sig = exc_to_signal(vec);
        u64 cr2_val = 0;

        /* 记录 fault 信息到 current（supervisor 可以通过 SYS_waitpid 读到） */
        if (current != NULL) {
            current->fault_type = vec;
            current->fault_rip  = frame->rip;
            /* #PF 时额外保存 CR2（缺页地址） */
            if (vec == EXC_PF) {
                cr2_val = read_cr2();
                current->fault_addr = cr2_val;
            } else {
                current->fault_addr = 0;
            }
        }

        /* 简洁 USER FAULT 消息 */
        arch_console_set_color(CON_COLOR_YELLOW);
        arch_console_print("[fault] ");
        arch_console_set_color(CON_COLOR_DEFAULT);
        if (current != NULL) {
            arch_console_print(current->name);
        }
        arch_console_print(" #PF -> kill (");
        arch_console_set_color(CON_COLOR_RED);
        arch_console_print(signal_name(sig));
        arch_console_set_color(CON_COLOR_DEFAULT);
        arch_console_print(")\n");

        /* 杀死当前用户任务，系统继续运行 */
        sched_exit_with_code(sig);
        /* sched_exit_with_code 永不返回 */
        return;
    }

    /* ============================================================
     * 内核态 fault：仍然 panic（内核 bug 不可恢复）
     *
     *   保持原有行为：红色横幅 + 寄存器 dump + halt。
     *   内核态的 #PF / #GP / #DF 等都是严重 bug，
     *   唯一正确的做法是停机调试。
     * ============================================================ */
    arch_console_print("\n");
    arch_console_set_color(CON_COLOR_RED);
    arch_console_print("!!! CPU EXCEPTION !!!\n");
    arch_console_set_color(CON_COLOR_DEFAULT);

    arch_console_print("  Exception: ");
    arch_console_set_color(CON_COLOR_YELLOW);
    arch_console_print(exc_name((int)frame->int_no));
    arch_console_set_color(CON_COLOR_DEFAULT);
    arch_console_print("\n");

    arch_console_print("  Vector:    ");
    print_dec_u64(frame->int_no);
    arch_console_print("\n");
    arch_console_print("  Error code: 0x");
    print_hex_u64(frame->err_code);
    arch_console_print("\n");

    /* #PF 特殊处理：解码错误码，打印详细缺页信息 */
    if (frame->int_no == EXC_PF) {
        dump_page_fault(frame);
    }

    dump_frame(frame);

    /* 永久停机：panic 后 arch_halt 不会返回 */
    panic(__FILE__, __LINE__, "unhandled CPU exception in kernel mode");
}
