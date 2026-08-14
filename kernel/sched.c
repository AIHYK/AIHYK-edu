/* ================================================================
 * kernel/sched.c — 任务调度器实现（抢占式 + 时间片轮转）
 *
 * 【Lesson 5 核心新增】
 *
 * 实现 include/kernel/sched.h 的接口。
 *
 * 调度器的核心数据结构：
 *
 *   ┌──────────────────────────────────────────────────────────┐
 *   │  all_tasks[MAX_TASKS]      所有任务的数组（含 init）       │
 *   │     [0] = init_task (静态)                              │
 *   │     [1..N] = 动态创建的任务                              │
 *   │                                                          │
 *   │  run_queue_head           就绪队列头（READY 任务链表）    │
 *   │  run_queue_tail           就绪队列尾（方便 O(1) 入队）     │
 *   │                                                          │
 *   │  current                  当前运行的任务                  │
 *   └──────────────────────────────────────────────────────────┘
 *
 * 调度流程（sched_yield）：
 *
 *   ┌──────────────────────────────────────────────────────────┐
 *   │  1. 关中断（保护 run_queue）                              │
 *   │  2. task_reaper() 清理 TERMINATED 任务                    │
 *   │  3. 唤醒所有 sleep 到期的任务                             │
 *   │  4. 当前任务若仍 RUNNING：标 READY，加到 run_queue 尾      │
 *   │  5. 从 run_queue 头取 next                                │
 *   │  6. next == NULL ? 恢复当前任务，开中断，返回             │
 *   │  7. next->state = RUNNING                                 │
 *   │  8. current = next                                        │
 *   │  9. arch_context_switch(prev, next) ← 真正切换             │
 *   │ 10. (这里 prev 已经被切回来) 恢复中断状态，返回            │
 *   └──────────────────────────────────────────────────────────┘
 *
 * 任务生命周期（state 转换）：
 *
 *   UNUSED ──sched_create_task──→ READY ──sched_yield──→ RUNNING
 *                                       ←───sched_yield────┘
 *                                          │
 *                                          ├──sched_sleep──→ BLOCKED
 *                                          │                  │
 *                                          │                  └──sched_tick
 *                                          │                     (wakeup_tick到期)
 *                                          │                     ↓
 *                                          │                     READY
 *                                          │
 *                                          └──sched_exit──→ TERMINATED
 *                                                              │
 *                                                              └──task_reaper
 *                                                                 → UNUSED
 *
 * ================================================================ */

#include <arch/console.h>
#include <arch/cpu.h>
#include <arch/mem.h>
#include <arch/pit.h>
#include <arch/task.h>
#include <arch/user.h>
#include <kernel/cap.h>
#include <kernel/ipc.h>
#include <kernel/mm.h>
#include <kernel/panic.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>
#include <kernel/types.h>
#include <kernel/util.h>

/* ---------------------------------------------------------------
 * 全局状态
 * --------------------------------------------------------------- */

/* 当前运行的任务（init task 静态分配，初始指向它） */
struct task_struct *current = NULL;

/* 就绪队列（单链表，FIFO） */
static struct task_struct *run_queue_head = NULL;
static struct task_struct *run_queue_tail = NULL;

/* 所有任务的数组（用于遍历 / stats / reaper） */
static struct task_struct *all_tasks[MAX_TASKS];

/* 已用任务数（含 init task） */
static int num_tasks = 0;

/* 下一个任务的 ID（递增，不复用） */
static u64 next_task_id = 1;

/* 调度器是否已初始化（防御性，未 init 时 sched_* 报警） */
static int sched_initialized = 0;

/* 【Lesson 8】boot 栈顶（entry.asm 导出），用于 init task 的 kstack_top */
extern u8 stack_top_64[];

/* ---------------------------------------------------------------
 * init_task — 内核启动任务（静态分配）
 *
 *   内核进入 kernel_main 时，"当前执行流"被当作 init task：
 *     - 用 entry.asm 的 boot 栈（不通过 kmalloc）
 *     - 第一次 sched_yield 时 arch_context_switch 会写入 saved_rsp
 *     - entry/arg 为 NULL（init 不需要 entry，它已经在跑 kernel_main）
 *
 *   【为什么静态分配】
 *     - init task 必须在 sched_init() 时就存在
 *     - sched_init 在 kmalloc 可用之前可能被调用（实际我们后才初始化，但防御）
 *     - 静态分配避免依赖 kmalloc，最稳
 *
 *   【为什么 next 字段 = NULL】
 *     init task 当前不在 run_queue 里（它在 RUNNING），
 *     sched_yield 时会先把 RUNNING 任务加到队尾。
 * --------------------------------------------------------------- */
static struct task_struct init_task = {
    .saved_rsp       = 0,            /* 第一次 sched_yield 时被写入 */
    .state           = TASK_RUNNING,
    .task_id         = 0,
    .name            = "init",
    .entry           = NULL,
    .arg             = NULL,
    .stack           = NULL,         /* 用 boot 栈，不通过 kmalloc */
    .stack_size      = 0,
    .cpu_time_ticks  = 0,
    .wakeup_tick     = 0,
    .time_slice      = TIME_SLICE_DEFAULT,
    .next            = NULL,

    /* Lesson 6: IPC 等待状态全清零（init task 一开始不在等任何 channel） */
    .next_waiter     = NULL,
    .wait_channel    = NULL,
    .wait_kind       = 0,
    .ipc_buf         = NULL,
    .ipc_buf_cap     = 0,
    .ipc_out_type    = 0,
    .ipc_out_sender  = 0,
    .ipc_out_len     = 0,
    .ipc_result      = 0,
    .ipc_timeout_tick = 0,

    /* Lesson 7: Capability 字段（init task 不持有任何 cap，
     * 它是 demo orchestrator，由 cap.c 直接操作其 cspace） */
    .cspace                  = NULL,
    .ipc_recv_cap_has_cap    = 0,
    .ipc_recv_cap_type       = 0,
    .ipc_recv_cap_rights     = 0,
    .ipc_recv_cap_object     = NULL,
    .ipc_recv_cap_lineage    = 0,

