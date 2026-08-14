/* ================================================================
 * kernel/ipc.c — IPC 实现（通道 + 阻塞消息传递）
 *
 * 【Lesson 6 核心新增】
 *
 * 实现 include/kernel/ipc.h 的接口。
 *
 * 核心数据结构：
 *
 *   ┌────────────────────────────────────────────────────────────┐
 *   │  channel_table[IPC_MAX_CHANNELS]   所有通道的静态数组        │
 *   │                                                            │
 *   │  每个 struct ipc_channel：                                 │
 *   │     id            — 对外句柄                              │
 *   │     in_use        — 槽位是否占用                           │
 *   │     name[16]      — 调试用名字                             │
 *   │     capacity      — 队列容量                               │
 *   │     msg_head/tail — 等待发送的消息队列（FIFO 链表）          │
 *   │     msg_count     — 当前队列中的消息数                      │
 *   │     send_waiters — 因队列满阻塞的发送者链表                 │
 *   │     recv_waiters  — 因队列空阻塞的接收者链表                 │
 *   └────────────────────────────────────────────────────────────┘
 *
 * 发送链路（ipc_send）：
 *
 *   ┌────────────────────────────────────────────────────────────┐
 *   │ 1. 校验 + 分配 struct ipc_message + 拷贝 payload            │
 *   │ 2. 关中断进入临界区                                        │
 *   │ 3. 若有 recv_waiter（还在 BLOCKED）：                       │
 *   │      → 直接把 msg 拷到 recv_waiter->ipc_buf                │
 *   │      → 设 recv_waiter->ipc_out_*  字段                     │
 *   │      → sched_wake(recv_waiter)                              │
 *   │      → kfree(msg)，return 0  （零拷贝直送）                │
 *   │ 4. 否则若队列未满：                                        │
 *   │      → 把 msg 入队尾                                       │
 *   │      → return 0                                            │
 *   │ 5. 否则队列满：                                            │
 *   │      → current 加入 send_waiters，state = BLOCKED          │
 *   │      → sched_yield 切换走                                  │
 *   │      → 被唤醒后检查 ipc_result：                           │
 *   │           0          → 重试（loop 回到 step 2）             │
 *   │           -TIMEDOUT → 超时，return 错误                    │
 *   │           -CLOSED    → channel 销毁了，return 错误          │
 *   └────────────────────────────────────────────────────────────┘
 *
 * 接收链路（ipc_recv）对称：
 *
 *   ┌────────────────────────────────────────────────────────────┐
 *   │ 1. 校验                                                     │
 *   │ 2. 关中断进入临界区                                        │
 *   │ 3. 若队列非空：                                             │
 *   │      → 取队头 msg，拷贝到 buf                               │
 *   │      → kfree(msg)                                          │
 *   │      → 若有 send_waiter：sched_wake 它（让它入队）           │
 *   │      → return 0                                            │
 *   │ 4. 否则队列空：                                             │
 *   │      → current 加入 recv_waiters，state = BLOCKED          │
 *   │      → 把 buf / buf_cap 存到 current->ipc_buf / ipc_buf_cap │
 *   │      → sched_yield 切换走                                  │
 *   │      → 被唤醒后检查 ipc_result：                           │
 *   │           0          → 数据已在 buf 里（直送或取队）         │
 *   │           -TIMEDOUT → 超时                                  │
 *   │           -CLOSED    → channel 销毁                         │
 *   └────────────────────────────────────────────────────────────┘
 *
 * 【直送（direct handoff）的价值】
 *   - 不入队不出队，少两次链表操作
 *   - 唤醒接收者时数据已就位，零延迟
 *   - 经典 L4/seL4 优化
 *
 * 【为什么 send 醒来后要 loop 重试，而不是直接返回成功】
 *   - 发送方被唤醒的原因可能是：
 *       a. 接收方拿走一条消息，让出空位
 *       b. 接收方现在阻塞了，等发送方直送
 *       c. channel 被销毁（错误）
 *       d. 超时（错误）
 *   - 情况 a 后队列可能仍满（多个发送方竞争），需要再 block
 *   - 情况 b 直送成功，return 0
 *   - 情况 c/d 直接 return 错误
 *   - loop 让代码统一处理这些情况
 *
 * 【为什么 send_waiters 用 FIFO 链表】
 *   - 公平性：先阻塞的先唤醒，避免饥饿
 *   - 简单：单链表 + head/tail，O(1) 入队出队
 * ================================================================ */

#include <arch/console.h>
#include <arch/cpu.h>
#include <arch/pit.h>
#include <arch/task.h>
#include <kernel/ipc.h>
#include <kernel/mm.h>
#include <kernel/panic.h>
#include <kernel/sched.h>
#include <kernel/types.h>

/* ---------------------------------------------------------------
 * IPC 通道（静态分配，避免 channel 自身的内存管理）
 * --------------------------------------------------------------- */

/* 单条消息：固定头 + 固定大小负载
 *
 * 【Lesson 7 新增】has_cap + cap_snap：
 *   - has_cap = 1 表示本消息附带一个 cap 快照（cap_send_with_cap 路径）
 *   - cap_snap 是 (type, rights, object, lineage) 四元组
 *   - 接收方在 deliver_to_waiter / dequeue 路径把快照拷到
 *     recv_task->ipc_recv_cap_* 字段
 *   - cap_recv_with_cap 醒来后读这些字段，安装到自己的 CSpace
 *
 * 【为什么 cap_snap 直接嵌在消息里而不是指针】
 *   - 指针需要发送方分配 + 接收方释放，跨任务内存管理复杂
 *   - 快照是固定大小（24 字节），直接嵌在消息里值拷贝，简单
 *   - 适合小内核：消息已 kmalloc，多 24 字节不算什么 */
