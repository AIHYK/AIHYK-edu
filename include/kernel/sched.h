/* ================================================================
 * kernel/sched.h — 任务调度器（架构无关接口）
 *
 * 【Lesson 5 核心新增】
 *
 * 这个头文件定义"任务调度"的统一接口：
 *   - sched_init()         — 初始化调度器（创建 init task）
 *   - sched_create_task()  — 创建一个新任务
 *   - sched_yield()        — 主动让出 CPU（voluntary yield）
 *   - sched_tick()         — 时钟中断驱动（被 pit handler 调用，触发抢占）
 *   - sched_exit()         — 任务退出（被 task_trampoline 调用）
 *   - sched_sleep()        — 任务睡眠 N 个 tick
 *   - sched_stats()        — 打印所有任务状态
 *
 * 抽象设计（与 arch/cpu.h / arch/mem.h 一致）：
 *   - "做什么" 在这里声明
 *   - "怎么做" 在 kernel/sched.c 实现
 *   - 架构相关部分（上下文切换）通过 arch/task.h 接口调用
 *
 * =================================================================
 *
 * 【调度器设计：抢占式 + 时间片轮转】
 *
 *   调度策略：Round-Robin with Time Slice
 *
 *   - 每个任务有一个时间片（time_slice，初始为 TIME_SLICE_DEFAULT）
 *   - 每 tick（10ms）sched_tick 递减当前任务的 time_slice
 *   - time_slice 归 0 时触发切换（preemptive preemption）
 *   - 任务也可以主动 sched_yield（voluntary yield）
 *
 *   ┌──────────────────────────────────────────────────────────────┐
 *   │  调度时序示例（3 个任务 A/B/C，时间片 10 ticks）              │
 *   ├──────────────────────────────────────────────────────────────┤
 *   │  Tick  0-9   : A 运行（time_slice 10→1→0）                   │
 *   │  Tick 10     : sched_tick 触发切换 A→B                       │
 *   │  Tick 10-19  : B 运行（time_slice 10→1→0）                   │
 *   │  Tick 20     : sched_tick 触发切换 B→C                       │
 *   │  Tick 20-29  : C 运行（time_slice 10→1→0）                   │
 *   │  Tick 30     : sched_tick 触发切换 C→A（循环）                │
 *   │  ...                                                          │
 *   └──────────────────────────────────────────────────────────────┘
 *
 *   主动让出（sched_yield）和被动抢占（sched_tick 触发）走相同的路径：
 *     都调用 arch_context_switch 切换到下一个 READY 任务。
 *
 * =================================================================
 *
 * 【就绪队列（run queue）设计】
 *
 *   - 用单链表实现，head 指向下一个要运行的任务
 *   - sched_yield 把当前任务（如果仍 RUNNING）加到队尾
 *   - sched_yield 从队头取出下一个任务运行
 *   - 一旦任务 BLOCKED（sleep）或 TERMINATED，不加回队列
 *
 *   ┌─────────┐    ┌─────────┐    ┌─────────┐
 *   │  head   │ →  │ task A  │ →  │ task B  │ → NULL
 *   └─────────┘    └─────────┘    └─────────┘
 *                    ↑ next to run
 *
 *   这是经典的 FIFO round-robin，简单可靠。
 *
 * =================================================================
 *
 * 【init task 的特殊性】
 *
 *   内核启动时正在执行 kernel_main 的"上下文"被当作 init task：
 *     - ID = 0
 *     - 名字 = "init"
 *     - stack = NULL（用的是 entry.asm 的 boot 栈）
 *     - state = TASK_RUNNING
 *     - saved_rsp = 0（第一次 sched_yield 时被 arch_context_switch 写入）
 *
 *   init task 是静态分配的（不通过 kmalloc），生命周期和内核一样长。
 *
 *   kernel_main 在调用 sched_init() 后，可以 sched_create_task 创建其他任务，
 *   然后调用 sched_yield() 让出 CPU 给第一个新任务。
 *   init task 之后会作为"idle task"：没其他任务可运行时回到它，halt 等中断。
 * ================================================================ */

#ifndef KERNEL_SCHED_H
#define KERNEL_SCHED_H

#include <kernel/types.h>
#include <arch/task.h>

