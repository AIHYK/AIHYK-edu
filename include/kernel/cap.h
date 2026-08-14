/* ================================================================
 * kernel/cap.h — Capability 框架（架构无关接口）
 *
 * 【Lesson 7 核心新增】
 *
 * AIHYK 是 Hybrid Kernel。L6 完成了 IPC：通道 + 阻塞消息传递。
 * 但光有 IPC 不够 —— 谁能在哪个通道上 send / recv？谁能销毁通道？
 * 谁能再分发权限？这是"访问控制"问题。
 *
 * Capability 是"把权限当作可传递对象"的访问控制模型：
 *   - 每个 cap 是一个 (type, rights, object) 三元组
 *   - 持有 cap = 持有对该 object 的某种权限
 *   - cap 可以从 A 传递到 B（IPC 携带），可以"克制地"复制给 B
 *
 * =================================================================
 *
 * 【Capability vs ACL —— 为什么不用 ACL】
 *
 *   ACL（访问控制表）：每个 object 上挂一张表，列出"谁能对它做什么"。
 *     例：channel X 的 ACL = { T1: read, T2: read+write }
 *
 *   问题：
 *     (a) 检查权限时要查 object 的 ACL，不知道 T1 有哪些权限（reverse 查询难）
 *     (b) 想把"对 X 的读权限"暂时给一个新任务，得改 X 的 ACL（修改全局状态）
 *     (c) 想撤销"我给出去的所有权限"，得遍历所有 object 的 ACL
 *     (d) ACL 不太能跨机器/进程传递（"我是 T1" 在另一个 namespace 里无意义）
 *
 *   Capability（能力）：每个 task 持有一张 cap 表，列出"我能对哪些 object 做什么"。
 *     例：T1 的 cap table = { (channel X, RECV), (channel Y, SEND) }
 *
 *   优势：
 *     (a) 查"我能对 X 做什么" → 在自己的 cap 表里找 X（O(1)~O(slots)）
 *     (b) 想把"对 X 的读权限"给一个新任务 → mint 一份新 cap 给它（局部操作）
 *     (c) 想撤销"我给出去的所有权限" → 按 lineage 追溯，一次 revoke（见下）
 *     (d) cap 可以作为消息附件随 IPC 传递（"借我用一下你的 read 权限"）
 *
 *   现代系统里 seL4 / Fuchsia / Barrelfish 都用 capability。
 *
 * =================================================================
 *
 * 【Mint —— 委托与"权限单调下降"】
 *
 *   cap_mint(src_slot, dst_task, new_rights)：
 *     - 把 src cap 复制一份到 dst_task 的 cap 表
 *     - 复制时只能"减少"权限，不能"增加"
 *
 *   为什么必须单调下降？
 *     - 假设 T1 持有 cap (X, RECV|MINT|SEND)，它 mint 给 T2 一个
 *       (X, RECV|SEND)。T2 得到的权限 ⊆ T1 的权限。
 *     - 如果 T2 能 mint 出 (X, RECV|MINT|SEND)（超出自己持有的），
 *       那委托就毫无意义 —— 任何人都能"凭空"获得没给他的权限。
 *     - 单调下降保证：从 root 出发，任何权限链路上，
 *       下游任务持有的权限 ⊆ 上游任务持有的权限。
 *
 *   类比：root 用户能"sudo"，但 sudo 给的子用户不能比 root 还强。
 *
 *   实现要点：
 *     - mint 时检查 new_rights ⊆ src.rights（位与）
 *     - 否则返回 -CAP_ERR_NORIGHT
 *
 * =================================================================
 *
 * 【Revoke —— 基于 lineage 的"传递性删除"】
 *
 *   每个 cap 有一个 lineage（族系）字段，u64 整数。
 *     - root cap 的 lineage 是它创建时分配的唯一 ID
 *     - mint 出来的 cap 继承 src 的 lineage（不是新分配的）
 *
 *   cap_revoke(src_slot)：
 *     - 把当前任务里 src_slot 对应的那个 cap 删了
 *     - 同时遍历【所有任务】的 cap 表，把 lineage 相同的 cap 全删了
 *     - 但 src 自己不删（它就是要被 revoke 的源头）
 *     - 实际上："revoke src" 意思是"把我 mint 出去的那批 cap 收回"
 *
 *   为什么用 lineage 而不是树结构？
 *     - 树结构需要父指针，每次 mint 要存父 cap 的指针，revoke 要递归遍历子树
 *     - lineage 是 u64 整数，比较相等就行，O(N*M) 遍历简单可靠
 *     - 教学项目里 N（任务数）≤ 16，M（cap 槽位）≤ 32，N*M = 512 次比较，ns 级
 *     - seL4 也用类似的 cspace 模型（更复杂，支持 cap tree）
 *
 *   为什么 revoke 不删 src 自己？
 *     - revoke 的语义是"撤销我委托出去的"，不是"销毁自己的"
 *     - 销毁自己的用 cap_delete
 *
 * =================================================================
 *
 * 【Cap 随 IPC 传递 —— "快照"模型】
 *
 *   场景：file-server 想给 client 一个"临时读权限"，让它回复 ACK。
 *
 *   方案 A（引用传递）：在消息里塞一个 src_slot，client 收到后通过
 *     src_slot 反查 server 的 cap 表 —— 跨地址空间，行不通（教学内核
 *     还是单地址空间，但概念上不对）。
 *
 *   方案 B（拷贝 cap 内容）：在消息里塞一个 (type, rights, object, lineage)
 *     四元组快照。client 收到后在自己的 cap 表里安装一份新 cap。
 *     - 简单：和 IPC 消息一并序列化
 *     - 但"快照" ≠ "原始 cap"：server 之后 revoke，client 那份"快照"
 *       不会被回收（除非也按 lineage 追溯 —— 实际上会，因为 lineage 一致）
 *
 *   L7 用方案 B（快照），简单可靠。
 *
 *   实现：
 *     - ipc_send_with_cap(chan, type, payload, len, cap_snap)
 *         cap_snap 是 (type, rights, object, lineage) 四元组
 *     - ipc_recv_with_cap 从 current 的字段读出快照，安装到自己的 cap 表
 *
 *   【关于 object 指针的安全】
 *     - object 指针是 channel 指针，属于内核数据
 *     - 快照里包含 object，client 装到自己的 cap 表后用这个 object
 *       做 cap_send/recv —— 内核代码所以指针可信任
 *     - 用户态实现里 object 应该是"内核句柄"而不是裸指针，防止用户伪造
 *     - 教学内核是内核态 cap，简化为裸指针
 *
 * =================================================================
 *
 * 【和 L6 IPC 的关系】
 *
 *   cap 层建立在 L6 channel 层之上：
 *     - cap_send(slot, ...) → cap_lookup_check(slot, SEND) →
 *       ipc_send_on_channel(channel_ptr, ...)
 *     - cap_recv(slot, ...) → cap_lookup_check(slot, RECV) →
 *       ipc_recv_on_channel(channel_ptr, ...)
 *
 *   L6 的 ipc_send / ipc_recv 直接吃 channel id，没有权限检查；
 *   L7 的 cap_send / cap_recv 吃 cap slot，先查权限再用 channel。
 *
 *   类比：
 *     - L6 像Unix 早期的 fd —— 谁拿到 fd 就能用
 *     - L7 像现代 capability 系统 —— 必须先有 cap 才能用
 *
 *   L6 的 ipc.c 同时暴露了"channel 指针版"的内部 API（L7 加进去的）：
 *     ipc_channel_lookup / ipc_send_on_channel / ipc_recv_on_channel / ...
 *   这些内部 API 让 cap 层不必知道 channel 数据结构内部细节，
 *   只要拿到 channel 指针就能操作。
 * =================================================================
 *
 * 【错误码（返回值）】
 *
 *   0                  — 成功
 *   -CAP_ERR_INVAL     — 参数非法 / slot 越界
 *   -CAP_ERR_NORIGHT   — 权限不足（mint 时 new_rights ⊄ src.rights 等）
 *   -CAP_ERR_NOMEM     — kmalloc 失败
 *   -CAP_ERR_NOTFOUND  — slot 未使用 / cap 不存在
 *   -CAP_ERR_TYPE      — 类型不匹配（用 channel cap 当 file cap 等）
 *   -CAP_ERR_TOOLONG   — payload 超长（send 路径）
 * ================================================================ */