struct ipc_message {
    u64 sender_id;             /* 发送者 task_id（自动填） */
    u64 type;                  /* 用户自定义消息类型 */
    u64 payload_len;           /* 实际负载字节数（0 ~ IPC_MAX_PAYLOAD） */
    struct ipc_message *next;  /* 链表 next（队列用） */
    u8  payload[IPC_MAX_PAYLOAD];  /* 负载数据 */

    /* Lesson 7：cap 快照（has_cap=1 时有效） */
    int has_cap;               /* 1 = 消息附带 cap 快照 */
    struct ipc_cap_snapshot cap_snap;  /* cap 快照内容 */
};

/* 通道结构 */
struct ipc_channel {
    int in_use;                       /* 槽位是否占用 */
    ipc_channel_id_t id;              /* 对外句柄 */
    char name[16];                    /* 调试用名字 */

    u64 capacity;                     /* 队列容量 */
    u64 msg_count;                    /* 当前队列消息数 */

    struct ipc_message *msg_head;     /* 队头消息 */
    struct ipc_message *msg_tail;      /* 队尾消息 */

    struct task_struct *send_waiters_head;  /* 等空位的发送者链表头 */
    struct task_struct *send_waiters_tail;  /* 链表尾 */

    struct task_struct *recv_waiters_head;  /* 等消息的接收者链表头 */
    struct task_struct *recv_waiters_tail;  /* 链表尾 */
};

/* 通道表（静态分配，O(N) 查找够快） */
static struct ipc_channel channel_table[IPC_MAX_CHANNELS];

/* 下一个 channel ID（递增，不复用，避免悬空引用） */
static ipc_channel_id_t next_channel_id = 1;

/* IPC 是否已初始化 */
static int ipc_initialized = 0;

/* ================================================================
 * 局部工具函数
 * ================================================================ */