/* ---------------------------------------------------------------
 * 默认时间片（tick 数）
 *
 *   100Hz tick → 1 tick = 10ms
 *   10 ticks = 100ms 时间片
 *
 *   【为什么 100ms】
 *     - 太短（如 10ms）：切换开销大，CPU 时间浪费在调度上
 *     - 太长（如 1s）：响应慢，交互体验差
 *     - 100ms 是 Linux 早期 O(1) 调度器的默认值，平衡
 *
 *   【interactive 任务可以 sched_yield 主动让出，不用等时间片用完】 */
#define TIME_SLICE_DEFAULT  10

/* ---------------------------------------------------------------
 * 最大任务数
 *
 *   16 个任务对教学内核够用：
 *     - 1 个 init task（kernel main）
 *     - 几个 demo 任务
 *     - 后续课程可能有 IPC server / 用户进程对应的内核线程
 *
 *   静态数组方便遍历（reaper / stats），不用动态链表。
 * --------------------------------------------------------------- */
#define MAX_TASKS  16

/* ---------------------------------------------------------------
 * current — 当前正在运行的任务
 *
 *   这个全局变量是调度器的核心：
 *     - 调度时 current 切换
 *     - 任何代码可以读 current 拿到"我现在是谁"
 *     - task_trampoline 用 current 找到 entry/arg
 *
 *   单核下 current 是单一全局变量；多核下每个 CPU 一个。
 *   我们单核，简单全局即可。
 *
 *   【volatile】
 *     - 编译器不能缓存 current 到寄存器
 *     - 因为 sched_yield 调用后 current 可能变了
 *     - 实际上 sched_yield 是函数调用，编译器本就会重新读
 *       但 volatile 防御性更稳
 * --------------------------------------------------------------- */
extern struct task_struct *current;

/* ---------------------------------------------------------------
 * sched_init — 初始化调度器
 *
 *   - 静态分配 init task
 *   - current = &init_task
 *   - run_queue 清空
 *   - all_tasks[0] = &init_task
 *
 *   必须在 arch_sti() 之前调用（避免 IRQ 在调度器就绪前触发）。
 *   必须在 kernel_mm_init() 之后调用（创建任务需要 kmalloc）。 */
void sched_init(void);

/* ---------------------------------------------------------------
 * sched_create_task — 创建一个新任务
 *
 * 参数：
 *   entry — 任务入口函数（void (*)(void *)）
 *   arg   — 传给 entry 的参数
 *   name  — 任务名（最长 15 字符，会被复制）
 *
 * 返回值：
 *   >= 0  — 任务 ID
 *   -1    — 失败（任务表满 / kmalloc 失败）
 *
 * 行为：
 *   1. 从 all_tasks 数组找一个 UNUSED 槽位
 *   2. kmalloc(sizeof(task_struct)) + kmalloc(TASK_STACK_SIZE)
 *   3. 填字段 + arch_task_stack_init
 *   4. 标 READY，加入 run_queue
 *
 * 【创建后任务什么时候开始运行】
 *   - 不立即运行，标 READY 加入队列
 *   - 等下次 sched_yield / sched_tick 时被调度
 *
 * 【为什么不在创建时立即切换】
 *   - 创建可能在中断关闭的上下文里
 *   - 立即切换会让调用方"半路切走"，逻辑复杂
 *   - 简单做法：标 READY 加入队列，让调度器决定何时运行 */
s64 sched_create_task(void (*entry)(void *arg), void *arg, const char *name);

/* ---------------------------------------------------------------
 * sched_yield — 主动让出 CPU
 *
 *   1. 把当前任务（如果仍 RUNNING）标 READY，加入 run_queue 末尾
 *   2. 从 run_queue 头取下一个任务
 *   3. 如果没有其他任务，直接返回（继续运行当前）
 *   4. 否则 arch_context_switch 切到下一个
 *
 *   调用方通常这样用：
 *     while (running) {
 *         do_some_work();
 *         sched_yield();  // 让其他任务也能跑
 *     }
 *
 *   也可以在 sched_tick 里被调用（被动抢占走相同路径）。
 *
 * 【IRQ 安全】
 *   内部用 arch_irq_save/restore 保护 run_queue 和 all_tasks，
 *   防止切换过程中被中断（中断处理程序可能也调 sched_*）。 */
void sched_yield(void);

