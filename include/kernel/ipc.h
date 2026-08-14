/* ================================================================
 * kernel/ipc.h — 进程间通信（IPC）架构无关接口
 *
 * 【Lesson 6 核心新增】
 *
 * AIHYK 是 Hybrid Kernel（混合内核）——内核态有微内核风格的
 * "server task"提供服务，用户态有传统宏内核的"用户进程"。
 * IPC 是 server 之间、用户进程↔server 之间的"通用语言"。
 *
 * 这个头文件定义 IPC 的统一接口：
 *   ipc_init()                       — 初始化 IPC 子系统
 *   ipc_channel_create(name, cap)    — 创建一个消息通道
 *   ipc_channel_destroy(id)          — 销毁通道
 *   ipc_send(id, type, p, len)       — 阻塞发送
 *   ipc_recv(id, &type, p, &cap)     — 阻塞接收
 *   ipc_try_send / ipc_try_recv      — 非阻塞变体
 *   ipc_send_timeout / ipc_recv_timeout — 带超时的阻塞变体
 *   ipc_stats()                      — 打印通道状态（调试）
 *
 * 抽象设计（与 sched.h / mm.h 一致）：
 *   - "做什么" 在这里声明
 *   - "怎么做" 在 kernel/ipc.c 实现
 *   - 内部依赖：sched_wake / sched_yield / kmalloc / kfree
 *
 * =================================================================
 *
 * 【IPC 模型：通道 + 异步消息队列】
 *
 *   每个 channel 是一个有界 FIFO 消息队列：
 *
 *     ┌─────────────────────── channel ───────────────────────┐
 *     │                                                         │
 *     │   send_waiters ──► T1 ──► T2 ──► NULL                  │
 *     │   （channel 已满时阻塞在这）                              │
 *     │                                                         │
 *     │   msg queue   ┌────┐ ┌────┐ ┌────┐ ┌────┐              │
 *     │   (FIFO)      │ M1 │ │ M2 │ │ M3 │ │ M4 │  (capacity=4)│
 *     │               └────┘ └────┘ └────┘ └────┘              │
 *     │                                                         │
 *     │   recv_waiters ──► T3 ──► NULL                          │
 *     │   （channel 空时阻塞在这）                                │
 *     │                                                         │
 *     └─────────────────────────────────────────────────────────┘
 *
 *   - 发送方：先尝试入队，队满则进 send_waiters 阻塞
 *   - 接收方：先尝试出队，队空则进 recv_waiters 阻塞
 *   - 任一方操作后，若对方有等待者，唤醒对方
 *
 *   这是经典"bounded buffer + condition variable"模型，
 *   用任务阻塞/唤醒实现，而不是忙等。
 *
 * =================================================================
 *
 * 【为什么选 channel 而不是 thread-to-thread】
 *
 *   L4/seL4 用 thread ID 做端点（"send to thread 17"），
 *   这要求发送方知道接收方的 thread ID（命名问题）。
 *
 *   Channel 模型：
 *   - channel 有名字（"logger", "calc", ...），更易发现
 *   - channel 是一等对象，可以传递、关闭
 *   - 多接收者可以从同一 channel 收（load balance）
 *   - 多发送者可以发到同一 channel（fan-in）
 *
 *   对教学项目更直观，和 POSIX mq 接近。
 *
 * =================================================================
 *
 * 【消息格式：固定头 + 可变负载】
 *
 *   每条消息：
 *     - sender_id  : 发送者 task_id（自动填）
 *     - type       : 消息类型（用户自定义，比如 1=LOG, 2=CALC）
 *     - payload_len: 实际负载字节数（0 ~ IPC_MAX_PAYLOAD）
 *     - payload    : 负载数据（最多 IPC_MAX_PAYLOAD 字节）
 *
 *   小消息直接拷贝（< 64B），不需要共享内存。
 *   大消息超过 IPC_MAX_PAYLOAD 由调用方分片（教学内核简化）。
 *
 *   ┌────────────────────────────────────────────────┐
 *   │  sender_id (8B)  │  type (8B)  │  len (8B)  │
 *   ├────────────────────────────────────────────────┤
 *   │  payload [0..63]  (64B max)                    │
 *   └────────────────────────────────────────────────┘
 *
 *   总大小：~88 字节。kmalloc 一个 struct ipc_message 即可。
 *
 * =================================================================
 *
 * 【阻塞 vs 非阻塞 vs 超时】
 *
 *   ipc_send           — 阻塞直到入队成功
 *   ipc_try_send       — 不阻塞，队满立即返回 -IPC_ERR_WOULDBLOCK
 *   ipc_send_timeout   — 阻塞最多 N tick，超时返回 -IPC_ERR_TIMEDOUT
 *
 *   recv 同理。
 *
 *   【为什么默认是阻塞】
 *     内核任务的常见模式：
 *       while (1) {
 *           ipc_recv(ch, &type, buf, &cap);
 *           handle(type, buf);
 *       }
 *     阻塞语义让 server 不用忙等，让出 CPU 给其他任务。
 *
 * =================================================================
 *
 * 【同步唤醒链（典型 RPC）】
 *
 *   client                    server (logger)
 *   ──────                    ────────────────
 *   ipc_send(LOG, "hi")  ──►  ipc_recv 醒来，拿到 "hi"
 *   阻塞等 reply            打印 "hi"
 *                          ipc_send(REPLY, "ok")  ──►  client 醒来，拿到 "ok"
 *
 *   单个 channel 可以双向用（FIFO 保证顺序）。
 *   实际 RPC 一般 client 建一个临时 reply channel，server 回复到那里。
 *   教学项目里复用同一 channel 也行（顺序保证）。
 *
 * =================================================================
 *
 * 【错误码（返回值）】
 *
 *   0                   — 成功
 *   -IPC_ERR_INVAL      — channel id 无效 / 参数非法
 *   -IPC_ERR_WOULDBLOCK — try_* 变体：操作会阻塞，未执行
 *   -IPC_ERR_TIMEDOUT   — timeout 变体：等待超时
 *   -IPC_ERR_FULL       — 队列满（已废弃，用 WOULDBLOCK）
 *   -IPC_ERR_NOMEM      — kmalloc 失败
 *   -IPC_ERR_CLOSED     — channel 已销毁（发送时还在等）
 *   -IPC_ERR_TOOLONG   — payload 太长（> IPC_MAX_PAYLOAD）
 *
 *   返回值约定和 Linux 类似：负数表示错误，0 表示成功。
 * ================================================================ */

