/* ================================================================
 * arch/x86_64/task.c — 任务栈初始化（架构相关部分）
 *
 * 【Lesson 5 核心新增】
 *
 * 这个文件实现 arch_task_stack_init：
 *   给新任务的栈"伪造"一个"仿佛被切走的老任务"的样子，
 *   这样第一次 arch_context_switch 切到它时，能正确恢复到 task_trampoline。
 *
 * 配合 arch/x86_64/switch.asm 的 arch_context_switch 一起工作。
 *
 * 【栈布局（低地址→高地址）】
 *
 *   arch_context_switch 的 pop 顺序（必须和 push 顺序相反）：
 *     pop r15, pop r14, pop r13, pop r12, pop rbx, pop rbp, popfq, ret
 *
 *   saved_rsp 指向最低地址（r15 那一项），然后逐项往上 pop。
 *
 *   栈顶（高地址）→
 *   ┌──────────────────┐
 *   │  (alignment pad)   │   ← 8 字节占位，让 trampoline 入口 RSP 8 mod 16
 *   ├──────────────────┤
 *   │  task_trampoline   │   ← ret 弹这里作为 RIP（任务入口）
 *   ├──────────────────┤
 *   │  RFLAGS = 0x202    │   ← popfq 弹这里（IF=1）
 *   ├──────────────────┤
 *   │  rbp = 0           │   ← pop rbp
 *   ├──────────────────┤
 *   │  rbx = 0           │   ← pop rbx
 *   ├──────────────────┤
 *   │  r12 = 0           │   ← pop r12
 *   ├──────────────────┤
 *   │  r13 = 0           │   ← pop r13
 *   ├──────────────────┤
 *   │  r14 = 0           │   ← pop r14
 *   ├──────────────────┤
 *   │  r15 = 0           │   ← pop r15（最低地址，saved_rsp 指向这里）
 *   └──────────────────┘
 *      ↑
 *      saved_rsp = stack_top - 72
 *
 *   arch_context_switch 弹栈顺序：
 *     pop r15 (0) → pop r14 (0) → ... → pop rbp (0)
 *     popfq (RFLAGS=0x202, IF=1)
 *     ret (RIP = task_trampoline)
 * ================================================================ */

#include <arch/task.h>
#include <kernel/panic.h>
#include <kernel/types.h>

/* ---------------------------------------------------------------
 * task_trampoline 的外部声明
 *
 * 它在 kernel/sched.c 定义：
 *   void task_trampoline(void) {
 *       arch_sti();
 *       current->entry(current->arg);
 *       sched_exit();   // never returns
 *   }
 *
 * 这里只声明，让 arch_task_stack_init 能取它的地址写到栈上。
 * --------------------------------------------------------------- */
extern void task_trampoline(void);