#ifndef KERNEL_CAP_H
#define KERNEL_CAP_H

#include <kernel/types.h>
#include <kernel/ipc.h>

/* ---------------------------------------------------------------
 * Capability slot 类型
 *
 *   cap_slot_t 是 cap 在 CSpace 中的索引（数组下标）。
 *   用 u64 是为了和 IPC 的 channel id / future user-handle 兼容。
 *
 *   【为什么不是 int】
 *     - 64 位内核，u64 是"通用句柄类型"
 *     - 未来用户态接口可能把 cap slot 直接当 64 位 handle 暴露 */
typedef u64 cap_slot_t;

/* 0 表示"无效 slot"（类似 IPC_INVALID_CHANNEL）。
 * 注意：cap slot 0 在数组里是合法位置，但约定上不使用，
 * 让"无效 cap"和"未初始化 cap"用同一个值。 */
#define CAP_INVALID_SLOT  ((cap_slot_t)0)

/* 每个任务的 CSpace 槽位数。
 *
 * 32 个槽位对教学内核够用：
 *   - 几个 channel cap（send + recv + mint + destruct）
 *   - 几个临时 mint 出来的 cap（demo 用）
 *
 * 静态数组方便遍历（revoke / stats）。 */
#define CAP_SLOTS_PER_TASK  32