/* ---------------------------------------------------------------
 * sched_tick — 时钟中断驱动调度
 *
 *   由 pit_irq_handler 每 10ms 调用一次。
 *   行为：
 *     1. current->cpu_time_ticks++
 *     2. 唤醒所有 wakeup_tick <= now 的 BLOCKED 任务
 *     3. current->time_slice--
 *     4. 如果 time_slice <= 0，重置 + sched_yield()
 *
 *   【为什么这里要 sched_yield 而不是直接切换】
 *     sched_yield 已经处理了"加回队列 + 取下一个 + context switch"
 *     的完整逻辑，复用即可，不重复造轮子。
 *
 *   【调用上下文】
 *     在 IRQ0 handler 里，IF=0（中断门自动清 IF）。
 *     sched_yield 内部的 arch_irq_save 会保存 IF=0，restore 恢复 IF=0，
 *     最终返回 IRQ handler 时 IF 仍为 0，iretq 才恢复原 IF。 */
void sched_tick(void);

/* ---------------------------------------------------------------
 * sched_exit — 任务退出
 *
 *   - 标 current->state = TERMINATED
 *   - 调 sched_yield 切换到下一个任务
 *   - 永远不返回（切换走后，stack 仍被占用，等清理）
 *
 *   【为什么不在 sched_exit 里 free 自己的栈】
 *     sched_exit 在 current 的栈上运行（用的是 current->stack）。
 *     如果 kfree(current->stack)，下面还在用这个栈 → 崩溃。
 *     解决：标 TERMINATED，让其他任务在 sched_yield / sched_create 时
 *     看到 TERMINATED 任务就 reaper（清理）。 */
void sched_exit(void);

/* ---------------------------------------------------------------
 * sched_sleep — 当前任务睡眠 ticks 个 tick
 *
 * 参数：ticks — 睡眠时长（tick 数，10ms 单位）
 *
 *   1. 标 current->state = BLOCKED
 *   2. 设 current->wakeup_tick = now + ticks
 *   3. sched_yield 切换走
 *   4. 当 sched_tick 检查到 wakeup_tick <= now 时，把任务回到 READY
 *   5. 任务被调度时，从 sched_yield 返回，sched_sleep 也返回
 *
 *   【和 busy-wait 的区别】
 *     busy-wait: while (time_not_reached) {}  → 浪费 CPU
 *     sleep: 切换走，让其他任务跑 → CPU 不浪费
 *
 *   【精度】
 *     100Hz tick → 精度 10ms。sleep(1) 实际睡眠 0~10ms 之间。 */
void sched_sleep(u64 ticks);

/* ---------------------------------------------------------------
 * sched_wake — 唤醒一个阻塞的任务（IPC / 同步原语用）
 *
 * 参数：t — 要唤醒的任务（必须处于 BLOCKED 状态）
 *
 * 行为：
 *   1. 关中断
 *   2. 把 t->state 置 READY
 *   3. 重置 t->time_slice
 *   4. 加入 run_queue
 *
 * 【Lesson 6 核心新增】
 *   ipc_send / ipc_recv 需要这个函数：
 *     - 接收方在 channel 等消息时 BLOCKED，发送方来消息后 sched_wake 它
 *     - 发送方在 channel 满 BLOCKED，接收方拿走一条后 sched_wake 它
 *
 *   和 sched_sleep 的 wakeup 是对称的：
 *     - sleep 的 wakeup 由 sched_tick 时钟驱动
 *     - IPC 的 wakeup 由对端 send/recv 驱动
 *
 * 【为什么 t->wait_channel / wait_kind 不在这里清】
 *   这些字段是 IPC 状态，由 ipc.c 在"摘队列"时清。
 *   sched_wake 只关心把任务标 READY + 加 run_queue，
 *   不需要懂 IPC 数据结构。
 *
 * 【调用上下文】
 *   通常在 ipc_send / ipc_recv 内部、关中断的临界区里调用。
 *   内部 arch_irq_save/restore 保证 run_queue 操作原子。 */
void sched_wake(struct task_struct *t);

/* ---------------------------------------------------------------
 * sched_num_tasks — 返回当前任务总数（含 init task）
 *
 *   用于 init task 等待所有 demo 任务退出：
 *     while (sched_num_tasks() > 1) sched_sleep(10);
 *
 *   任务调用 sched_exit 后会被 task_reaper 清理，num_tasks 减少。
 *   当 num_tasks == 1 时只剩 init task，可以认为所有 demo 任务都退出了。
 *
 * 【Lesson 6 新增】
 *   init task 需要等所有 demo 任务退出后才打印"All done"并清理 channel。
 *   但 demo 任务会阻塞在 ipc_recv（state=BLOCKED，不在 run_queue），
 *   所以 init 的 sched_yield 可能在 demo 任务还活着时就被弹回。
 *   用 num_tasks > 1 作为"还有任务活着"的判据。 */