#ifndef KERNEL_IPC_H
#define KERNEL_IPC_H

#include <kernel/types.h>

/* ---------------------------------------------------------------
 * 通道 ID 类型
 *
 *   通道 ID 是对外的句柄（用户态将来也用这种 ID）。
 *   0 表示"无效通道"（类似 POSIX 的 -1 fd）。
 *
 *   【为什么从 1 开始】
 *     0 留作"无效"标记，方便初始化和错误检查。 */
typedef u64 ipc_channel_id_t;

#define IPC_INVALID_CHANNEL  ((ipc_channel_id_t)0)

/* ---------------------------------------------------------------
 * 等待类型枚举（task_struct.wait_kind）
 *
 *   这些是 task_struct.wait_kind 字段的合法取值。
 *   放这里而不是 task.h 是因为它们是 IPC 概念，
 *   只有 IPC 代码读写这个字段。
 * --------------------------------------------------------------- */
#define IPC_WAIT_NONE  0   /* 不在 IPC 等待 */
#define IPC_WAIT_SEND  1   /* 在 ipc_send 中阻塞（channel 满了） */
#define IPC_WAIT_RECV  2   /* 在 ipc_recv 中阻塞（channel 空了） */

/* ---------------------------------------------------------------
 * 错误码
 *
 *   返回值负数 = 错误，0 = 成功。
 *   不要直接返回 -1，要返回这些有意义的码。
 *
 *   【为什么用宏而不是 enum】
 *     内核没有 enum 跨文件可靠性的保证（不同编译器选项可能影响），
 *     宏简单稳定。 */
#define IPC_OK             0
#define IPC_ERR_INVAL     -1   /* 参数非法 / channel 不存在 */
#define IPC_ERR_WOULDBLOCK -2  /* 非阻塞调用：操作会阻塞 */
#define IPC_ERR_TIMEDOUT  -3   /* timeout 变体：等待超时 */
#define IPC_ERR_NOMEM     -4   /* kmalloc 失败 */
#define IPC_ERR_CLOSED    -5   /* channel 已销毁 */
#define IPC_ERR_TOOLONG   -6   /* payload 超过 IPC_MAX_PAYLOAD */
#define IPC_ERR_BUSY      -7   /* 通道忙（保留，未用） */