/* ---------------------------------------------------------------
 * Capability 类型
 *
 *   目前只有 channel cap。未来可扩展：
 *     - CAP_TYPE_PAGE  ：内存页 cap（共享内存）
 *     - CAP_TYPE_TASK  ：任务控制 cap（kill / suspend）
 *     - CAP_TYPE_IRQ   ：中断 cap（让用户态驱动接管 IRQ） */
#define CAP_TYPE_NONE     0   /* 未使用 */
#define CAP_TYPE_CHANNEL  1   /* 通道权限（L7 实现） */

/* ---------------------------------------------------------------
 * 权限位
 *
 *   SEND    : 可以向 channel 发消息（ipc_send）
 *   RECV    : 可以从 channel 收消息（ipc_recv）
 *   MINT    : 可以 mint（委托）出新的 cap
 *   DESTRUCT: 可以销毁 underlying channel（ipc_channel_destroy）
 *
 *   【为什么 MINT 单独一位】
 *     - 持有 SEND+RECV 不等于能委托
 *     - 类比：能 read 文件 ≠ 能 chmod 给别人 read 权限
 *
 *   【为什么 DESTRUCT 单独一位】
 *     - 销毁 channel 影响所有持有该 channel cap 的任务
 *     - 是高危操作，单独一位避免误触 */
#define CAP_RIGHT_SEND     (1u << 0)   /* 0x01 */
#define CAP_RIGHT_RECV     (1u << 1)   /* 0x02 */
#define CAP_RIGHT_MINT     (1u << 2)   /* 0x04 */
#define CAP_RIGHT_DESTRUCT (1u << 3)   /* 0x08 */
#define CAP_RIGHT_ALL      (CAP_RIGHT_SEND | CAP_RIGHT_RECV \
                          | CAP_RIGHT_MINT | CAP_RIGHT_DESTRUCT)

/* ---------------------------------------------------------------
 * 错误码 */
#define CAP_OK           0
#define CAP_ERR_INVAL   (-1)   /* 参数非法 */
#define CAP_ERR_NORIGHT (-2)   /* 权限不足 */
#define CAP_ERR_NOMEM   (-3)   /* kmalloc 失败 */
#define CAP_ERR_NOTFOUND (-4)  /* slot 未使用 */
#define CAP_ERR_TYPE    (-5)   /* 类型不匹配 */
#define CAP_ERR_TOOLONG (-6)   /* payload 太长 */
#define CAP_ERR_CSPACE_FULL (-7)  /* 【C2 修复】接收方 cspace 满，附带 cap 被丢弃 */

