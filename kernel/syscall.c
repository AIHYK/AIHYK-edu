/* ================================================================
 * kernel/syscall.c — System Call 分发实现
 *
 * 【Lesson 8 核心新增】
 *
 * 实现 <kernel/syscall.h> 的 syscall_handler。
 *
 * 调用链：
 *   user 代码 → int 0x80
 *     → CPU 查 IDT[0x80]（陷阱门，DPL=3，selector=0x08）
 *     → 切到 ring 0，从 TSS.sp0 取栈，压 SS:RSP:RFLAGS:CS:RIP
 *     → 跳到 isr128 stub（isr.asm 自动生成）
 *     → stub push 0(占位) + 0x80(向量) → jmp isr_common
 *     → isr_common push 15 GPRs → call arch_irq_dispatch(frame)
 *     → arch_irq_dispatch 见 vec==0x80 → 调 syscall_handler(frame)
 *     → syscall_handler 读 frame->rax(号) / rdi,rsi,rdx(参数)
 *     → 处理，返回值写 frame->rax
 *     → isr_common pop GPRs（含新 rax）→ iretq → 回 ring 3
 *     → user 看到 rax = 返回值
 *
 * 【设计原则】
 *   - 每个 syscall 短小（微秒级），运行时 IF=0（中断门），不会被打断
 *   - SYS_yield 例外：它调 sched_yield 切走，永不"返回"本调用栈——
 *     切回来时从 arch_context_switch 后继续，iretq 回 user，user 看到 rax=0
 *   - SYS_exit 调 sched_exit，永不返回（user 不会再执行）
 *
 * 【安全性】
 *   user 传来的指针（如 SYS_write 的 buf）是 user 虚拟地址。
 *   共享地址空间设计下，内核可直接读（user 页 U=1，内核 ring 0 也能访问）。
 *   但内核应校验指针在 user 区间内，防止 user 传内核指针让内核帮忙读内核数据——
 *   教学简化：暂不校验（user 页 U=1 但内核页 U=0，user 根本拿不到内核指针内容）。
 *   （user 伪造一个内核地址传给 SYS_write：内核会读那个地址——但 user 也不知道
 *    内核里有什么，最多泄露内核内存。产品级要 copy_from_user + 范围检查。）
 * ================================================================ */

#include <arch/console.h>
#include <arch/pit.h>
#include <kernel/panic.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>
#include <kernel/types.h>
#include <kernel/util.h>

/* ---------------------------------------------------------------
 * syscall_handler — syscall 分发
 *
 *   frame 包含 user 进入 int 0x80 时的寄存器快照：
 *     frame->rax  = syscall 号
 *     frame->rdi  = arg0
 *     frame->rsi  = arg1
 *     frame->rdx  = arg2
 *     frame->rip  = user 返回地址（iretq 回去的地方）
 *     frame->cs   = 0x23（user CS | RPL3）
 *
 *   返回值写 frame->rax，iretq 时恢复到 user 的 rax。
 * --------------------------------------------------------------- */