/* 局部 memcpy（避免依赖 string.h） */
static void ipc_memcpy(void *dst, const void *src, u64 n) {
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    for (u64 i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

/* 局部 min */
static u64 ipc_min(u64 a, u64 b) {
    return (a < b) ? a : b;
}

/* 复制通道名（最多 15 字符 + \0） */
static void copy_chan_name(char *dst, const char *src) {
    int i;
    for (i = 0; i < 15 && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/* 根据 id 查找通道（返回 NULL 表示不存在） */
static struct ipc_channel *find_channel(ipc_channel_id_t id) {
    if (id == IPC_INVALID_CHANNEL) return NULL;
    for (int i = 0; i < IPC_MAX_CHANNELS; i++) {
        if (channel_table[i].in_use && channel_table[i].id == id) {
            return &channel_table[i];
        }
    }
    return NULL;
}

/* ================================================================
 * 等待队列（FIFO 单链表）操作
 *
 *   每个链表用 head + tail 两个指针，让 push/pop 都是 O(1)。
 *   任务通过 task_struct.next_waiter 字段串成链表。
 * ================================================================ */

static void wait_queue_push(struct task_struct **head, struct task_struct **tail,
                            struct task_struct *t) {
    t->next_waiter = NULL;
    if (*tail != NULL) {
        (*tail)->next_waiter = t;
    } else {
        *head = t;
    }
    *tail = t;
}

/* 弹出链表头（不管状态） */
static struct task_struct *wait_queue_pop(struct task_struct **head,
                                           struct task_struct **tail) {
    struct task_struct *t = *head;
    if (t == NULL) return NULL;
    *head = t->next_waiter;
    if (*head == NULL) {
        *tail = NULL;
    }
    t->next_waiter = NULL;
    return t;
}

/* 弹出链表头，跳过已经不在 BLOCKED 状态的任务
 * （这些任务是被 timer 超时唤醒的，还没自己摘队列）。
 * 这是"懒清理"机制：让 waker 顺便把僵尸节点弹掉。 */
static struct task_struct *wait_queue_pop_blocked(struct task_struct **head,
                                                     struct task_struct **tail) {
    while (1) {
        struct task_struct *t = wait_queue_pop(head, tail);
        if (t == NULL) return NULL;
        if (t->state == TASK_BLOCKED) return t;
        /* else: 已被 timer 唤醒的任务，弹出后丢弃（不需要再做任何事，
         * 因为 timer 已经把它加回 run_queue，等它运行时会自己处理 result）。 */
    }
}

/* 从链表中摘除指定任务（用于被 timer 唤醒的任务自己清理）
 * 如果任务不在链表里，无操作（幂等）。 */
static void wait_queue_unlink(struct task_struct **head,
                                struct task_struct **tail,
                                struct task_struct *target) {
    struct task_struct *prev = NULL;
    struct task_struct *cur = *head;
    while (cur != NULL) {
        if (cur == target) {
            if (prev != NULL) {
                prev->next_waiter = cur->next_waiter;
            } else {
                *head = cur->next_waiter;
            }
            if (*tail == cur) {
                *tail = prev;
            }
            cur->next_waiter = NULL;
            return;
        }
        prev = cur;
        cur = cur->next_waiter;
    }
}

/* ================================================================
 * 局部打印工具（避免依赖其他模块的 static 函数）
 * ================================================================ */
static void print_dec(u64 v) {
    char buf[21];
    int i = 0;
    if (v == 0) { arch_console_putchar('0'); return; }
    while (v > 0 && i < 20) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i > 0) arch_console_putchar(buf[--i]);
}

static void print_padded(const char *s, int width) {
    int i = 0;
    while (s[i] != '\0' && i < width) {
        arch_console_putchar(s[i]);
        i++;
    }
    while (i < width) {
        arch_console_putchar(' ');
        i++;
    }
}

/* ================================================================
 * 公开接口实现
 * ================================================================ */

/* ---------------------------------------------------------------
 * ipc_init — 初始化 IPC 子系统
 * --------------------------------------------------------------- */
void ipc_init(void) {
    for (int i = 0; i < IPC_MAX_CHANNELS; i++) {
        channel_table[i].in_use = 0;
        channel_table[i].id = IPC_INVALID_CHANNEL;
        channel_table[i].name[0] = '\0';
        channel_table[i].capacity = 0;
        channel_table[i].msg_count = 0;
        channel_table[i].msg_head = NULL;
        channel_table[i].msg_tail = NULL;
        channel_table[i].send_waiters_head = NULL;
        channel_table[i].send_waiters_tail = NULL;
        channel_table[i].recv_waiters_head = NULL;
        channel_table[i].recv_waiters_tail = NULL;
    }
    next_channel_id = 1;
    ipc_initialized = 1;
}

/* ---------------------------------------------------------------
 * ipc_channel_create — 创建一个消息通道
 * --------------------------------------------------------------- */
ipc_channel_id_t ipc_channel_create(const char *name, s64 capacity) {
    if (!ipc_initialized) {
        panic(__FILE__, __LINE__, "ipc_channel_create before ipc_init");
    }

    /* 找一个空槽 */
    int slot = -1;
    for (int i = 0; i < IPC_MAX_CHANNELS; i++) {
        if (!channel_table[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return IPC_INVALID_CHANNEL;   /* 通道表满 */
    }

    struct ipc_channel *c = &channel_table[slot];
    c->in_use = 1;
    c->id = next_channel_id++;
    if (name != NULL) {
        copy_chan_name(c->name, name);
    } else {
        copy_chan_name(c->name, "anon");
    }
    if (capacity < 0) {
        c->capacity = IPC_DEFAULT_CAPACITY;
    } else {
        c->capacity = (u64)capacity;
    }
    c->msg_count = 0;
    c->msg_head = NULL;
    c->msg_tail = NULL;
    c->send_waiters_head = NULL;
    c->send_waiters_tail = NULL;
    c->recv_waiters_head = NULL;
    c->recv_waiters_tail = NULL;

    return c->id;
}

/* ---------------------------------------------------------------
 * ipc_channel_destroy — 销毁通道
 *
 *   - 释放所有排队中的消息
 *   - 唤醒所有阻塞的 sender/receiver（设 result = -CLOSED）
 *   - 标 in_use = 0
 * --------------------------------------------------------------- */
int ipc_channel_destroy(ipc_channel_id_t id) {
    /* 关中断保护整个销毁过程（多步操作必须原子） */
    u64 flags = arch_irq_save();

    struct ipc_channel *c = find_channel(id);
    if (c == NULL) {
        arch_irq_restore(flags);
        return IPC_ERR_INVAL;
    }

    /* 释放所有排队消息 */
    struct ipc_message *m = c->msg_head;
    while (m != NULL) {
        struct ipc_message *next = m->next;
        kfree(m);
        m = next;
    }
    c->msg_head = NULL;
    c->msg_tail = NULL;
    c->msg_count = 0;

    /* 唤醒所有 send_waiters，告诉它们 channel 关了 */
    while (1) {
        struct task_struct *t = wait_queue_pop_blocked(&c->send_waiters_head,
                                                        &c->send_waiters_tail);
        if (t == NULL) break;
        t->ipc_result = IPC_ERR_CLOSED;
        t->wait_channel = NULL;
        t->wait_kind = 0;
        t->ipc_timeout_tick = 0;
        sched_wake(t);
    }
    /* 把残留在队列里的（已被 timer 唤醒的僵尸节点）也清掉 */
    while (wait_queue_pop(&c->send_waiters_head, &c->send_waiters_tail) != NULL) {
        /* 弹掉即可，task 自己会处理 result */
    }

    /* 唤醒所有 recv_waiters */
    while (1) {
        struct task_struct *t = wait_queue_pop_blocked(&c->recv_waiters_head,
                                                        &c->recv_waiters_tail);
        if (t == NULL) break;
        t->ipc_result = IPC_ERR_CLOSED;
        t->wait_channel = NULL;
        t->wait_kind = 0;
        t->ipc_timeout_tick = 0;
        sched_wake(t);
    }
    while (wait_queue_pop(&c->recv_waiters_head, &c->recv_waiters_tail) != NULL) {
        /* 弹掉僵尸节点 */
    }

    /* 标槽位为空，下次 create 可复用 */
    c->in_use = 0;
    c->id = IPC_INVALID_CHANNEL;
    c->name[0] = '\0';

    arch_irq_restore(flags);
    return IPC_OK;
}

/* ================================================================
 * 内部：把消息直送给一个等待中的接收者
 *
 *   输入：
 *     recv_t — 接收者任务（必须仍在 BLOCKED 状态，已在 recv_waiters 队列里）
 *     msg    — 待发送的消息
 *
 *   行为：
 *     - 设 recv_t->ipc_out_*  字段（type / sender / len）
 *     - 拷贝 payload 到 recv_t->ipc_buf（按 ipc_buf_cap 截断）
 *     - 设 recv_t->ipc_result = 0（成功）
 *     - 清 recv_t 的等待状态字段
 *     - sched_wake(recv_t)
 *
 *   注意：调用方负责把 recv_t 从 recv_waiters 摘下来（已 pop）。
 *        这里只负责"填字段 + 唤醒"。
 * ================================================================ */
static void deliver_to_waiter(struct task_struct *recv_t, struct ipc_message *msg) {
    u64 actual = ipc_min(msg->payload_len, recv_t->ipc_buf_cap);
    if (actual > 0 && recv_t->ipc_buf != NULL) {
        ipc_memcpy(recv_t->ipc_buf, msg->payload, actual);
    }
    recv_t->ipc_out_type   = msg->type;
    recv_t->ipc_out_sender = msg->sender_id;
    recv_t->ipc_out_len    = msg->payload_len;
    recv_t->ipc_result     = IPC_OK;

    /* 【Lesson 7】把消息附带的 cap 快照拷贝到 recv_task 字段。
     *
     *   - has_cap = 1 表示 sender 通过 ipc_send_with_cap 附带了 cap
     *   - 把快照内容拷到 recv_t->ipc_recv_cap_*
     *   - cap_recv_with_cap 醒来后读这些字段，安装到自己的 CSpace
     *
     * 【为什么用 task 字段而不是返回值】
     *   - ipc_recv_on_channel 的签名里 in_out_cap 是字节数，不好塞 cap 快照
     *   - 用 task 字段"传出"是 L6 已有的模式（ipc_out_sender 等）
     *   - 一致性 > 接口纯粹性
     *
     * 【has_cap = 0 时也要清字段】
     *   - 防止 recv_task 之前留有旧值（前一次 recv 的 cap）
     *   - 调用方看到 has_cap=0 就知道本次没附带 cap */
    if (msg->has_cap) {
        recv_t->ipc_recv_cap_has_cap = 1;
        recv_t->ipc_recv_cap_type     = msg->cap_snap.type;
        recv_t->ipc_recv_cap_rights   = msg->cap_snap.rights;
        recv_t->ipc_recv_cap_object   = msg->cap_snap.object;
        recv_t->ipc_recv_cap_lineage  = msg->cap_snap.lineage;
    } else {
        recv_t->ipc_recv_cap_has_cap = 0;
        recv_t->ipc_recv_cap_type     = 0;
        recv_t->ipc_recv_cap_rights   = 0;
        recv_t->ipc_recv_cap_object   = NULL;
        recv_t->ipc_recv_cap_lineage  = 0;
    }

    /* 清等待状态：任务即将变成 READY，不再是 wait 状态 */
    recv_t->wait_channel   = NULL;
    recv_t->wait_kind      = IPC_WAIT_NONE;
    recv_t->ipc_buf        = NULL;
    recv_t->ipc_buf_cap    = 0;
    recv_t->ipc_timeout_tick = 0;

    sched_wake(recv_t);
}

/* ================================================================
 * 内部：把消息入队（队尾）
 * ================================================================ */
static void msg_enqueue(struct ipc_channel *c, struct ipc_message *m) {
    m->next = NULL;
    if (c->msg_tail != NULL) {
        c->msg_tail->next = m;
    } else {
        c->msg_head = m;
    }
    c->msg_tail = m;
    c->msg_count++;
}

/* ================================================================
 * 内部：从队头取一条消息
 * ================================================================ */
static struct ipc_message *msg_dequeue(struct ipc_channel *c) {
    struct ipc_message *m = c->msg_head;
    if (m == NULL) return NULL;
    c->msg_head = m->next;
    if (c->msg_head == NULL) {
        c->msg_tail = NULL;
    }
    c->msg_count--;
    m->next = NULL;
    return m;
}

/* ================================================================
 * 内部：唤醒一个 send_waiter（队列已有空位时调用）
 *
 *   不做"代替 sender 入队"——只把 sender 唤醒，让它自己 retry。
 *   这样 sender 醒来后能根据当时的 channel 状态决定：
 *     - 直送 recv_waiter（如果有了）
 *     - 入队（如果还有空位）
 *     - 再 block（如果其他 sender 抢先填满了）
 * ================================================================ */
static void wake_one_send_waiter(struct ipc_channel *c) {
    struct task_struct *t = wait_queue_pop_blocked(&c->send_waiters_head,
                                                    &c->send_waiters_tail);
    if (t == NULL) return;

    /* 标"唤醒原因 = 有空位"，sender 醒来 retry */
    t->ipc_result = IPC_OK;
    t->wait_channel = NULL;
    t->wait_kind = IPC_WAIT_NONE;
    t->ipc_timeout_tick = 0;
    /* ipc_buf 仍指向 sender 的 msg，sender 醒来后用它 retry */

    sched_wake(t);
}

/* ================================================================
 * ipc_send — 阻塞发送消息（核心实现）
 *
 *   timeout_ticks == 0 且 nonblock == 0 → 永久阻塞
 *   timeout_ticks > 0  且 nonblock == 0 → 限时阻塞
 *   nonblock != 0                       → 非阻塞（try_send）
 *
 *   这一个函数同时支撑 ipc_send / ipc_try_send / ipc_send_timeout / ipc_send_with_cap。
 *
 * 【Lesson 7 重构】
 *   原版参数是 ipc_channel_id_t id，每次循环都要 find_channel(id)。
 *   现在 cap 层需要直接传 channel 指针，避免 cap.c 依赖 channel 表结构。
 *
 *   改造：参数改为 struct ipc_channel *c，caller 负责拿到指针。
 *   原 ipc_send 等 wrapper 调 find_channel(id) 然后调本函数。
 *
 *   增加参数 cap_snap：非 NULL 表示本消息附带 cap 快照（cap_send_with_cap 用）。
 *   cap_snap=NULL → msg->has_cap = 0。
 *   cap_snap非NULL → msg->has_cap = 1 + 拷贝快照。
 * ================================================================ */
static int ipc_send_internal(struct ipc_channel *c, u64 type,
                              const void *payload, u64 len,
                              u64 timeout_ticks, int nonblock,
                              const struct ipc_cap_snapshot *cap_snap) {
    /* 参数校验 */
    if (len > IPC_MAX_PAYLOAD) {
        return IPC_ERR_TOOLONG;
    }
    if (c == NULL) {
        return IPC_ERR_INVAL;
    }

    /* 分配消息结构 + 拷贝 payload（值语义，发送方返回后改 payload 不影响已发消息） */
    struct ipc_message *msg =
        (struct ipc_message *)kmalloc(sizeof(struct ipc_message));
    if (msg == NULL) {
        return IPC_ERR_NOMEM;
    }
    msg->sender_id   = current ? current->task_id : 0;
    msg->type        = type;
    msg->payload_len = len;
    msg->next        = NULL;
    if (payload != NULL && len > 0) {
        ipc_memcpy(msg->payload, payload, len);
    }

    /* 【Lesson 7】cap 快照 */
    if (cap_snap != NULL) {
        msg->has_cap = 1;
        msg->cap_snap.type    = cap_snap->type;
        msg->cap_snap.rights  = cap_snap->rights;
        msg->cap_snap.object  = cap_snap->object;
        msg->cap_snap.lineage = cap_snap->lineage;
    } else {
        msg->has_cap = 0;
        msg->cap_snap.type    = 0;
        msg->cap_snap.rights  = 0;
        msg->cap_snap.object  = NULL;
        msg->cap_snap.lineage = 0;
    }

    /* 永久阻塞 or 限时阻塞 or 非阻塞 */
    u64 deadline = 0;
    if (timeout_ticks > 0) {
        deadline = arch_pit_get_tick_count() + timeout_ticks;
    }

    /* 主循环：try-then-block */
    while (1) {
        u64 flags = arch_irq_save();

        /* 【Lesson 7】检查 channel 是否仍然有效（destroy 后 in_use=0）
         *   原来用 find_channel(id) == NULL 判断，现在用 !c->in_use。 */
        if (!c->in_use) {
            /* channel 不存在或已销毁 */
            kfree(msg);
            arch_irq_restore(flags);
            return IPC_ERR_CLOSED;
        }

        /* (1) 直送：如果有 recv_waiter，把消息直接交给它 */
        struct task_struct *recv_t =
            wait_queue_pop_blocked(&c->recv_waiters_head, &c->recv_waiters_tail);
        if (recv_t != NULL) {
            /* 直送：消息不进队列，直接拷到接收者 buf */
            deliver_to_waiter(recv_t, msg);
            kfree(msg);
            arch_irq_restore(flags);
            return IPC_OK;
        }

        /* (2) 入队：如果队列没满 */
        if (c->msg_count < c->capacity) {
            msg_enqueue(c, msg);
            arch_irq_restore(flags);
            return IPC_OK;
        }

        /* (3) 队列满 → 必须阻塞（或非阻塞时返回 WOULDBLOCK） */
        if (nonblock) {
            arch_irq_restore(flags);
            kfree(msg);
            return IPC_ERR_WOULDBLOCK;
        }

        /* 阻塞自己 */
        current->state           = TASK_BLOCKED;   /* 【关键】必须置 BLOCKED，否则 sched_yield 会把我们加回 run_queue */
        current->wait_kind       = IPC_WAIT_SEND;
        current->wait_channel    = c;
        current->ipc_result      = IPC_OK;
        current->ipc_buf         = msg;   /* 保存 msg 指针，retry 时复用 */
        current->ipc_buf_cap     = 0;
        current->ipc_out_type    = 0;
        current->ipc_out_sender  = 0;
        current->ipc_out_len     = 0;
        current->ipc_timeout_tick = (deadline > 0) ? deadline : 0;

        wait_queue_push(&c->send_waiters_head, &c->send_waiters_tail, current);

        arch_irq_restore(flags);

        /* 切换走，让其他任务运行 */
        sched_yield();

        /* ===== 被唤醒后从这里继续 =====
         *
         * 此时 current->ipc_result 反映唤醒原因：
         *   IPC_OK          → 有人在等我 retry（接收方拿走了一条消息）
         *   IPC_ERR_TIMEDOUT → 超时
         *   IPC_ERR_CLOSED   → channel 被销毁
         */
        flags = arch_irq_save();

        if (current->wait_channel != NULL) {
            struct ipc_channel *chan =
                (struct ipc_channel *)current->wait_channel;
            wait_queue_unlink(&chan->send_waiters_head,
                              &chan->send_waiters_tail, current);
            current->wait_channel = NULL;
        }
        current->wait_kind = IPC_WAIT_NONE;
        current->ipc_timeout_tick = 0;
        /* ipc_buf 仍指向 msg，继续保留 */

        int result = current->ipc_result;
        arch_irq_restore(flags);

        if (result != IPC_OK) {
            /* 超时或 channel 关闭：释放 msg，返回错误 */
            kfree(msg);
            current->ipc_buf = NULL;
            return result;
        }

        /* result == IPC_OK：retry，循环回到顶部的 try */
    }
    /* 不可达 */
}

/* ---------------------------------------------------------------
 * ipc_send — 阻塞发送
 *   【Lesson 7】变为 wrapper：find_channel → ipc_send_internal
 * --------------------------------------------------------------- */
int ipc_send(ipc_channel_id_t id, u64 type, const void *payload, u64 len) {
    struct ipc_channel *c = find_channel(id);
    if (c == NULL) return IPC_ERR_CLOSED;
    return ipc_send_internal(c, type, payload, len, 0, 0, NULL);
}

/* ---------------------------------------------------------------
 * ipc_try_send — 非阻塞发送
 * --------------------------------------------------------------- */
int ipc_try_send(ipc_channel_id_t id, u64 type, const void *payload, u64 len) {
    struct ipc_channel *c = find_channel(id);
    if (c == NULL) return IPC_ERR_CLOSED;
    return ipc_send_internal(c, type, payload, len, 0, 1, NULL);
}

/* ---------------------------------------------------------------
 * ipc_send_timeout — 限时阻塞发送
 * --------------------------------------------------------------- */
int ipc_send_timeout(ipc_channel_id_t id, u64 type, const void *payload,
                     u64 len, u64 timeout_ticks) {
    struct ipc_channel *c = find_channel(id);
    if (c == NULL) return IPC_ERR_CLOSED;
    if (timeout_ticks == 0) {
        /* timeout == 0 时退化为非阻塞 */
        return ipc_send_internal(c, type, payload, len, 0, 1, NULL);
    }
    return ipc_send_internal(c, type, payload, len, timeout_ticks, 0, NULL);
}

/* ================================================================
 * ipc_recv — 阻塞接收消息（核心实现）
 *
 *   和 ipc_send_internal 对称。
 *
 * 【Lesson 7 重构】
 *   参数改为 struct ipc_channel *c。
 *   开始时清空 current->ipc_recv_cap_* 字段（cap_recv_with_cap 读取）。
 *   dequeue 路径上把 msg->has_cap + cap_snap 拷到 current 字段。
 *   直送路径在 deliver_to_waiter 里拷贝（已加）。
 * ================================================================ */
static int ipc_recv_internal(struct ipc_channel *c, u64 *out_type,
                              void *buf, u64 *in_out_cap,
                              u64 timeout_ticks, int nonblock) {
    if (buf == NULL || in_out_cap == NULL) {
        return IPC_ERR_INVAL;
    }
    if (c == NULL) {
        return IPC_ERR_INVAL;
    }

    u64 buf_cap = *in_out_cap;
    u64 deadline = 0;
    if (timeout_ticks > 0) {
        deadline = arch_pit_get_tick_count() + timeout_ticks;
    }

    /* 【Lesson 7】在循环外清 cap 快照字段。
     *   - 如果本次 recv 没拿到 cap，字段保持清零状态
     *   - 拿到 cap 时 dequeue / deliver_to_waiter 会填上 */
    current->ipc_recv_cap_has_cap = 0;
    current->ipc_recv_cap_type     = 0;
    current->ipc_recv_cap_rights   = 0;
    current->ipc_recv_cap_object   = NULL;
    current->ipc_recv_cap_lineage  = 0;

    while (1) {
        u64 flags = arch_irq_save();

        /* 【Lesson 7】检查 channel 是否仍有效 */
        if (!c->in_use) {
            arch_irq_restore(flags);
            return IPC_ERR_CLOSED;
        }

        /* (1) 取队头消息 */
        if (c->msg_count > 0) {
            struct ipc_message *m = msg_dequeue(c);

            /* 拷贝到调用方 buf（按容量截断） */
            u64 actual = ipc_min(m->payload_len, buf_cap);
            if (actual > 0) {
                ipc_memcpy(buf, m->payload, actual);
            }
            if (out_type != NULL) {
                *out_type = m->type;
            }
            /* 通过 current->ipc_out_*  暴露 sender/len，调用方可读 */
            current->ipc_out_sender = m->sender_id;
            current->ipc_out_len    = m->payload_len;
            *in_out_cap = m->payload_len;   /* 真实长度，可能 > buf_cap */

            /* 【Lesson 7】dequeue 路径拷贝 cap 快照到 current 字段
             *   （直送路径在 deliver_to_waiter 里已拷贝） */
            if (m->has_cap) {
                current->ipc_recv_cap_has_cap = 1;
                current->ipc_recv_cap_type    = m->cap_snap.type;
                current->ipc_recv_cap_rights  = m->cap_snap.rights;
                current->ipc_recv_cap_object  = m->cap_snap.object;
                current->ipc_recv_cap_lineage = m->cap_snap.lineage;
            } else {
                current->ipc_recv_cap_has_cap = 0;
            }

            kfree(m);

            /* 拿走一条消息后，若有 send_waiter，唤醒它（让它入队） */
            wake_one_send_waiter(c);

            arch_irq_restore(flags);
            return IPC_OK;
        }

        /* (2) 队列空 → 必须阻塞（或非阻塞返回 WOULDBLOCK） */
        if (nonblock) {
            arch_irq_restore(flags);
            return IPC_ERR_WOULDBLOCK;
        }

        /* 阻塞自己，把 buf 留给 sender 直送时用 */
        current->state            = TASK_BLOCKED;   /* 【关键】必须置 BLOCKED，否则 sched_yield 会把我们加回 run_queue */
        current->wait_kind        = IPC_WAIT_RECV;
        current->wait_channel    = c;
        current->ipc_buf         = buf;
        current->ipc_buf_cap     = buf_cap;
        current->ipc_out_type    = 0;
        current->ipc_out_sender  = 0;
        current->ipc_out_len     = 0;
        current->ipc_result      = IPC_OK;
        current->ipc_timeout_tick = (deadline > 0) ? deadline : 0;

        wait_queue_push(&c->recv_waiters_head, &c->recv_waiters_tail, current);

        arch_irq_restore(flags);

        /* 切换走 */
        sched_yield();

        /* ===== 被唤醒后从这里继续 ===== */
        flags = arch_irq_save();

        int result = current->ipc_result;
        u64 out_type_v = current->ipc_out_type;
        u64 out_sender = current->ipc_out_sender;
        u64 out_len    = current->ipc_out_len;

        /* 如果是被 timer 唤醒的，wait_channel 仍非 NULL，需要自己摘队列 */
        if (current->wait_channel != NULL) {
            struct ipc_channel *chan =
                (struct ipc_channel *)current->wait_channel;
            wait_queue_unlink(&chan->recv_waiters_head,
                              &chan->recv_waiters_tail, current);
            current->wait_channel = NULL;
        }
        current->wait_kind = IPC_WAIT_NONE;
        current->ipc_buf = NULL;
        current->ipc_buf_cap = 0;
        current->ipc_timeout_tick = 0;

        /* 【Lesson 7】读 cap 快照字段：
         *   - 直送路径在 deliver_to_waiter 里填到 current 字段
         *   - 这里被唤醒后读出来保留（ sched_wake 后被其他任务调度有可能被覆盖，
         *     但在 waker 填到本任务 wakeup 之间 current 不变）
         *   - 用临时变量读出，避免 arch_irq_restore 后被改 */
        int recv_has_cap = current->ipc_recv_cap_has_cap;
        u32 recv_cap_type    = current->ipc_recv_cap_type;
        u32 recv_cap_rights  = current->ipc_recv_cap_rights;
        void *recv_cap_object = current->ipc_recv_cap_object;
        u64  recv_cap_lineage = current->ipc_recv_cap_lineage;

        arch_irq_restore(flags);

        if (result != IPC_OK) {
            /* 超时或 channel 关闭 */
            return result;
        }

        /* 直送路径：sender 已经把数据写到 buf 里了
         * 通过 out_*  字段拿到 type / sender / len */
        if (out_type != NULL) {
            *out_type = out_type_v;
        }
        current->ipc_out_sender = out_sender;
        current->ipc_out_len    = out_len;
        *in_out_cap = out_len;

        /* 【Lesson 7】恢复 cap 快照字段（从临时变量读回）
         *   - 直送路径已填进 current 字段，但被 sched_yield 切回来后
         *     current 字段可能仍保留（因为 current 在本任务上）
         *   - 这里重新写回，以防中间被其他路径清
         *   - 让 cap_recv_with_cap 能读到 */
        current->ipc_recv_cap_has_cap = recv_has_cap;
        current->ipc_recv_cap_type    = recv_cap_type;
        current->ipc_recv_cap_rights  = recv_cap_rights;
        current->ipc_recv_cap_object  = recv_cap_object;
        current->ipc_recv_cap_lineage = recv_cap_lineage;
        return IPC_OK;
    }
    /* 不可达 */
}

/* ---------------------------------------------------------------
 * ipc_recv — 阻塞接收
 *   【Lesson 7】变为 wrapper
 * --------------------------------------------------------------- */
int ipc_recv(ipc_channel_id_t id, u64 *out_type, void *buf, u64 *in_out_cap) {
    struct ipc_channel *c = find_channel(id);
    if (c == NULL) return IPC_ERR_CLOSED;
    return ipc_recv_internal(c, out_type, buf, in_out_cap, 0, 0);
}

/* ---------------------------------------------------------------
 * ipc_try_recv — 非阻塞接收
 * --------------------------------------------------------------- */
int ipc_try_recv(ipc_channel_id_t id, u64 *out_type, void *buf, u64 *in_out_cap) {
    struct ipc_channel *c = find_channel(id);
    if (c == NULL) return IPC_ERR_CLOSED;
    return ipc_recv_internal(c, out_type, buf, in_out_cap, 0, 1);
}

/* ---------------------------------------------------------------
 * ipc_recv_timeout — 限时阻塞接收
 * --------------------------------------------------------------- */
int ipc_recv_timeout(ipc_channel_id_t id, u64 *out_type, void *buf,
                     u64 *in_out_cap, u64 timeout_ticks) {
    struct ipc_channel *c = find_channel(id);
    if (c == NULL) return IPC_ERR_CLOSED;
    if (timeout_ticks == 0) {
        return ipc_recv_internal(c, out_type, buf, in_out_cap, 0, 1);
    }
    return ipc_recv_internal(c, out_type, buf, in_out_cap, timeout_ticks, 0);
}

/* ---------------------------------------------------------------
 * ipc_stats — 打印所有 channel 状态
 * --------------------------------------------------------------- */
static int count_waiters(struct task_struct *head) {
    int n = 0;
    while (head != NULL) {
        n++;
        head = head->next_waiter;
    }
    return n;
}

void ipc_stats(void) {
    arch_console_set_color(CON_COLOR_CYAN);
    arch_console_print("\nIPC Channel Stats:\n");
    arch_console_set_color(CON_COLOR_DEFAULT);

    arch_console_print("  ID    NAME            CAP  MSGS  SEND_WAIT  RECV_WAIT\n");

    u64 flags = arch_irq_save();

    int active = 0;
    u64 total_msgs = 0;
    int total_send_waiters = 0;
    int total_recv_waiters = 0;

    for (int i = 0; i < IPC_MAX_CHANNELS; i++) {
        struct ipc_channel *c = &channel_table[i];
        if (!c->in_use) continue;
        active++;
        total_msgs += c->msg_count;
        int sw = count_waiters(c->send_waiters_head);
        int rw = count_waiters(c->recv_waiters_head);
        total_send_waiters += sw;
        total_recv_waiters += rw;

        arch_console_print("  ");
        print_dec(c->id);
        arch_console_print("    ");
        print_padded(c->name, 12);
        arch_console_print("  ");
        print_dec(c->capacity);
        arch_console_print("    ");
        print_dec(c->msg_count);
        arch_console_print("    ");
        print_dec((u64)sw);
        arch_console_print("          ");
        print_dec((u64)rw);
        arch_console_print("\n");
    }

    arch_console_print("\n  Total channels: ");
    print_dec((u64)active);
    arch_console_print("  Pending msgs: ");
    print_dec(total_msgs);
    arch_console_print("  Send waiters: ");
    print_dec((u64)total_send_waiters);
    arch_console_print("  Recv waiters: ");
    print_dec((u64)total_recv_waiters);
    arch_console_print("\n");

    arch_irq_restore(flags);
}

/* ================================================================
 * 【Lesson 7 新增】channel 指针版内部 API
 *
 *   给 cap.c 用，让它不必直接访问 channel_table / find_channel。
 *
 *   设计要点：
 *     - find_channel 是 static 函数（不暴露）
 *     - 暴露 ipc_channel_lookup(id) → 返回 channel 指针
 *     - 暴露 ipc_channel_id_of(c)  → 返回 channel id
 *     - send/recv/try/timeout 系列函数接收 channel 指针
 *
 *   cap.c 的使用方式：
 *     struct cap *cap = cap_lookup_check(slot, CAP_RIGHT_SEND);
 *     struct ipc_channel *c = (struct ipc_channel *)cap->object;
 *     return ipc_send_on_channel(c, type, payload, len);
 *
 *   【为什么 cap->object 是 void*】
 *     - struct cap 的 object 字段是 void*（不依赖具体类型）
 *     - cap_channel_create 时把 channel 指针存进去
 *     - cap_send/recv 取出后 cast 成 struct ipc_channel*
 *     - 这是 forward declaration 模式，避免头文件循环依赖
 * ================================================================ */

struct ipc_channel *ipc_channel_lookup(ipc_channel_id_t id) {
    return find_channel(id);
}

ipc_channel_id_t ipc_channel_id_of(struct ipc_channel *c) {
    if (c == NULL) return IPC_INVALID_CHANNEL;
    return c->id;
}

int ipc_send_on_channel(struct ipc_channel *c, u64 type,
                         const void *payload, u64 len) {
    return ipc_send_internal(c, type, payload, len, 0, 0, NULL);
}

int ipc_recv_on_channel(struct ipc_channel *c, u64 *out_type,
                         void *buf, u64 *in_out_cap) {
    return ipc_recv_internal(c, out_type, buf, in_out_cap, 0, 0);
}

int ipc_try_send_on_channel(struct ipc_channel *c, u64 type,
                             const void *payload, u64 len) {
    return ipc_send_internal(c, type, payload, len, 0, 1, NULL);
}

int ipc_try_recv_on_channel(struct ipc_channel *c, u64 *out_type,
                             void *buf, u64 *in_out_cap) {
    return ipc_recv_internal(c, out_type, buf, in_out_cap, 0, 1);
}

int ipc_send_timeout_on_channel(struct ipc_channel *c, u64 type,
                                 const void *payload, u64 len,
                                 u64 timeout_ticks) {
    if (timeout_ticks == 0) {
        return ipc_send_internal(c, type, payload, len, 0, 1, NULL);
    }
    return ipc_send_internal(c, type, payload, len, timeout_ticks, 0, NULL);
}

int ipc_recv_timeout_on_channel(struct ipc_channel *c, u64 *out_type,
                                 void *buf, u64 *in_out_cap,
                                 u64 timeout_ticks) {
    if (timeout_ticks == 0) {
        return ipc_recv_internal(c, out_type, buf, in_out_cap, 0, 1);
    }
    return ipc_recv_internal(c, out_type, buf, in_out_cap, timeout_ticks, 0);
}

/* ---------------------------------------------------------------
 * ipc_send_with_cap — 发消息时附带一个 cap 快照
 *
 *   把 cap_snap 嵌进消息的 has_cap + cap_snap 字段。
 *   接收方在 recv 时通过 current->ipc_recv_cap_* 拿到。
 *
 *   【cap_snap 参数约束】
 *     - cap_snap != NULL 时附加 cap 快照
 *     - cap_snap == NULL 时退化为普通 ipc_send（兼容）
 *     - cap_snap->object 必须非 NULL（否则安装到 CSpace 没意义） */
int ipc_send_with_cap(struct ipc_channel *c, u64 type,
                       const void *payload, u64 len,
                       const struct ipc_cap_snapshot *cap_snap) {
    /* 校验 cap_snap 内容（如果传了的话） */
    if (cap_snap != NULL && cap_snap->object == NULL) {
        /* object 必须非 NULL：cap_recv_with_cap 安装到 CSpace 时
         * 需要有效的 underlying object 指针 */
        return IPC_ERR_INVAL;
    }
    return ipc_send_internal(c, type, payload, len, 0, 0, cap_snap);
}