/* ---------------------------------------------------------------
 * 最大通道数
 *
 *   32 个通道对教学内核够用：
 *     - 几个 server 的服务通道
 *     - 几个 client 的临时回复通道
 *     - 留余量
 *
 *   静态数组方便遍历，O(N) 查找够快。
 * --------------------------------------------------------------- */
#define IPC_MAX_CHANNELS  32

/* ---------------------------------------------------------------
 * 默认通道容量
 *
 *   channel 创建时如果不指定 capacity，用这个值。
 *   8 条消息 = 8 × 88B = 704B，对小内核是合理的延迟线。
 *
 *   【为什么 8】
 *     - 太小（如 1）：纯 rendezvous，sender 和 receiver 必须同时在场
 *     - 太大（如 1000）：占内存多，且会让 sender 远超 receiver
 *     - 8 平衡：可吸收短时突发，又不让 receiver 跟不上 */
#define IPC_DEFAULT_CAPACITY 8

/* ---------------------------------------------------------------
 * 最大负载字节数
 *
 *   64 字节够放：
 *     - 一行日志（短）
 *     - 一个 RPC 请求（op + 两个参数 + 一个返回值）
 *     - 几个 capability 句柄（将来 Lesson 7 用）
 *
 *   大消息走"共享内存 + 控制消息"模式（不在本课实现）。
 * --------------------------------------------------------------- */
#define IPC_MAX_PAYLOAD   64

/* ---------------------------------------------------------------
 * 默认 RPC 超时（教学 demo 用）
 * --------------------------------------------------------------- */
#define IPC_DEFAULT_TIMEOUT_TICKS  500   /* 5 秒 */

/* ---------------------------------------------------------------
 * ipc_init — 初始化 IPC 子系统
 *
 *   - 清空 channel_table
 *   - 重置 next_channel_id
 *
 *   必须在 sched_init 之后调用（IPC 依赖调度器）。
 *   必须在 kernel_mm_init 之后调用（IPC 依赖 kmalloc）。 */
void ipc_init(void);

/* ---------------------------------------------------------------
 * ipc_channel_create — 创建一个消息通道
 *
 * 参数：
 *   name     — 通道名（最长 15 字符，调试用）
 *   capacity — 队列容量（0 = 纯 rendezvous；<0 用默认）
 *
 * 返回值：
 *   > 0 — 通道 ID（IPC_INVALID_CHANNEL 表示失败）
 *   == IPC_INVALID_CHANNEL — 失败（表满）
 *
 * 行为：
 *   1. 从 channel_table 找一个空槽
 *   2. 填字段（id, name, capacity, msg_head/tail = NULL）
 *   3. 标 in_use
 *
 * 【为什么 capacity = 0 也合法】
 *   capacity 0 = 没有缓冲，发送必须等接收者来 → 纯同步 IPC
 *   这是 L4 风格的"rendezvous"，对教学有意义。
 *
 * 【为什么不在 kmalloc 上动态分配 channel】
 *   静态数组让遍历快（stats / destroy_all）、
 *   避免 channel 自身的内存管理复杂化。
 *   32 个槽位对教学项目够用。 */
ipc_channel_id_t ipc_channel_create(const char *name, s64 capacity);

/* ---------------------------------------------------------------
 * ipc_channel_destroy — 销毁通道
 *
 *   - 标 channel 不再 in_use
 *   - 释放所有排队中的消息
 *   - 唤醒所有阻塞的 sender / recv_waiter（设 ipc_result = -IPC_ERR_CLOSED）
 *
 * 【为什么不立即 kfree channel】
 *   channel 是静态数组里的元素，不能 kfree。
 *   标 in_use=0 让下次 create 时复用这个槽位。 */
int ipc_channel_destroy(ipc_channel_id_t id);