void syscall_handler(struct interrupt_frame *frame) {
    if (frame == NULL) {
        return;
    }

    u64 num = frame->rax;
    u64 a0  = frame->rdi;
    u64 a1  = frame->rsi;
    u64 a2  = frame->rdx;
    (void)a2;   /* 当前 syscall 最多用 2 参数，保留接口 */

    switch (num) {

    /* -------------------------------------------------------
     * SYS_write(fd, buf, len) — 写字符串到控制台
     *
     *   fd 被忽略（只有一个控制台）
     *   buf 是 user 虚拟地址，内核可直接读
     *   len 是字节数
     *   返回写入字节数
     * ------------------------------------------------------- */
    case SYS_write: {
        const char *buf = (const char *)a0;
        u64 len = a1;

        /* 【C1 修复】用户指针长度上限校验
         *
         *   原实现直接 `for (i = 0; i < len; i++) putchar(buf[i])`，
         *   用户传 len=2^60 + buf 近页边界 → 内核走过页尾 → #PF → panic
         *   → 整个内核 halt。任何用户任务可 DoS 内核。
         *
         *   教学简化：单次 write 上限 4KB（够 hello-world / 串口日志用）。
         *   超过返回 -1，让用户态自己分片。完整 copy_from_user + access_ok
         *   留给 pro 版（需 per-process 页表，edu 是共享地址空间）。
         *
         *   选 4KB 理由：1 页大小，VGA 文本屏 80×25×2=4000 字节刚好装下，
         *   教学示例（hello.asm / crash.asm）都在 100 字节内。 */
        if (len > 4096) {
            frame->rax = (u64)(-1);
            break;
        }

        for (u64 i = 0; i < len; i++) {
            arch_console_putchar(buf[i]);
        }
        frame->rax = len;
        break;
    }

    /* -------------------------------------------------------
     * SYS_yield() — 主动让出 CPU
     *
     *   调 sched_yield 切到其他任务，切回来后返回 0。
     *   sched_yield 内部 arch_irq_save/restore，IF 状态正确恢复。
     * ------------------------------------------------------- */
    case SYS_yield:
        sched_yield();
        frame->rax = 0;
        break;

    /* -------------------------------------------------------
     * SYS_exit(code) — 任务退出，永不返回
     *
     *   标 current TERMINATED，sched_yield 切走。
     *   syscall_handler 不返回（切走后本调用栈废弃）。
     *   【Lesson 9】用 sched_exit_with_code 传入用户退出码，
     *   让 SYS_waitpid 能读到退出状态。
     * ------------------------------------------------------- */
    case SYS_exit: {
        int exit_code = (int)(s64)a0;  /* 用户退出码（可能负数） */
        /* 安静退出，不打印（L10 清洁输出） */
        sched_exit_with_code(exit_code);
        /* 永不返回 */
        break;
    }

    /* -------------------------------------------------------
     * SYS_getpid() — 返回当前任务 ID
     * ------------------------------------------------------- */
    case SYS_getpid:
        frame->rax = current->task_id;
        break;

    /* -------------------------------------------------------
     * SYS_gettick() — 返回内核 tick 计数（10ms 单位）
     * ------------------------------------------------------- */
    case SYS_gettick:
        frame->rax = arch_pit_get_tick_count();
        break;

    /* -------------------------------------------------------
     * SYS_putchar(c) — 输出单个字符
     * ------------------------------------------------------- */
    case SYS_putchar:
        arch_console_putchar((char)(a0 & 0xFF));
        frame->rax = a0 & 0xFF;
        break;

    /* -------------------------------------------------------
     * SYS_waitpid(child_id) — 等待子任务退出（Lesson 9 核心新增）
     *
     *   参数：rdi = child task_id
     *   返回值：
     *     - 子任务的 exit_code（子已终止）
     *     - -1（子任务不存在 / 已被 reaper 清理）
     *
     *   实现：简单轮询 + yield。
     *   如果子任务还在运行，yield 让出 CPU 等下次再查。
     *   如果子任务已 TERMINATED，返回 exit_code，
     *     并让 reaper 在下次 sched_yield 时清理它。
     *
     *   【为什么是轮询而不是阻塞等待】
     *     教学内核简化：阻塞等待需要 per-child wait queue +
     *     parent 挂起 + child 退出时唤醒 parent。
     *     轮询 + yield 足够用（子任务会很快退出/崩溃）。
     *     产品级内核用 wait queue + SIGCHLD。
     *
     *   【和 Unix waitpid 的区别】
     *     Unix waitpid 挂起调用方直到子退出，
     *     我们用轮询（sched_yield + retry），语义略有不同
     *     但最终效果一样：调用方在子退出后拿到退出码。
     *     而且轮询期间其他任务可以跑（yield 让出 CPU）。 */
    case SYS_waitpid: {
        u64 child_id = a0;
        struct task_struct *child = sched_get_task_by_id(child_id);
        if (child == NULL) {
            /* 子任务不存在（可能已被 reaper 清理，或 ID 无效） */
            frame->rax = (u64)(-1);   /* -1 表示不存在 */
            break;
        }
        if (child->state == TASK_TERMINATED) {
            /* 子任务已终止：返回 exit_code */
            frame->rax = (u64)(s64)child->exit_code;
            /* 【Lesson 9】读完退出码后允许 reaper 清理 zombie。
             *   设 parent_task_id = 0，下次 reaper 回收。 */
            child->parent_task_id = 0;
            break;
        }
        /* 子任务还在运行/就绪/阻塞：yield 让出 CPU，下次再查。
         *   用户态循环调用 SYS_waitpid 会形成轮询，
         *   每次轮询 yield 一次，其他任务有机会跑。 */
        sched_yield();
        /* yield 回来后：再次查子任务状态（重新走 dispatch 不方便，
         *   直接返回一个特殊值让用户态知道"还没退出，请重试"）。
         *   用 -2 表示"子任务仍在运行，请重试"。 */
        child = sched_get_task_by_id(child_id);
        if (child == NULL) {
            frame->rax = (u64)(-1);   /* 子任务已被 reaper 清理 */
        } else if (child->state == TASK_TERMINATED) {
            frame->rax = (u64)(s64)child->exit_code;
            /* 【Lesson 9】读完退出码后允许 reaper 清理 zombie */
            child->parent_task_id = 0;
        } else {
            frame->rax = (u64)(-2);   /* -2 = still running, retry */
        }
        break;
    }

    default:
        /* 非法 syscall 号 */
        arch_console_set_color(CON_COLOR_YELLOW);
        arch_console_print("[kernel] unknown syscall #");
        kprint_dec(num);
        arch_console_print(" from task ");
        kprint_dec(current->task_id);
        arch_console_print("\n");
        arch_console_set_color(CON_COLOR_DEFAULT);
        frame->rax = (u64)-1;   /* -1 表示错误 */
        break;
    }
}