/* ---------------------------------------------------------------
 * struct cap — 单个 capability
 *
 *   in_use  : 此槽位是否占用
 *   type    : CAP_TYPE_CHANNEL 等
 *   rights  : CAP_RIGHT_* 位或
 *   object  : 指向 underlying 对象（channel 指针）
 *             用 void* 避免循环依赖（kernel/cap.h 不 include ipc_channel 结构）
 *   lineage : 族系 ID，revoke 时追溯用
 *
 *   【lineage 的设计】
 *     - root cap 创建时分配全局唯一 lineage（递增计数器）
 *     - mint 出来的 cap 继承 src 的 lineage
 *     - revoke(src) 时遍历所有 task 的所有 cap，把 lineage 相同的删掉
 *     - 简单可靠，比父子树结构省内存
 *
 *   【为什么 in_use 是 int 而不是 bit】
 *     - 数组里每个元素单独可读，调试方便
 *     - 和 IPC channel 的 in_use 字段一致 */
struct cap {
    int  in_use;     /* 0 = 空槽，1 = 占用 */
    u32  type;       /* CAP_TYPE_* */
    u32  rights;      /* CAP_RIGHT_* 位或 */
    void *object;     /* underlying 对象指针（如 channel） */
    u64  lineage;     /* 族系 ID（revoke 追溯用） */
};

/* ---------------------------------------------------------------
 * struct cspace — Capability Space（一个任务的全部 cap）
 *
 *   slots   : 固定大小数组，每个槽位一个 cap
 *   num_used: 已使用槽位数（统计用，加快"是否还有 cap"判断）
 *
 *   【C5 修复】原文档说"cspace 是 task_struct 的字段"，实际不是——
 *   task_struct 里是 `struct cspace *cspace` 指针（见 arch/include/arch/task.h），
 *   struct cspace 本体由 cap_cspace_init() 通过 kmalloc(sizeof(struct cspace))
 *   单独分配（kernel/cap.c:192），cap_cspace_destroy() 时 kfree（cap/c:229）。
 *   生命周期：随 task 创建而 kmalloc，随 task 退出（reaper）而 kfree。
 *
 *   【为什么 cspace 是指针 + kmalloc，而不是 task_struct 内嵌字段】
 *     - 内嵌字段会让 task_struct 多 1280 字节（CAP_SLOTS_PER_TASK=32
 *       × sizeof(struct cap)=40 = 1280B），即使任务不用 cap 也占内存
 *     - 指针 + 按需 kmalloc：只在任务实际需要 cspace 时（sched_create_task
 *       调 cap_cspace_init）才分配，未初始化的任务 cspace=NULL
 *     - 缺点：多一次 kmalloc/kfree，多一次指针解引用
 *     - 教学项目可接受；seL4 等用更紧凑的 cap tree
 *
 *   【为什么 cap 表是 per-task 而不是全局】
 *     - capability 的核心是"任务持有哪些权限"
 *     - 全局表 + owner 字段也行，但语义上 cspace 属于 task
 *     - seL4 的 cspace_t 也是 per-thread 的
 *
 *   【为什么静态数组而不是动态链表】
 *     - 32 个槽位 = 32 × 40 字节 = 1280 字节，小
 *     - 数组遍历快（revoke / stats）
 *     - 固定 layout，调试方便 */
struct cspace {
    struct cap slots[CAP_SLOTS_PER_TASK];
    u64 num_used;
};

/* ================================================================
 * API 函数声明
 * ================================================================ */

/* ---------------------------------------------------------------
 * cap_init_subsystem — 初始化 cap 子系统全局状态
 *
 *   - 重置 lineage 计数器
 *   - 打印初始化日志
 *
 *   必须在 sched_init + ipc_init 之后调用（cap 依赖 current 和 channel）。 */
void cap_init_subsystem(void);

/* ---------------------------------------------------------------
 * cap_cspace_init — 为一个任务初始化 CSpace
 *
 *   - kmalloc(sizeof(struct cspace)) 分配 cspace 本体
 *   - 把所有 slots 标 in_use=0
 *   - num_used = 0
 *   - t->cspace 指向新分配的 cspace
 *
 *   在 sched_create_task 里调用（任务一被创建就有空 CSpace）。
 *   返回 CAP_ERR_NOMEM 如果 kmalloc 失败。 */