    /* Lesson 8: 用户态字段。
     *   init task 是 ring 0 内核任务，is_user=0。
     *   kstack_top 用 boot 栈顶（stack_top_64，entry.asm 导出）。
     *   sched_init 会显式再设一次（防止 stack_top_64 符号解析时机问题）。 */
    .is_user                 = 0,
    .kstack_top              = 0,    /* sched_init 里设成 (u64)stack_top_64 */
    .user_rip                = 0,
    .user_rsp                = 0,
    .user_code_vma           = 0,
    .user_code_pages         = 0,
    .user_stack_vma          = 0,
};

/* ---------------------------------------------------------------
 * 局部工具函数
 * --------------------------------------------------------------- */

/* 把 name 复制到 task->name（最多 TASK_NAME_MAX-1 字符 + \0） */
static void copy_name(char *dst, const char *src) {
    int i;
    for (i = 0; i < TASK_NAME_MAX - 1 && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/* 就绪队列入队（加到队尾） */
static void run_queue_push(struct task_struct *t) {
    t->next = NULL;
    if (run_queue_tail != NULL) {
        run_queue_tail->next = t;
    } else {
        run_queue_head = t;
    }
    run_queue_tail = t;
}

/* 就绪队列出队（从队头取） */
static struct task_struct *run_queue_pop(void) {
    struct task_struct *t = run_queue_head;
    if (t != NULL) {
        run_queue_head = t->next;
        if (run_queue_head == NULL) {
            run_queue_tail = NULL;
        }
        t->next = NULL;
    }
    return t;
}

/* ---------------------------------------------------------------
 * task_reaper — 清理已终止的任务
 *
 *   遍历 all_tasks，对 TERMINATED 任务：
 *     1. kfree(task->stack)（如果不是 init task）
 *     2. kfree(task)（如果不是 init task）
 *     3. all_tasks[i] = NULL（让槽位可复用）
 *
 *   【为什么 init task 不清理】
 *     init task 用的是 boot 栈（stack=NULL），不是 kmalloc 出来的。
 *     而且 init task 永不终止（kernel_main 是死循环）。
 *
 *   【为什么不能在 sched_exit 里直接清理】
 *     sched_exit 在 task->stack 上运行，
 *     kfree(task->stack) 会让后续指令访问无效内存 → 崩溃。
 *     标 TERMINATED 后切换走，等其他任务来清理才安全。
 * --------------------------------------------------------------- */
void task_reaper(void) {
    for (int i = 0; i < num_tasks; i++) {
        struct task_struct *t = all_tasks[i];
        if (t == NULL) continue;

        if (t->state == TASK_TERMINATED) {
            /* 不能清理 init task（它的 stack 是静态的） */
            if (t == &init_task) continue;

            /* 【Lesson 6 修复】不能清理 current task
             *   current 在自己的栈上调用 sched_exit → sched_yield → task_reaper。
             *   如果这里 kfree(current->stack)，下面 sched_yield 还在用这个栈
             *   → use-after-free → 崩溃。
             *   标 TERMINATED 让其他任务的 task_reaper 来清理。 */
            if (t == current) continue;

            /* 【Lesson 9】Zombie 任务：有 parent_task_id 的子任务
             *   在 TERMINATED 后不立即清理，等父任务读完 exit_code。
             *   父任务读完后设 parent_task_id = 0，下次 reaper 才清理。
             *   这类似 Unix 的 zombie 进程：exit 后不消失，等 wait()。
             *   没有 parent 的任务（parent_task_id == 0）立即清理。 */
            if (t->parent_task_id != 0) continue;

            /* 【Lesson 7】先释放 CSpace（cap 表）。
             *   注意：cap_cspace_destroy 不销毁 underlying channel，
             *   channel 由 cap_destroy_channel 显式销毁。 */
            cap_cspace_destroy(t);

            /* 【Lesson 8】如果是 user 任务，先 unmap + free user 页
             *   （代码页 + 栈页）。这些页 U/S=1，必须显式回收，
             *   否则 PMM 永久泄漏这些帧。
             *   注意：此时 t->user_code_vma 等字段仍有效（task_struct 还没 free）。 */
            if (t->is_user) {
                if (t->user_code_pages > 0) {
                    arch_user_unmap_pages(t->user_code_vma, t->user_code_pages);
                }
                if (t->user_stack_vma != 0) {
                    arch_user_unmap_pages(t->user_stack_vma, USER_STACK_PAGES);
                }
            }

            /* 释放栈和 task_struct */
            if (t->stack != NULL) {
                kfree(t->stack);
            }
            all_tasks[i] = NULL;

            /* 压缩数组：把后面的元素前移（保持 all_tasks 紧凑） */
            for (int j = i; j < num_tasks - 1; j++) {
                all_tasks[j] = all_tasks[j + 1];
            }
            all_tasks[num_tasks - 1] = NULL;
            num_tasks--;

            /* 因为前移了，i 需要重新检查这个位置的新元素 */
            i--;

            kfree(t);
        }
    }
}

/* ---------------------------------------------------------------
 * wake_sleeping_tasks — 唤醒睡眠到期的任务 / IPC 超时任务
 *
 *   遍历 all_tasks，对 BLOCKED 任务检查两种超时：
 *     (a) sched_sleep 设置的 wakeup_tick（now >= wakeup_tick）
 *     (b) ipc_send / ipc_recv 设置的 ipc_timeout_tick（now >= ipc_timeout_tick）
 *
 *   两者都把任务从 BLOCKED 唤醒回 READY：
 *     - case (a) 是正常 sleep 到期
 *     - case (b) 是 IPC 超时，需要把任务从 channel 的等待队列摘下来，
 *       并设 ipc_result = -IPC_ERR_TIMEDOUT，ipc_recv / ipc_send 返回时
 *       读这个字段告诉调用方"超时了"
 *
 *   【为什么在 sched_yield 里调用】
 *     sched_yield 是切换的入口，刚好可以顺便检查唤醒。
 *     sched_tick 也调用 sched_yield，所以 tick 也会触发唤醒。
 *
 *   【IPC 超时唤醒的"摘队列"工作由 ipc.c 完成】
 *     这里只负责把任务标 READY 并加回 run_queue。
 *     当 ipc_send / ipc_recv 被超时唤醒后，会检查自己是否还在 channel 的
 *     等待队列里，如果是就把自己摘下来（用 ipc_unlink_waiter）。
 *     这个"延迟摘队列"避免 wake 函数需要知道 channel 数据结构。
 * --------------------------------------------------------------- */
static void wake_sleeping_tasks(void) {
    u64 now = arch_pit_get_tick_count();

    for (int i = 0; i < num_tasks; i++) {
        struct task_struct *t = all_tasks[i];
        if (t == NULL) continue;
        if (t->state != TASK_BLOCKED) continue;

        /* (a) sched_sleep 到期（task 不在 IPC 等待，wakeup_tick 是睡眠时长） */
        if (t->wait_kind == 0 && t->wakeup_tick != 0 && now >= t->wakeup_tick) {
            t->state = TASK_READY;
            t->time_slice = TIME_SLICE_DEFAULT;
            t->wakeup_tick = 0;
            run_queue_push(t);
            continue;
        }

        /* (b) ipc_send / ipc_recv 超时（ipc_timeout_tick != 0 表示有限等待） */
        if (t->wait_kind != 0 && t->ipc_timeout_tick != 0
                && now >= t->ipc_timeout_tick) {
            t->ipc_result = IPC_ERR_TIMEDOUT;   /* -3 */
            /* 注意：不在这里摘 channel 等待队列，留给被唤醒的 ipc_send/recv
             *       自己处理（它知道自己在哪个 channel 上） */
            t->state = TASK_READY;
            t->time_slice = TIME_SLICE_DEFAULT;
            t->ipc_timeout_tick = 0;
            run_queue_push(t);
        }
    }
}

/* ---------------------------------------------------------------
 * sched_init — 初始化调度器
 * --------------------------------------------------------------- */
void sched_init(void) {
    /* 重置全局状态 */
    run_queue_head = NULL;
    run_queue_tail = NULL;
    num_tasks = 0;
    next_task_id = 1;

    /* 清空 all_tasks 数组 */
    for (int i = 0; i < MAX_TASKS; i++) {
        all_tasks[i] = NULL;
    }

    /* init task 是第一个 */
    init_task.state = TASK_RUNNING;
    init_task.time_slice = TIME_SLICE_DEFAULT;
    init_task.cpu_time_ticks = 0;
    init_task.wakeup_tick = 0;
    init_task.next = NULL;

    /* 【Lesson 8】init task 的内核栈顶 = boot 栈顶（entry.asm 的 stack_top_64）。
     *   TSS.sp0 在切到 ring-3 任务时才真正用到，init 自己是 ring 0。
     *   但 sched_yield 切回 init 时会设 TSS.sp0 = init_task.kstack_top，
     *   所以这里必须给一个有效值。 */
    init_task.kstack_top = (u64)stack_top_64;

    /* 【Lesson 9】init task 的 crash recovery 字段初始化。
     *   init task 不会 crash（ring 0），但字段要清零防御。 */
    init_task.exit_code      = TASK_EXIT_NORMAL;
    init_task.fault_type     = 0;
    init_task.fault_rip      = 0;
    init_task.fault_addr     = 0;
    init_task.parent_task_id = 0;

    all_tasks[0] = &init_task;
    num_tasks = 1;

    current = &init_task;
    sched_initialized = 1;
}

/* ---------------------------------------------------------------
 * sched_create_task — 创建一个新任务
 * --------------------------------------------------------------- */
s64 sched_create_task(void (*entry)(void *arg), void *arg, const char *name) {
    if (!sched_initialized) {
        panic(__FILE__, __LINE__, "sched_create_task before sched_init");
    }
    if (entry == NULL) {
        return -1;
    }
    if (num_tasks >= MAX_TASKS) {
        /* 先清理已终止的任务，看看有没有空位 */
        task_reaper();
        if (num_tasks >= MAX_TASKS) {
            return -1;  /* 任务表满 */
        }
    }

    /* 分配 task_struct */
    struct task_struct *t = (struct task_struct *)kmalloc(sizeof(struct task_struct));
    if (t == NULL) {
        return -1;
    }

    /* 分配栈 */
    void *stack = kmalloc(TASK_STACK_SIZE);
    if (stack == NULL) {
        kfree(t);
        return -1;
    }

    /* 填字段 */
    t->saved_rsp       = 0;   /* 由 arch_task_stack_init 设置 */
    t->state           = TASK_READY;
    t->task_id         = next_task_id++;
    if (name != NULL) {
        copy_name(t->name, name);
    } else {
        copy_name(t->name, "unnamed");
    }
    t->entry           = entry;
    t->arg             = arg;
    t->stack           = stack;
    t->stack_size      = TASK_STACK_SIZE;
    t->cpu_time_ticks  = 0;
    t->wakeup_tick     = 0;
    t->time_slice      = TIME_SLICE_DEFAULT;
    t->next            = NULL;

    /* Lesson 6: 新任务的 IPC 等待状态全清零（不在等任何 channel） */
    t->next_waiter      = NULL;
    t->wait_channel     = NULL;
    t->wait_kind        = 0;
    t->ipc_buf          = NULL;
    t->ipc_buf_cap      = 0;
    t->ipc_out_type     = 0;
    t->ipc_out_sender   = 0;
    t->ipc_out_len      = 0;
    t->ipc_result       = 0;
    t->ipc_timeout_tick  = 0;

    /* Lesson 7: Capability 字段初始化 + 分配 CSpace
     *   - 先把字段清零（cap_cspace_init 会分配 cspace 并填充） */
    t->cspace                  = NULL;
    t->ipc_recv_cap_has_cap    = 0;
    t->ipc_recv_cap_type       = 0;
    t->ipc_recv_cap_rights     = 0;
    t->ipc_recv_cap_object     = NULL;
    t->ipc_recv_cap_lineage    = 0;

    /* Lesson 8: 用户态字段初始化（普通内核任务，is_user=0）。
     *   kstack_top = 栈顶（用于 TSS.sp0，虽然 ring 0 任务不真正用 TSS，
     *   但 sched_yield 切到它时仍会设 TSS.sp0，给一个有效值即可）。 */
    t->is_user          = 0;
    t->kstack_top        = (u64)stack + TASK_STACK_SIZE;
    t->user_rip         = 0;
    t->user_rsp         = 0;
    t->user_code_vma    = 0;
    t->user_code_pages  = 0;
    t->user_stack_vma   = 0;

    /* Lesson 9: crash recovery 字段初始化
     *   exit_code = 0（正常退出码，被异常杀时会覆盖）
     *   fault_type = 0（不是被异常杀的）
     *   parent_task_id = current->task_id（创建者） */
    t->exit_code      = TASK_EXIT_NORMAL;
    t->fault_type     = 0;
    t->fault_rip      = 0;
    t->fault_addr     = 0;
    t->parent_task_id = current->task_id;

    /* 【Lesson 7】分配并初始化 CSpace。
     *   cap_cspace_init 内部 kmalloc(sizeof(struct cspace))，
     *   失败时返回错误码。我们走 cleanup 路径。 */
    if (cap_cspace_init(t) != CAP_OK) {
        kfree(stack);
        kfree(t);
        return -1;
    }

    /* 初始化栈（伪造"被切走的老任务"样子） */
    if (arch_task_stack_init(t) != 0) {
        /* 【Lesson 7】arch_task_stack_init 失败时也要清 CSpace */
        cap_cspace_destroy(t);
        kfree(stack);
        kfree(t);
        return -1;
    }

    /* IRQ 安全地加入 all_tasks 和 run_queue */
    u64 flags = arch_irq_save();

    /* 加入 all_tasks 数组 */
    all_tasks[num_tasks] = t;
    num_tasks++;

    /* 加入就绪队列 */
    run_queue_push(t);

    arch_irq_restore(flags);

    return (s64)t->task_id;
}

/* ---------------------------------------------------------------
 * sched_yield — 主动让出 CPU
 *
 *   完整流程见文件头注释。
 *
 *   【关键点】
 *   - arch_irq_save 在整个切换过程中关中断
 *   - arch_context_switch 切换走后，prev 任务被"挂起"
 *   - 当 prev 被切回来时，从 arch_context_switch 返回，继续往下执行
 *   - arch_irq_restore 恢复 prev 当初 yield 时的中断状态
 *
 *   【多次调用的"嵌套"问题】
 *     sched_yield 内部关中断了，所以不会被同一个 CPU 上的中断重入。
 *     单核下没有并发问题。多核需要 per-CPU 的 current + 自旋锁。 */
void sched_yield(void) {
    if (!sched_initialized) {
        panic(__FILE__, __LINE__, "sched_yield before sched_init");
    }

    u64 flags = arch_irq_save();

    /* 清理已终止的任务 */
    task_reaper();

    /* 唤醒睡眠到期的任务 */
    wake_sleeping_tasks();

    struct task_struct *prev = current;

    /* 当前任务若仍 RUNNING，加入就绪队列末尾 */
    if (prev->state == TASK_RUNNING) {
        prev->state = TASK_READY;
        run_queue_push(prev);
    }

    /* 取下一个就绪任务 */
    struct task_struct *next = run_queue_pop();

    /* 如果没有其他任务可运行 */
    if (next == NULL || next == prev) {
        /* 【Lesson 6 修复】如果 prev 是 TERMINATED，不能恢复它
         *   sched_exit 把 prev 标 TERMINATED 后调 sched_yield。
         *   如果此时队列空（其他任务都 BLOCKED 或都退出了），
         *   我们不能恢复 prev（它已经死了，再让它跑会执行死代码 → panic）。
         *
         *   解决：开中断 + halt，等下一次 IRQ 唤醒。
         *   IRQ（通常是 timer）会调 sched_tick → sched_yield（重入），
         *   那时 wake_sleeping_tasks 会唤醒 BLOCKED 任务（比如 init 的 sleep），
         *   run_queue 不再空，重入的 sched_yield 会 arch_context_switch
         *   切到那个任务——本 halt loop 的栈帧被抛弃（prev 已 TERMINATED，
         *   反正不会回来），切换后 init 等任务继续跑。
         *
         *   【Lesson 7 修复 - early EOI 是关键】
         *   原来的 bug：irq.c 是 "handler → EOI"，handler 内部 sched_yield
         *   切走后永不返回 → EOI 永不发 → PIC 阻塞 → timer 停 → 冻结。
         *   现在 irq.c 改为 "EOI → handler"（early EOI），PIC 在 handler
         *   执行前就已 ready 接收下一个中断，即使 handler 切走不返回，
         *   timer 也不会停。本 halt loop 在 init sleep 到期（≤100ms）后
         *   会被重入的 sched_yield 切走。
         *
         *   【为什么不 return】
         *   sched_exit 调本函数，期望本函数永不返回（切换走后 prev 栈被弃）。
         *   如果 return，sched_exit 会 panic("sched_exit returned")。
         *   所以这里必须 halt 等待，不能 return。
         *
         *   【嵌套安全性】
         *   halt 后 timer IRQ → 重入 sched_yield → 又进 halt loop（嵌套）。
         *   每次嵌套消耗 ~200B 栈，init sleep 10 tick（100ms）内最多嵌套
         *   ~10 层，远低于 8KB 内核栈限制（TASK_STACK_SIZE = 8192，
         *   arch/include/arch/task.h）。sleep 到期后重入 sched_yield
         *   切走，整个栈被抛弃。
         *
         *   【C4 修复】原注释写"16KB 栈限制"，实际 TASK_STACK_SIZE = 8192
         *   （8KB）。8KB 下 ~10 层嵌套 × ~200B = ~2KB，仍有 6KB 余量，安全。 */
        /* 【Bug fix】如果 prev 是 BLOCKED（例如 IPC 超时等待），
         *   也不能恢复它。原来代码把 BLOCKED 任务恢复成 RUNNING，
         *   导致 IPC 返回 IPC_OK（假唤醒，无数据），单任务场景下
         *   超时不可靠。
         *
         *   正确做法：halt 等待 timer IRQ，和 TERMINATED 类似，
         *   但 halt loop 会在任务被唤醒后退出（因为
         *   wake_sleeping_tasks 把 BLOCKED 改成 READY，reentrant
         *   sched_yield 再改成 RUNNING）。
         *
         *   流程：
         *   1. 本任务 BLOCKED，run_queue 空 → halt
         *   2. timer IRQ → sched_tick → sched_yield (reentrant)
         *   3. wake_sleeping_tasks: timeout 到期 → 设 ipc_result=TIMEDOUT,
         *      state=READY, push to run_queue
         *   4. reentrant sched_yield: next=prev, set RUNNING, return
         *   5. IRET 回到 halt loop, prev->state=RUNNING, loop 退出
         *   6. 本 sched_yield return → ipc_recv_internal 继续
         *   7. IPC 检查 ipc_result=TIMEDOUT → 正确返回
         *
         *   安全性：和 TERMINATED halt 相同的嵌套安全保证，
         *   但 BLOCKED 任务会被唤醒（不会永远 halt）。 */
        if (prev->state == TASK_TERMINATED) {
            /* 开中断（让 IRQ 能进来），然后 halt 等下一次中断
             *
             *   【Lesson 8 修复】必须显式 arch_sti()，不能只靠 arch_irq_restore(flags)。
             *     原代码：arch_irq_restore(flags) 恢复"调用 sched_yield 时的 IF 状态"。
             *     问题：当 sched_yield 是从 syscall 路径调来的（int 0x80 中断门
             *           进入时 IF=0），flags.IF=0，restore 后 IF=0 → hlt 永远不醒 → 冻结。
             *     场景：user 任务 sys_exit → sched_exit → sched_yield，此时 IF=0
             *           （int 0x80 中断门清了 IF），halt loop 的 hlt 无法被 timer 唤醒。
             *     修复：arch_irq_restore 后再 arch_sti() 强制开中断，保证 hlt 能醒。
             *     安全性：prev 已 TERMINATED，halt loop 永不返回，开中断无副作用。 */
            arch_irq_restore(flags);
            arch_sti();
            while (1) {
                __asm__ volatile ("hlt");
                /* halt 后被中断唤醒，中断处理会重入 sched_yield：
                 *   - 如果唤醒了任务 → 切走，本 loop 被抛弃（不返回）
                 *   - 如果没唤醒任务 → 中断返回后继续 hlt
                 * 两种情况都安全。 */
            }
        }

        if (prev->state == TASK_BLOCKED) {
            /* BLOCKED + 无 next 任务：halt 等待唤醒
             *
             *   和 TERMINATED halt 的区别：BLOCKED 任务会被唤醒
             *   （IPC 超时 / sleep 到期 / sender 直送），所以
             *   halt loop 条件是 `prev->state == TASK_BLOCKED`，
             *   任务被唤醒后 loop 退出，sched_yield return，
             *   调用方（ipc_recv/send_internal）继续检查 ipc_result。
             *
             *   同样需要 arch_sti() 保证 hlt 可被 timer 唤醒。 */
            arch_irq_restore(flags);
            arch_sti();
            while (prev->state == TASK_BLOCKED) {
                __asm__ volatile ("hlt");
            }
            /* 任务已被唤醒（state = READY 或 RUNNING），
             * sched_yield 正常返回，IPC 代码检查 ipc_result。 */
            return;
        }

        /* prev 仍可运行（READY 或 RUNNING），恢复它 */
        prev->state = TASK_RUNNING;
        if (next == prev) {
            /* 极端情况：run_queue 只有自己，没事可做 */
            prev->time_slice = TIME_SLICE_DEFAULT;
        }
        arch_irq_restore(flags);
        return;
    }

    /* 切换到 next */
    next->state = TASK_RUNNING;
    next->time_slice = TIME_SLICE_DEFAULT;
    current = next;

    /* 【Lesson 8】更新 TSS.sp0 = next 的内核栈顶。
     *   这样当 next（在 ring 3 跑时）被中断/syscall 时，CPU 从 TSS.sp0
     *   取 ring 0 栈，把中断帧压到 next 的内核栈上。
     *   必须在 arch_context_switch 之前设（切过去后才用）。 */
    arch_tss_set_sp0(next->kstack_top);

    /* 真正的上下文切换
     *
     * 调用后，CPU 跳到 next 任务执行。
     * prev 任务"卡"在这一行，等未来被切回来时从这里继续。
     *
     * 切回来时：
     *   - RSP 恢复到 prev 当时的栈
     *   - callee-saved 寄存器恢复
     *   - RFLAGS 恢复（IF 状态）
     *   - ret 跳回这一行之后
     *
     * 注意：arch_irq_restore(flags) 在切回来后才执行，
     *      恢复的是 prev 当初 yield 时的中断状态。 */
    arch_context_switch(prev, next);

    /* ===== 切回来后从这里继续 =====
     *
     * 此时我们在 prev 的栈上，RFLAGS = prev yield 时的值
     * arch_irq_restore 把中断状态恢复成调用 sched_yield 时的状态
     *
     * 【为什么不能省略 arch_irq_restore】
     *   arch_context_switch 内部的 popfq 恢复的是"yield 时压栈的 RFLAGS"。
     *   yield 时压栈的 RFLAGS 是 arch_irq_save【之后】的值（IF=0）。
     *   所以切回来时 IF=0，但调用 sched_yield 的人期望 IF=原状态。
     *   arch_irq_restore(flags) 把 IF 恢复成原状态。 */
    arch_irq_restore(flags);
}

/* ---------------------------------------------------------------
 * sched_tick — 时钟中断驱动调度
 *
 *   由 pit_irq_handler 每 10ms 调用。
 *
 *   行为：
 *     1. 当前任务的 cpu_time_ticks++
 *     2. 时间片递减
 *     3. 时间片归 0 → sched_yield 触发切换
 *
 *   【为什么不在 sched_tick 里直接做唤醒】
 *     sched_yield 内部已经 wake_sleeping_tasks 了。
 *     sched_tick 调 sched_yield 时会顺便唤醒。 */
void sched_tick(void) {
    if (!sched_initialized) return;
    if (current == NULL) return;

    current->cpu_time_ticks++;
    current->time_slice--;

    if (current->time_slice <= 0) {
        /* 时间片用完，触发切换
         * sched_yield 内部会：
         *   - 清理 TERMINATED 任务
         *   - 唤醒 sleep 到期的任务
         *   - 把当前任务加回队列
         *   - 切到下一个 READY 任务 */
        sched_yield();
    }
}

/* ---------------------------------------------------------------
 * sched_exit — 任务退出
 *
 *   由 task_trampoline 在 entry 返回后调用。
 *
 *   行为：
 *     1. 标 current->state = TERMINATED
 *     2. sched_yield 切换走（不再加回队列）
 *     3. 永远不返回（sched_yield 切换走后，这个调用就"挂起"了）
 *
 *   【为什么不释放栈】
 *     sched_exit 在 current->stack 上执行。
 *     如果这里 kfree(current->stack)，下面 arch_context_switch 还在用这个栈
 *     → use-after-free → 崩溃。
 *
 *     标 TERMINATED 后切换走，task_reaper 在其他任务的上下文里清理。 */
/* ---------------------------------------------------------------
 * sched_exit_with_code — 带退出码退出任务（Lesson 9 核心新增）
 *
 *   和 sched_exit 一样标 TERMINATED + sched_yield 切走，
 *   但额外设置 exit_code。
 *
 *   调用场景：
 *     - exceptions.c 杀死 fault 的用户任务：
 *         sched_exit_with_code(TASK_SIG_SEGV)
 *     - syscall.c 的 SYS_exit 路径：
 *         sched_exit_with_code(user_exit_code)
 *     - sched_exit() 变成本函数的简写：
 *         sched_exit_with_code(TASK_EXIT_NORMAL)
 *
 *   【为什么先设 exit_code 再标 TERMINATED】
 *     其他任务（SYS_waitpid）可能读到 TERMINATED 任务的 exit_code。
 *     如果先标 TERMINATED 再设 exit_code，会有短暂窗口读到 0（默认）
 *     而非真实退出码。虽然关了中断（单核无并发），但防御性先设再标。 */
void sched_exit_with_code(int code) {
    u64 flags = arch_irq_save();
    current->exit_code = code;
    current->state = TASK_TERMINATED;
    arch_irq_restore(flags);

    /* 切换走，永不返回 */
    sched_yield();

    /* 不应该到这里 */
    panic(__FILE__, __LINE__, "sched_exit_with_code returned (impossible)");
}

/* ---------------------------------------------------------------
 * sched_exit — 任务退出（退出码 = 0，即正常退出）
 *
 *   sched_exit 变成 sched_exit_with_code(TASK_EXIT_NORMAL) 的简写。
 *   所有 L1-L8 的调用点无需修改。 */
void sched_exit(void) {
    sched_exit_with_code(TASK_EXIT_NORMAL);
}

/* ---------------------------------------------------------------
 * sched_sleep — 当前任务睡眠 ticks 个 tick
 *
 *   - 标 current->state = BLOCKED
 *   - 设 wakeup_tick = now + ticks
 *   - sched_yield 切换走（BLOCKED 任务不会被加入 run_queue）
 *   - 等到 sched_tick 检查到 now >= wakeup_tick 时，唤醒
 *   - 任务被调度时，从 sched_yield 返回，sched_sleep 返回
 *
 *   【为什么 sleep 不需要单独的等待队列】
 *     所有任务都在 all_tasks 数组里，遍历即可找到 BLOCKED 任务。
 *     小内核里 O(N) 遍历够快（N <= 16）。 */
void sched_sleep(u64 ticks) {
    if (ticks == 0) {
        sched_yield();   /* sleep(0) 等价于 yield */
        return;
    }

    u64 flags = arch_irq_save();
    current->state = TASK_BLOCKED;
    current->wakeup_tick = arch_pit_get_tick_count() + ticks;
    arch_irq_restore(flags);

    /* 切换走，等被唤醒后再切回来 */
    sched_yield();
}

/* ---------------------------------------------------------------
 * sched_wake — 把一个 BLOCKED 任务唤醒回 READY
 *
 * 【Lesson 6 核心】让 IPC 实现能在收到消息 / 拿走消息时唤醒对端。
 *
 * 行为：
 *   1. 检查参数合法性（非 NULL + 当前是 BLOCKED）
 *   2. 关中断（保护 run_queue）
 *   3. 标 READY + 重置时间片 + 加 run_queue
 *
 * 不清 wait_kind / wait_channel / ipc_*  字段：
 *   这些是 IPC 状态，由 ipc.c 在摘 channel 等待队列时清。
 *   sched_wake 只关心把任务加回 run_queue。
 *
 * 【为什么不调 sched_yield】
 *   sched_wake 通常在 ipc_send / ipc_recv 的临界区里调用，
 *   调用方控制何时让出 CPU（可能还想继续干点活，比如再唤醒一个）。
 *   sched_wake 后调用方可以 sched_yield 主动切到刚唤醒的任务，
 *   也可以继续做完当前事再让调度器决定。
 *
 * 【竞态说明】
 *   "waker 把 waiter 标 READY" 和 "timer 把 sleeper 标 READY"
 *   可能在同一时刻发生（timer 在 waker 之前一点把 waiter 超时唤醒了）。
 *   这时 waiter 已经被 timer 加回 run_queue，state = READY。
 *   sched_wake 检查 state != BLOCKED 就直接返回，不会重复加。
 *   ipc_send/recv 看到 ipc_result 已被设过，知道是 timer 抢先了，
 *   会做相应的"摘队列"工作。 */
void sched_wake(struct task_struct *t) {
    if (t == NULL) return;

    u64 flags = arch_irq_save();

    if (t->state != TASK_BLOCKED) {
        /* 已经被唤醒（可能 timer 抢先）或已终止 */
        arch_irq_restore(flags);
        return;
    }

    t->state = TASK_READY;
    t->time_slice = TIME_SLICE_DEFAULT;
    run_queue_push(t);

    arch_irq_restore(flags);
}

/* ---------------------------------------------------------------
 * sched_num_tasks — 返回当前任务总数（含 init task）
 *
 *   init task 在 main.c 里用它做"等所有 demo 任务退出"的循环条件。
 *   关中断读 num_tasks 防止读到中间态。 */
int sched_num_tasks(void) {
    u64 flags = arch_irq_save();
    int n = num_tasks;
    arch_irq_restore(flags);
    return n;
}

/* ---------------------------------------------------------------
 * sched_stats — 打印所有任务状态
 *
 *   用于调试，看任务有没有卡住、谁占 CPU 多。
 *
 *   格式：
 *     ID    NAME             STATE         CPU_TICKS    STACK
 *     0     init             RUNNING       123          (boot)
 *     1     counter-a        READY         45           0xFFFF80000010...
 *     2     sleeper          BLOCKED       12           0xFFFF80000020...
 * --------------------------------------------------------------- */

/* 局部：打印任务名字（固定宽度，对齐） */
static void print_padded_name(const char *name) {
    int i = 0;
    while (i < TASK_NAME_MAX && name[i] != '\0') {
        arch_console_putchar(name[i]);
        i++;
    }
    while (i < 15) {  /* 15 = TASK_NAME_MAX - 1，对齐 */
        arch_console_putchar(' ');
        i++;
    }
}

/* 局部：状态名 */
static const char *state_name(int state) {
    switch (state) {
        case TASK_UNUSED:     return "UNUSED    ";
        case TASK_READY:      return "READY     ";
        case TASK_RUNNING:    return "RUNNING   ";
        case TASK_BLOCKED:    return "BLOCKED   ";
        case TASK_TERMINATED: return "TERMINATED";
        default:               return "UNKNOWN   ";
    }
}

void sched_stats(void) {
    arch_console_set_color(CON_COLOR_CYAN);
    arch_console_print("\nTask Scheduler Stats:\n");
    arch_console_set_color(CON_COLOR_DEFAULT);

    arch_console_print("  ID   NAME              STATE         CPU_TICKS   STACK\n");

    u64 flags = arch_irq_save();

    int ready_count = 0;
    int blocked_count = 0;
    int total_cpu_ticks = 0;

    for (int i = 0; i < num_tasks; i++) {
        struct task_struct *t = all_tasks[i];
        if (t == NULL) continue;

        if (t->state == TASK_READY) ready_count++;
        if (t->state == TASK_BLOCKED) blocked_count++;
        total_cpu_ticks += t->cpu_time_ticks;

        arch_console_print("  ");
        kprint_dec(t->task_id);
        arch_console_print("   ");
        print_padded_name(t->name);
        arch_console_print("  ");
        arch_console_print(state_name(t->state));
        arch_console_print("  ");
        kprint_dec(t->cpu_time_ticks);
        arch_console_print("        ");
        if (t->stack != NULL) {
            kprint_hex((u64)t->stack);
        } else {
            arch_console_print("(boot)");
        }
        arch_console_print("\n");
    }

    arch_console_print("\n  Total tasks: ");
    kprint_dec((u64)num_tasks);
    arch_console_print("  Ready: ");
    kprint_dec((u64)ready_count);
    arch_console_print("  Blocked: ");
    kprint_dec((u64)blocked_count);
    arch_console_print("  Total CPU ticks: ");
    kprint_dec(total_cpu_ticks);
    arch_console_print("\n");

    arch_irq_restore(flags);
}

/* ================================================================
 * task_trampoline — 新任务的入口跳板
 *
 *   这个函数不是被直接调用的，而是被 arch_task_stack_init 写到新任务
 *   的栈上，作为"第一次 arch_context_switch 切到新任务时 ret 的目标"。
 *
 *   行为：
 *     1. arch_sti() — 确保新任务运行时中断是开的
 *        （第一次切换可能从 IRQ 上下文切来，IF=0，新任务应该开中断）
 *     2. current->entry(current->arg) — 调用任务入口
 *     3. sched_exit() — entry 返回后退出任务
 *
 *   【为什么需要 trampoline】
 *     - 新任务的栈是 arch_task_stack_init 伪造的
 *     - 直接 ret 到 task entry 也行，但那样没法在 entry 前后做事
 *     - trampoline 让我们可以：
 *         a) 在 entry 前开中断
 *         b) 在 entry 后调 sched_exit（清理）
 *
 *   【为什么不能让新任务直接从 entry 开始】
 *     如果 entry 返回，会 ret 到栈上某个值（栈顶是 alignment pad=0）
 *     → 跳到地址 0 → #PF → panic
 *     trampoline 在 entry 返回后调 sched_exit，安全退出。
 * ================================================================ */
void task_trampoline(void) {
    /* 开中断（防御性：第一次切换可能从 IF=0 的上下文切来） */
    arch_sti();

    /* 调用任务入口 */
    if (current != NULL && current->entry != NULL) {
        current->entry(current->arg);
    }

    /* entry 返回后，退出任务 */
    sched_exit();

    /* 不应该到这里 */
    panic(__FILE__, __LINE__, "task_trampoline: sched_exit returned");
}

/* ================================================================
 * 【Lesson 7 新增】任务访问器
 *
 *   暴露 all_tasks 数组的只读访问，让 cap.c 的 cap_revoke /
 *   cap_destroy_channel 遍历所有任务的 CSpace。
 *
 *   【为什么用 IRQ save/restore】
 *     - all_tasks 数组在 task_reaper 里被压缩（前移元素）
 *     - 并发读时可能读到中间态
 *     - 关中断保证遍历期间数组稳定
 * ================================================================ */
struct task_struct *sched_get_task_by_index(int index) {
    if (index < 0 || index >= num_tasks) {
        return NULL;
    }

    u64 flags = arch_irq_save();
    struct task_struct *t = all_tasks[index];
    arch_irq_restore(flags);
    return t;
}

struct task_struct *sched_get_task_by_id(u64 task_id) {
    u64 flags = arch_irq_save();

    for (int i = 0; i < num_tasks; i++) {
        struct task_struct *t = all_tasks[i];
        if (t == NULL) continue;
        if (t->task_id == task_id) {
            arch_irq_restore(flags);
            return t;
        }
    }

    arch_irq_restore(flags);
    return NULL;
}

/* ================================================================
 * 【Lesson 8 新增】用户态任务（ring 3）
 *
 *   user_task_main — user 任务的内核侧入口（由 task_trampoline 调用）
 *
 *   这个函数在 ring 0 运行，做的事：
 *     1. 从 current 读 user_rip / user_rsp（创建时设好的）
 *     2. 调 arch_enter_user_mode(user_rip, user_rsp, 0)
 *        → 该函数构造 IRETQ 帧 + iretq → 永不返回（切到 ring 3）
 *
 *   所以本函数永不返回到 task_trampoline（iretq 后 CPU 在 ring 3）。
 *   user 退出时调 sys_exit → sched_exit 切走。
 *
 *   【为什么不在创建时直接 iretq】
 *     arch_task_stack_init 伪造的栈是为"普通函数调用"设计的（ret 到 trampoline），
 *     trampoline 调 entry（即本函数），本函数再 iretq。
 *     如果直接在栈上伪造 IRETQ 帧跳过 trampoline，会破坏现有切换模型。
 *     复用 trampoline + 多一层 arch_enter_user_mode 最干净。
 * ================================================================ */
static void user_task_main(void *arg) {
    (void)arg;   /* arg 不用，user_rip/rsp 在 current 里 */

    struct task_struct *t = current;

    /* 防御：如果不是 user 任务，直接退出 */
    if (!t->is_user) {
        arch_console_set_color(CON_COLOR_YELLOW);
        arch_console_print("[kernel] user_task_main called by non-user task\n");
        arch_console_set_color(CON_COLOR_DEFAULT);
        sched_exit();
    }

    /* 进入 ring 3：iretq 跳到 user 代码，永不返回 */
    arch_enter_user_mode(t->user_rip, t->user_rsp, 0);

    /* 不应该到这里（arch_enter_user_mode 永不返回） */
    panic(__FILE__, __LINE__, "user_task_main: arch_enter_user_mode returned");
}

/* ---------------------------------------------------------------
 * sched_create_user_task — 创建用户态任务
 *
 *   见 sched.h 的接口注释。
 * --------------------------------------------------------------- */
s64 sched_create_user_task(const void *image, u64 image_len, const char *name) {
    if (image == NULL || image_len == 0) {
        return -1;
    }

    /* 先用 sched_create_task 创建内核侧任务（cspace + 内核栈 + task_struct）。
     * entry = user_task_main，arg = NULL（user_rip/rsp 后面设）。
     * 此时任务已 READY 加入 run_queue，但还没跑（要等 sched_yield）。
     *
     * 【竞态】sched_create_task 把任务加入 run_queue 后，可能在我们设
     *   user_rip 之前就被 timer IRQ 抢占调度跑起来。
     *   user_task_main 会读 current->user_rip（此时还是 0）→ iretq 到 0 → #GP。
     *   解决：关中断保护"创建任务 + 设字段 + 映射 user 页"整个临界区
     *   （和 L7 cap setup 的修复一样的模式）。 */
    u64 flags = arch_irq_save();

    s64 id = sched_create_task(user_task_main, NULL, name);
    if (id < 0) {
        arch_irq_restore(flags);
        return -1;
    }

    struct task_struct *t = sched_get_task_by_id((u64)id);
    if (t == NULL) {
        /* 不应该发生（刚创建），但防御 */
        arch_irq_restore(flags);
        return -1;
    }

    /* 选 user VMA 槽位：按 task_id 分配（每个 user 任务不同槽位，避免覆盖）
     *   slot = task_id（从 1 开始递增）
     *   code_vma = USER_CODE_BASE + slot * USER_SLOT_SIZE
     *   stack_top = code_vma + USER_STACK_OFFSET */
    u64 slot = t->task_id;
    u64 code_vma = USER_CODE_BASE + slot * USER_SLOT_SIZE;
    u64 stack_top = code_vma + USER_STACK_OFFSET;

    /* 映射 user 代码页 + 装入 image */
    s64 npages = arch_user_map_code(code_vma, image, image_len);
    if (npages < 0) {
        arch_console_set_color(CON_COLOR_YELLOW);
        arch_console_print("[kernel] sched_create_user_task: map code failed\n");
        arch_console_set_color(CON_COLOR_DEFAULT);
        /* 标 TERMINATED 让 reaper 清理内核侧资源（task_struct + stack + cspace）。
         *   不能直接 kfree（当前在关中断 + 任务已入 run_queue）。
         *   标 TERMINATED 后，reaper 在下次 sched_yield 时清理。
         *   注意：此时不能 task_reaper()（会压缩 all_tasks 导致 t 失效），
         *   也不要直接 kfree(t)。 */
        t->state = TASK_TERMINATED;
        arch_irq_restore(flags);
        return -1;
    }

    /* 映射 user 栈页 */
    if (arch_user_map_stack(stack_top) != 0) {
        arch_console_set_color(CON_COLOR_YELLOW);
        arch_console_print("[kernel] sched_create_user_task: map stack failed\n");
        arch_console_set_color(CON_COLOR_DEFAULT);
        /* 回滚已映射的代码页 */
        arch_user_unmap_pages(code_vma, (u64)npages);
        t->state = TASK_TERMINATED;
        arch_irq_restore(flags);
        return -1;
    }

    /* 设 user 字段 */
    t->is_user          = 1;
    t->user_rip         = code_vma;   /* 入口在 image 偏移 0 = code_vma */
    t->user_rsp         = stack_top;   /* 栈顶 */
    t->user_code_vma    = code_vma;
    t->user_code_pages  = (u64)npages;
    t->user_stack_vma   = stack_top - USER_STACK_PAGES * PAGE_SIZE;

    arch_irq_restore(flags);

    return id;
}