/* ---------------------------------------------------------------
 * arch_task_stack_init — 初始化新任务的栈
 *
 * 参数：task — 已分配好 stack 的任务
 *
 * 行为：见文件头注释
 *
 * 【关于 RFLAGS = 0x202】
 *   x86-64 RFLAGS 寄存器：
 *     bit 1 (1)    : 保留位，恒为 1
 *     bit 9 (0x200): IF (Interrupt Enable) — 1 = 允许中断
 *     其他位       : 0（不开启任何特殊标志）
 *
 *   0x202 = 0b1000000010 = bit 1 + bit 9 = reserved1 | IF
 *   这是新任务"应该有的"RFLAGS：中断使能，其他默认。
 *
 * 【栈布局（关键）】
 *
 *   arch_context_switch 的 push 顺序：
 *     pushfq, push rbp, push rbx, push r12, push r13, push r14, push r15
 *
 *   push 让 RSP 减小，所以 push 顺序对应的栈布局（低地址→高地址）：
 *     [saved_rsp+0]   = r15      ← 最低地址，pop r15 读这里
 *     [saved_rsp+8]   = r14
 *     [saved_rsp+16]  = r13
 *     [saved_rsp+24]  = r12
 *     [saved_rsp+32]  = rbx
 *     [saved_rsp+40]  = rbp
 *     [saved_rsp+48]  = RFLAGS   ← popfq 读这里
 *     [saved_rsp+56]  = RIP      ← ret 读这里，跳过去
 *
 *   对新任务，我们要在栈上"伪造"这个布局：
 *     - r15..rbp = 0（首次执行没历史值）
 *     - RFLAGS = 0x202（IF=1）
 *     - RIP = task_trampoline（任务入口）
 *
 *   再加 8 字节 alignment pad 在 RIP 之上，让 ret 后 RSP 8 mod 16。
 *
 *   最终栈布局（低地址→高地址）：
 *     [saved_rsp+0]   = r15 = 0          ← saved_rsp 指向这里（最低地址）
 *     [saved_rsp+8]   = r14 = 0
 *     [saved_rsp+16]  = r13 = 0
 *     [saved_rsp+24]  = r12 = 0
 *     [saved_rsp+32]  = rbx = 0
 *     [saved_rsp+40]  = rbp = 0
 *     [saved_rsp+48]  = RFLAGS = 0x202
 *     [saved_rsp+56]  = RIP = task_trampoline
 *     [saved_rsp+64]  = (alignment pad, 不会被读)
 *
 *   数组索引 p[0] 是最低地址，所以：
 *     p[0] = r15, p[1] = r14, ..., p[6] = RFLAGS, p[7] = RIP, p[8] = pad
 *     saved_rsp = &p[0]  ← 关键！指向最低地址
 *
 * 【关于 8 字节 alignment pad】
 *   System V AMD64 ABI 要求函数入口 RSP ≡ 8 (mod 16)。
 *   - ret 弹 8 字节后 RSP = saved_rsp + 64
 *   - 要让 saved_rsp + 64 ≡ 8 (mod 16)
 *   - saved_rsp ≡ 8 - 64 = -56 ≡ 8 (mod 16)
 *
 *   如果 saved_rsp = stack_top - 72，stack_top 是 0 (mod 16)，
 *   72 mod 16 = 8，所以 saved_rsp = -72 mod 16 = 8 ✓
 *
 *   验证：
 *     saved_rsp = stack_top - 72 ≡ 8 (mod 16) ✓
 *     pop 7 个 qword（56 字节）后 RSP = saved_rsp + 56 = stack_top - 16 ≡ 0 (mod 16) ✓
 *     ret 弹 8 字节后 RSP = stack_top - 8 ≡ 8 (mod 16) ✓ （trampoline 入口）
 * --------------------------------------------------------------- */
int arch_task_stack_init(struct task_struct *task) {
    /* 参数校验 */
    if (task == NULL || task->stack == NULL || task->stack_size == 0) {
        return -1;
    }

    /* 栈大小必须是 16 的倍数（保证 stack_top 16 对齐） */
    if ((task->stack_size & 15) != 0) {
        return -1;
    }

    /* stack_top = 栈底 + 大小，指向栈最高地址（栈向下增长） */
    u64 stack_top = (u64)task->stack + task->stack_size;

    /* 在栈上往下放 9 个 qword（72 字节）：
     *   p[0]: r15 = 0     ← saved_rsp 指向这里（最低地址）
     *   p[1]: r14 = 0
     *   p[2]: r13 = 0
     *   p[3]: r12 = 0
     *   p[4]: rbx = 0
     *   p[5]: rbp = 0
     *   p[6]: RFLAGS = 0x202（IF=1）
     *   p[7]: RIP = task_trampoline（ret 弹这里）
     *   p[8]: alignment pad（不会被读） */
    u64 *p = (u64 *)(stack_top - 72);

    p[0] = 0;                                   /* r15 */
    p[1] = 0;                                   /* r14 */
    p[2] = 0;                                   /* r13 */
    p[3] = 0;                                   /* r12 */
    p[4] = 0;                                   /* rbx */
    p[5] = 0;                                   /* rbp */
    p[6] = 0x202ULL;                            /* RFLAGS（IF=1） */
    p[7] = (u64)task_trampoline;                 /* RIP（ret 弹这里） */
    p[8] = 0;                                   /* alignment pad（不会被读） */

    /* saved_rsp 指向 p[0]（最低地址），arch_context_switch 会 mov rsp, [task->saved_rsp]
     * 加载这个值到 RSP，然后 pop r15..rbp + popfq + ret */
    task->saved_rsp = (u64)&p[0];

    return 0;
}