int cap_cspace_init(struct task_struct *t);

/* ---------------------------------------------------------------
 * cap_cspace_destroy — 清理一个任务的 CSpace
 *
 *   - 遍历 slots，对每个 in_use 的 cap 不做 channel 销毁
 *     （channel 由 cap_destroy_channel 显式销毁）
 *   - 把所有 slots 标 in_use=0
 *
 *   在 task_reaper 里调用，任务退出时清理 cap 表。
 *   注意：不销毁 underlying channel —— 那是 cap_destroy_channel 的活。 */
void cap_cspace_destroy(struct task_struct *t);

/* ---------------------------------------------------------------
 * cap_channel_create — 创建一个 channel 并安装 root cap
 *
 * 参数：
 *   name    — channel 名字（传给 ipc_channel_create）
 *   capacity — 队列容量（传给 ipc_channel_create）
 *   rights  — root cap 的权限（通常是 CAP_RIGHT_ALL）
 *
 * 返回值：
 *   > 0 — cap slot 编号（CAP_INVALID_SLOT 表示失败）
 *
 * 行为：
 *   1. 调用 ipc_channel_create 拿到 channel id
 *   2. 调用 ipc_channel_lookup 拿到 channel 指针
 *   3. 在 current 的 CSpace 里找一个空 slot
 *   4. 安装 cap：type=CHANNEL, rights=rights, object=chan_ptr,
 *      lineage=新分配的全局唯一 ID
 *
 * 【为什么 root cap 拿全权限】
 *   - root cap 是这个 channel 的"创建者"
 *   - 创建者应该能做任何事（包括销毁、委托）
 *   - 后续 mint 出去的 cap 权限从 root 派生 */
cap_slot_t cap_channel_create(const char *name, s64 capacity, u32 rights);

/* ---------------------------------------------------------------
 * cap_lookup_check — 查找 cap 并检查权限
 *
 * 参数：
 *   slot         — cap slot 编号
 *   needed_rights — 需要的权限（CAP_RIGHT_* 位或）
 *
 * 返回值：
 *   非 NULL — 找到的 cap 指针（已确认 in_use 和权限）
 *   NULL    — 找不到 / 权限不足
 *
 *   内部使用：cap_send / cap_recv / cap_mint 等都先调这个拿到 cap。 */
struct cap *cap_lookup_check(cap_slot_t slot, u32 needed_rights);

/* ---------------------------------------------------------------
 * cap_mint — 委托（mint）一个新 cap 到另一个任务
 *
 * 参数：
 *   src_slot    — 源 cap（必须在 current 的 CSpace）
 *   dst_task    — 目标任务（必须存在）
 *   new_rights  — 新 cap 的权限（必须 ⊆ src.rights）
 *
 * 返回值：
 *   >= 0 — 新 cap 在 dst_task CSpace 里的 slot 编号
 *   < 0  — 失败（错误码）
 *
 * 行为：
 *   1. 检查 src cap 存在 + 持有 MINT 权限
 *   2. 检查 new_rights ⊆ src.rights（单调下降）
 *   3. 在 dst_task 的 CSpace 找空 slot
 *   4. 安装新 cap：继承 src.type / object / lineage
 *      rights = new_rights
 *
 * 【单调下降】
 *   (new_rights & ~src.rights) != 0 说明 new_rights 有 src 不具备的位
 *   → 拒绝 */
s64 cap_mint(cap_slot_t src_slot, struct task_struct *dst_task, u32 new_rights);

/* ---------------------------------------------------------------
 * cap_delete — 删除 current 的一个 cap
 *
 *   只删 cap 表项，不动 underlying channel。
 *   channel 由 cap_destroy_channel 处理。 */
int cap_delete(cap_slot_t slot);

/* ---------------------------------------------------------------
 * cap_revoke — 撤销从 src 派生出去的所有 cap
 *
 *   1. 查找 src cap（必须在 current 的 CSpace）
 *   2. 拿到 src.lineage
 *   3. 遍历【所有任务】的 CSpace，删除 lineage 相同的 cap（src 自己除外）
 *
 *   【为什么遍历所有任务】
 *     - mint 出去的 cap 在其他任务的 CSpace 里
 *     - 只有遍历所有任务才能找到它们
 *
 *   【为什么 src 自己不删】
 *     - revoke 是"撤销委托"，不是"销毁自己"
 *     - src 持有者调 revoke 是想"把我给出去的收回来"
 *     - src 自己保留，可以再次 mint 给别人 */
