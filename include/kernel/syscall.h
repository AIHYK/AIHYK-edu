/* ================================================================
 * kernel/syscall.h — System Call 接口（架构无关）
 *
 * 【Lesson 8 核心新增】
 *
 * 系统调用是用户态（ring 3）请求内核服务的【唯一】合法通道：
 *   user 程序 → int 0x80 → 内核 syscall_handler → 返回 user
 *
 * 这是"用户态 / 内核态"边界的核心机制：
 *   - user 代码不能直接调用内核函数（不同特权级 + 页表 U 位保护）
 *   - user 只能通过 int 0x80 软中断陷入内核，内核检查参数后提供服务
 *   - 这让内核能"控制" user 能做什么（只能调已注册的 syscall 号）
 *
 * =================================================================
 *
 * 【syscall ABI（AIHYK 自定义，类 Linux 风格）】
 *
 *   user 代码这样发起 syscall：
 *     mov rax, <syscall_num>
 *     mov rdi, <arg0>        ; 第 1 参数
 *     mov rsi, <arg1>        ; 第 2 参数
 *     mov rdx, <arg2>        ; 第 3 参数
 *     int 0x80
 *     ; 返回值在 rax
 *
 *   最多 3 个参数（够教学用）。需要更多可加 r10/r8。
 *
 *   内核 syscall_handler 从中断栈帧 frame 读取这些寄存器：
 *     num = frame->rax
 *     a0  = frame->rdi
 *     a1  = frame->rsi
 *     a2  = frame->rdx
 *   处理后把返回值写回 frame->rax，iretq 时恢复到 user 的 rax。
 *
 * 【为什么用 int 0x80 而不是 syscall 指令】
 *   - syscall/sysret 速度更快（不需查 IDT），但要配 MSR（STAR/LSTAR/MASK）
 *     + GDT 里的特定布局，复杂度高
 *   - int 0x80 是经典做法（Linux 2.x 用了很久），教学清晰
 *   - IDT[0x80] = 陷阱门 / 中断门 + DPL=3，让 ring 3 能 invoke
 *   - 后续优化课程可换 syscall 指令，syscall.h 接口不变
 *
 * 【为什么返回值写 frame->rax 而不是 return】
 *   - syscall_handler 返回后，isr_common 会 pop 所有通用寄存器（含 rax）
 *   - 直接 return 的话，rax 被 pop 成"进入时的 user rax"，user 看不到结果
 *   - 所以必须把返回值写到 frame->rax，让 pop 恢复时 user rax = 结果
 * ================================================================ */

#ifndef KERNEL_SYSCALL_H
#define KERNEL_SYSCALL_H

#include <kernel/types.h>
#include <arch/interrupts.h>   /* struct interrupt_frame */

/* ---------------------------------------------------------------
 * syscall 号码表
 *
 *   每个 syscall 一个号码，user 用 mov rax, SYS_xxx 指定。
 *   号码 0 保留（不用），从 1 开始。
 *   添加新 syscall：在下面加 #define，并在 syscall.c 的 dispatch 里加 case。
 * --------------------------------------------------------------- */
#define SYS_write     1   /* 写字符串到控制台: rdi=buf, rsi=len  → 写入字节数 */
#define SYS_yield     2   /* 主动让出 CPU（sched_yield）          → 0 */
#define SYS_exit      3   /* 任务退出（永不返回）: rdi=exit_code   → 不返回 */
#define SYS_getpid    4   /* 获取当前任务 ID                       → task_id */
#define SYS_gettick   5   /* 获取内核 tick 计数（10ms 单位）       → tick_count */
#define SYS_putchar   6   /* 输出单个字符: rdi=char               → char */
#define SYS_waitpid   7   /* 等待子任务退出: rdi=child_task_id    → exit_code or -1 */

/* 最大 syscall 号（防御性，超过视为非法） */
#define SYS_MAX       7

/* ---------------------------------------------------------------
 * syscall_handler — syscall 分发函数
 *
 *   由 arch_irq_dispatch 在 vec == 0x80 时调用。
 *   参数 frame 包含 user 进入 syscall 时的所有寄存器快照。
 *
 *   行为：
 *     1. 读 frame->rax = syscall 号
 *     2. 读 frame->rdi/rsi/rdx = 参数
 *     3. 按 syscall 号分发
 *     4. 把返回值写到 frame->rax
 *
 *   返回值：无（返回值通过 frame->rax 传回 user）
 *
 * 【user 传来的指针安全吗】
 *   SYS_write 传来的 buf 指针是 user 虚拟地址。因为 user 代码页 / 栈页
 *   在内核地址空间里也映射了（共享地址空间设计），内核可以直接读。
 *
 *   【C1 修复】单次长度上限 4KB（见 syscall.c SYS_write 实现），
 *     防止用户传 len=2^60 让内核走过页尾触发 #PF → panic → 内核 DoS。
 *     完整 copy_from_user + access_ok 范围校验需要 per-process 页表，
 *     属于 pro 版工作（edu 是共享地址空间，无法隔离用户/内核指针）。
 *   （页表 U 位已保证 user 不能伪造内核指针——user 根本访问不到内核页。）
 * --------------------------------------------------------------- */
void syscall_handler(struct interrupt_frame *frame);

#endif /* KERNEL_SYSCALL_H */