/* ---------------------------------------------------------------
 * ipc_send — 阻塞发送消息
 *
 * 参数：
 *   id      — 通道 ID
 *   type    — 消息类型（用户自定义）
 *   payload — 负载指针（NULL 表示空消息）
 *   len     — 负载字节数（0 ~ IPC_MAX_PAYLOAD）
 *
 * 返回值：
 *   0                  — 成功
 *   -IPC_ERR_INVAL     — channel 不存在
 *   -IPC_ERR_TOOLONG   — len > IPC_MAX_PAYLOAD
 *   -IPC_ERR_NOMEM     — kmalloc 失败
 *   -IPC_ERR_CLOSED    — 等待期间 channel 被销毁
 *
 * 行为：
 *   1. 校验参数
 *   2. kmalloc 一个 struct ipc_message 拷贝数据
 *   3. 关中断进入临界区：
 *      a. 若有 recv_waiter：直接把消息写进它的 ipc_buf，
 *         设 ipc_out_*  字段，sched_wake 它（零拷贝直送）
 *      b. 否则若 msg_count < capacity：入队
 *      c. 否则：current 进 send_waiters，sched_yield
 *   4. 返回时 ipc_result 反映成功/超时/关闭
 *
 * 【为什么 (a) 优先】
 *   - 直接 handoff 避免一次入队 + 一次出队的拷贝
 *   - 接收方立即被唤醒，延迟最低
 *   - 经典 L4/L3 优化
 *
 * 【关于 payload 拷贝】
 *   调用方传 payload 指针，IPC 内部 memcpy 到消息结构。
 *   这是值拷贝语义，发送方返回后改 payload 不影响已发出的消息。
 *   这避免了"发送后释放"的 use-after-free 问题。 */
int ipc_send(ipc_channel_id_t id, u64 type, const void *payload, u64 len);

/* ---------------------------------------------------------------
 * ipc_recv — 阻塞接收消息
 *
 * 参数：
 *   id      — 通道 ID
 *   out_type — [out] 消息类型（NULL 表示不关心）
 *   buf     — [out] 接收缓冲区
 *   in_out_cap — [in/out] 入参是 buf 容量，出参是实际长度
 *
 * 返回值：
 *   0                  — 成功（*in_out_cap 是实际收到的字节数）
 *   -IPC_ERR_INVAL     — channel 不存在 / buf 为 NULL
 *   -IPC_ERR_TIMEDOUT  — timeout 变体超时
 *   -IPC_ERR_CLOSED    — 等待期间 channel 被销毁
 *
 * 行为（对称于 send）：
 *   1. 校验参数
 *   2. 关中断进入临界区：
 *      a. 若 msg_count > 0：取队头消息，拷贝到 buf，kfree 消息，
 *         若有 send_waiter：sched_wake 它（让它能继续入队）
 *      b. 否则：current 进 recv_waiters，sched_yield
 *
 * 【in_out_cap 截断】
 *   如果消息负载 > buf 容量，截断到容量大小，但 ipc_out_len 是原始 len。
 *   调用方应检查 *in_out_cap <= 自己分配的容量，否则数据被截断。
 *
 * 【关于 out_sender】
 *   接收方可以通过 task_id 知道消息来自哪个任务，做"reply-to-sender"。
 *   我们通过 task->ipc_out_sender 暴露，由调用方读 task->task_id 自己拿
 *   （接口更简单：不增加一个 out 参数）。 */
int ipc_recv(ipc_channel_id_t id, u64 *out_type, void *buf, u64 *in_out_cap);

/* ---------------------------------------------------------------
 * ipc_try_send / ipc_try_recv — 非阻塞变体
 *
 *   行为同 send/recv，但若会阻塞，立即返回 -IPC_ERR_WOULDBLOCK。
 *
 *   适用：
 *     - 中断处理程序里不能阻塞
 *     - 探测式调用：先试，不行就 busy-wait 或 yield 再试 */
int ipc_try_send(ipc_channel_id_t id, u64 type, const void *payload, u64 len);
int ipc_try_recv(ipc_channel_id_t id, u64 *out_type, void *buf, u64 *in_out_cap);

/* ---------------------------------------------------------------
 * ipc_send_timeout / ipc_recv_timeout — 带超时的阻塞变体
 *
 *   参数 timeout_ticks：
 *     0  — 等价于 try_*（不阻塞）
 *     >0 — 最多等 N 个 tick，超时返回 -IPC_ERR_TIMEDOUT
 *
 *   实现：设置 current->ipc_timeout_tick = now + timeout_ticks
 *   然后调内部阻塞版本。wake_sleeping_tasks 检测到时把任务唤醒，
 *   设 ipc_result = -IPC_ERR_TIMEDOUT。 */