int sched_num_tasks(void);

/* ---------------------------------------------------------------
 * sched_stats — 打印所有任务状态
 *
 *   遍历 all_tasks，打印 ID / name / state / cpu_time / stack 地址
 *   调试用，看任务有没有卡住、谁占 CPU 多。 */
void sched_stats(void);

/* ---------------------------------------------------------------
 * task_reaper — 清理已终止的任务
 *
 *   遍历 all_tasks，把 TERMINATED 任务的 stack 和 task_struct kfree，
 *   把 all_tasks 槽位标 UNUSED。
 *
 *   【何时调用】
 *     - sched_yield 里（每次切换时清理一下）
 *     - sched_create_task 里（创建新任务前清理）
 *
 *   【为什么不能在 sched_exit 里直接 free】
 *     见 sched_exit 的注释。 */
void task_reaper(void);

/* ================================================================
 * 【Lesson 7 新增】任务访问器
 *
 *   cap.c 的 cap_revoke / cap_destroy_channel 需要遍历所有任务的
 *   CSpace。但 cap.c 不能直接访问 sched.c 的 all_tasks 数组（私有）。
 *
 *   暴露两个访问器：
 *     sched_get_task_by_index(i) — 按 all_tasks 顺序取任务（遍历用）
 *     sched_get_task_by_id(id)   — 按 task_id 找任务
 *
 *   返回 NULL 表示越界或任务不存在。
 *
 *   【为什么不直接把 all_tasks 设成全局】
 *     - 暴露内部数据结构会让外部代码依赖具体实现
 *     - 接口函数隔离内部 layout，将来改成链表/哈希表都不影响调用方
 * ================================================================ */
struct task_struct *sched_get_task_by_index(int index);
struct task_struct *sched_get_task_by_id(u64 task_id);

/* ================================================================
 * 【Lesson 9 新增】带退出码退出
 *
 *   sched_exit_with_code — 设置 exit_code 后退出任务
 *
 *   参数：code — 退出码（0=正常，<0=fault 信号，>0=用户自定义）
 *
 *   异常处理器用它杀死 fault 的用户任务：
 *     arch_exception_handler → sched_exit_with_code(TASK_SIG_SEGV)
 *   正常 sys_exit 也走这个路径：
 *     syscall_handler → sched_exit_with_code(user_code)
 *
 *   【为什么需要单独函数而不只改 sched_exit 签名】
 *     sched_exit 已在很多地方被调用（task_trampoline, syscall_handler 等），
 *     改签名要改所有调用点。新增一个 _with_code 变体更安全，
 *     sched_exit 本身变成 "sched_exit_with_code(TASK_EXIT_NORMAL)" 的简写。
 * ================================================================ */
void sched_exit_with_code(int code);

/* ================================================================
 * 【Lesson 8 新增】用户态任务（ring 3）
 *
 *   sched_create_user_task — 创建一个在 ring 3 运行的用户任务
 *
 *   参数：
 *     image     — 用户程序二进制（flat bin，入口在偏移 0）
 *     image_len — 二进制长度（字节）
 *     name      — 任务名
 *
 *   返回值：
 *     >= 0  — 任务 ID
 *     -1    — 失败（任务表满 / 内存不足 / 用户页映射失败）
 *
 *   行为：
 *     1. 选一个 user VMA 槽位（每个 user 任务独立地址，避免覆盖）
 *     2. sched_create_task(user_task_main, t, name) 创建内核任务
 *        （内核侧：cspace + 内核栈 + task_struct）
 *     3. 映射 user 代码页 + 栈页（U/S=1）
 *     4. 把 image 字节拷进代码页
 *     5. 设 t->is_user=1, user_rip=user_code_vma, user_rsp=user_stack_top
 *     6. 标 READY 加入 run_queue
 *
 *   【任务怎么开始跑】
 *     sched_yield 切到它 → task_trampoline → user_task_main
 *     → arch_enter_user_mode(user_rip, user_rsp, 0) → iretq → ring 3
 *     user 代码跑起来，用 int 0x80 发 syscall。
 *
 *   【user 怎么退出】
 *     user 代码调 sys_exit → syscall_handler → sched_exit → TERMINATED
 *     → task_reaper 清理（含 user 页 unmap + free）。
 * ================================================================ */
s64 sched_create_user_task(const void *image, u64 image_len, const char *name);

#endif /* KERNEL_SCHED_H */