int cap_revoke(cap_slot_t src_slot);

/* ---------------------------------------------------------------
 * cap_send — 通过 cap 向 channel 发消息（阻塞）
 *
 *   1. cap_lookup_check(slot, CAP_RIGHT_SEND)
 *   2. ipc_send_on_channel(channel_ptr, type, payload, len) */
int cap_send(cap_slot_t slot, u64 type, const void *payload, u64 len);

/* ---------------------------------------------------------------
 * cap_recv — 通过 cap 从 channel 收消息（阻塞） */
int cap_recv(cap_slot_t slot, u64 *out_type, void *buf, u64 *in_out_cap);

/* ---------------------------------------------------------------
 * cap_send_with_cap — 发消息时附带一个 cap 快照
 *
 * 参数：
 *   chan_slot    — 通过哪个 channel cap 发（必须 SEND 权限）
 *   type, payload, len — 消息内容
 *   transfer_cap_slot — 要传递的 cap（必须在 current 的 CSpace，不需要 MINT）
 *
 *   【为什么不需要 MINT 权限】
 *     - 传递 cap 不等于 mint 出一个新的 cap
 *     - mint 是"我把权限委托给你，从此你独立持有"
 *     - 传递是"借我用一下你，但实际上是接收方安装一份快照"
 *     - 持有 SEND 就能传递任意自己的 cap（不增加权限，receiver 拿到的
 *       权限 ⊆ sender 持有的 —— 因为快照是 sender 自己的 cap 拷贝）
 *     - 简化教学：只要持有 SEND 就能附带传递任意自己的 cap
 *
 *   如果 receiver 持有的权限 ⊆ sender 自己持有的，那是 sender 自己
 *   "自愿给"的，符合委托精神。 */
int cap_send_with_cap(cap_slot_t chan_slot, u64 type,
                       const void *payload, u64 len,
                       cap_slot_t transfer_cap_slot);

/* ---------------------------------------------------------------
 * cap_recv_with_cap — 收消息并安装附带的 cap 快照
 *
 *   1. cap_lookup_check(slot, CAP_RIGHT_RECV)
 *   2. ipc_recv_on_channel 拿到消息 + cap 快照
 *   3. 如果消息附带 cap，把快照安装到 current 的 CSpace 一个空 slot
 *   4. 通过 *out_cap_slot 返回新 slot 编号
 *
 *   返回值：
 *     CAP_OK               — 成功（*out_cap_slot = CAP_INVALID_SLOT 表示消息没附带 cap）
 *     CAP_ERR_CSPACE_FULL  — 【C2】消息已收到但附带的 cap 因 cspace 满未安装
 *                            （buf 数据有效，*out_cap_slot = CAP_INVALID_SLOT）
 *     其他 < 0             — 错误（权限/类型/IPC 错误） */
int cap_recv_with_cap(cap_slot_t slot, u64 *out_type, void *buf,
                       u64 *in_out_cap, cap_slot_t *out_cap_slot);

/* ---------------------------------------------------------------
 * cap_destroy_channel — 销毁 underlying channel
 *
 *   1. cap_lookup_check(slot, CAP_RIGHT_DESTRUCT)
 *   2. ipc_channel_destroy(channel_id)
 *   3. 遍历所有任务的 CSpace，把指向同一 channel 的 cap 全清掉
 *      （防止悬空引用） */
int cap_destroy_channel(cap_slot_t slot);

/* ---------------------------------------------------------------
 * cap_stats — 打印 current 任务的 CSpace 内容 */
void cap_stats(void);

/* ---------------------------------------------------------------
 * cap_total_caps — 统计所有任务的 cap 总数
 *
 *   遍历所有任务的所有 cap 槽，返回 in_use=1 的总数。
 *   调试用，看 cap 是否泄漏。 */
int cap_total_caps(void);

#endif /* KERNEL_CAP_H */