int ipc_send_timeout(ipc_channel_id_t id, u64 type, const void *payload, u64 len, u64 timeout_ticks);
int ipc_recv_timeout(ipc_channel_id_t id, u64 *out_type, void *buf, u64 *in_out_cap, u64 timeout_ticks);

/* ---------------------------------------------------------------
 * ipc_stats — 打印所有 channel 状态（调试用）
 *
 *   格式：
 *     ID   NAME       CAP  MSGS  SEND_WAIT  RECV_WAIT
 *     1    logger     8    3     0          1
 *     2    calc       4    0     0          0
 * --------------------------------------------------------------- */
void ipc_stats(void);

/* ================================================================
 * 【Lesson 7 新增】Cap 层使用的内部 API
 *
 *   L6 的 ipc_send / ipc_recv 吃 channel id（u64 句柄），
 *   L7 的 cap 层需要"拿 channel 指针直接操作"的版本，避免：
 *     (a) cap 层每次操作都要 find_channel(id) → 多一次 O(N) 查找
 *     (b) cap 层的 channel id 来源还是 cap lookup 出来的 channel 指针
 *
 *   解决：把 ipc_send/recv 的核心逻辑抽出"内部版"，参数是 channel 指针。
 *   原 ipc_send/recv 改为"find_channel(id) → 调内部版"的薄壳。
 *
 *   【为什么不在 cap.c 里直接操作 channel 结构】
 *     - 会让 cap.c 依赖 ipc.c 的内部 struct ipc_channel（破坏封装）
 *     - IPC 内部状态（msg_head, send_waiters 等）是 IPC 模块私有
 *     - 通过这些内部 API 暴露"channel 指针 + 操作"是干净的接口
 *
 *   【cap 随消息传递】
 *     - ipc_send_with_cap 多带一个 cap_snap 参数
 *     - 接收方在 deliver_to_waiter / dequeue 路径把快照拷到
 *       task->ipc_recv_cap_* 字段
 *     - ipc_recv_on_channel 拿到消息后让 caller 知道"有没有 cap"
 * ================================================================ */

/* channel 指针类型（forward declaration，不暴露 struct ipc_channel） */
struct ipc_channel;

/* cap 快照（cap 随消息传递的载体） */
struct ipc_cap_snapshot {
    u32  type;        /* CAP_TYPE_* */
    u32  rights;      /* CAP_RIGHT_* 位或 */
    void *object;     /* underlying 对象指针 */
    u64  lineage;     /* 族系 ID */
};

/* 根据 id 查找 channel 指针（暴露给 cap 层） */
struct ipc_channel *ipc_channel_lookup(ipc_channel_id_t id);

/* 拿 channel 指针对应的 id（cap 层调 ipc_channel_destroy 时用） */
ipc_channel_id_t ipc_channel_id_of(struct ipc_channel *c);

/* channel 指针版的 send/recv —— 内部 API */
int ipc_send_on_channel(struct ipc_channel *c, u64 type,
                         const void *payload, u64 len);
int ipc_recv_on_channel(struct ipc_channel *c, u64 *out_type,
                         void *buf, u64 *in_out_cap);

/* 非阻塞版 */
int ipc_try_send_on_channel(struct ipc_channel *c, u64 type,
                             const void *payload, u64 len);
int ipc_try_recv_on_channel(struct ipc_channel *c, u64 *out_type,
                             void *buf, u64 *in_out_cap);

/* 超时版 */
int ipc_send_timeout_on_channel(struct ipc_channel *c, u64 type,
                                 const void *payload, u64 len,
                                 u64 timeout_ticks);
int ipc_recv_timeout_on_channel(struct ipc_channel *c, u64 *out_type,
                                 void *buf, u64 *in_out_cap,
                                 u64 timeout_ticks);

/* 发消息时附带 cap 快照 */
int ipc_send_with_cap(struct ipc_channel *c, u64 type,
                       const void *payload, u64 len,
                       const struct ipc_cap_snapshot *cap_snap);

#endif /* KERNEL_IPC_H */
