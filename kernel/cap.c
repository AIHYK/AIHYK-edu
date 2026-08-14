/* ================================================================
 * kernel/cap.c — Capability 框架实现
 *
 * 【Lesson 7 核心新增】
 *
 * 实现 include/kernel/cap.h 的接口。
 *
 * 核心数据结构：
 *
 *   ┌────────────────────────────────────────────────────────────┐
 *   │  每个 task_struct 有一个 struct cspace 指针 (cspace 字段)    │
 *   │  cspace.slots[CAP_SLOTS_PER_TASK] 是该任务的所有 cap        │
 *   │  cspace.num_used 是已使用槽位数（统计用）                   │
 *   └────────────────────────────────────────────────────────────┘
 *
 *   全局 lineage 计数器（cap_lineage_counter）：
 *     - 每次 cap_channel_create 创建 root cap 时分配新 lineage
 *     - 后续 mint 派生的 cap 继承 src 的 lineage
 *     - cap_revoke 按 lineage 找出"我派生出去的所有 cap"
 *
 * 操作流程：
 *
 *   cap_channel_create(name, cap, rights)
 *     │
 *     ├─ ipc_channel_create → 拿到 channel id
 *     ├─ ipc_channel_lookup  → 拿到 channel 指针
 *     ├─ 在 current 的 cspace 找空 slot
 *     └─ 安装 cap：type=CHANNEL, rights=rights, object=chan_ptr,
 *                lineage=新分配的全局唯一 ID
 *
 *   cap_send(slot, type, payload, len)
 *     │
 *     ├─ cap_lookup_check(slot, CAP_RIGHT_SEND) → 拿到 cap
 *     ├─ channel_ptr = (struct ipc_channel *)cap->object
 *     └─ ipc_send_on_channel(channel_ptr, type, payload, len)
 *
 *   cap_recv(slot, ...)
 *     └─ 同 cap_send，但查 RECV 权限 + 调 ipc_recv_on_channel
 *
 *   cap_mint(src_slot, dst_task, new_rights)
 *     │
 *     ├─ 查 src cap：必须存在 + 持有 MINT 权限
 *     ├─ 检查 new_rights ⊆ src.rights (单调下降)
 *     ├─ 在 dst_task 的 cspace 找空 slot
 *     └─ 安装新 cap：继承 src.type / object / lineage，
 *                  rights = new_rights
 *
 *   cap_revoke(src_slot)
 *     │
 *     ├─ 查 src cap：必须在 current 的 cspace
 *     ├─ 拿到 src.lineage
 *     └─ 遍历所有任务的所有 cap 槽，把 lineage 相同的 cap 清掉
 *        （src 自己除外）
 *
 *   cap_send_with_cap(chan_slot, ..., transfer_cap_slot)
 *     │
 *     ├─ 查 chan cap：必须 SEND 权限
 *     ├─ 查 transfer cap：必须存在（不需要 MINT）
 *     ├─ 构造 cap_snap 快照（type/rights/object/lineage）
 *     └─ ipc_send_with_cap(channel_ptr, ..., &cap_snap)
 *
 *   cap_recv_with_cap(slot, ...)
 *     │
 *     ├─ 查 RECV 权限
 *     ├─ ipc_recv_on_channel 拿到消息 + current->ipc_recv_cap_*
 *     └─ 如果 has_cap：在 current cspace 找空 slot 安装新 cap
 * ================================================================ */

#include <arch/console.h>
#include <arch/cpu.h>
#include <arch/task.h>
#include <kernel/cap.h>
#include <kernel/ipc.h>
#include <kernel/mm.h>
#include <kernel/panic.h>
#include <kernel/sched.h>
#include <kernel/types.h>

/* ---------------------------------------------------------------
 * 全局状态
 * --------------------------------------------------------------- */

/* lineage 计数器（全局递增）
 *
 *   每个 root cap 分配一个唯一的 lineage。
 *   mint 出来的 cap 继承 src 的 lineage，不分配新的。
 *
 *   从 1 开始：0 表示"无效 lineage"。 */
static u64 cap_lineage_counter = 0;

/* cap 子系统是否已初始化 */
static int cap_initialized = 0;

/* ---------------------------------------------------------------
 * 局部工具：打印数字（避免依赖其他模块的 static）
 * --------------------------------------------------------------- */
static void cap_print_dec(u64 v) {
    char buf[21];
    int i = 0;
    if (v == 0) { arch_console_putchar('0'); return; }
    while (v > 0 && i < 20) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i > 0) arch_console_putchar(buf[--i]);
}

static void cap_print_hex(u64 v) {
    char buf[17];
    int i = 0;
    const char *hex = "0123456789ABCDEF";
    if (v == 0) { arch_console_print("0x0"); return; }
    while (v > 0 && i < 16) {
        buf[i++] = hex[v & 0xF];
        v >>= 4;
    }
    arch_console_print("0x");
    while (i > 0) arch_console_putchar(buf[--i]);
}

/* 把权限位格式化成字符串 "SMRD"（4 个字母） */
static void cap_print_rights(u32 rights) {
    arch_console_putchar((rights & CAP_RIGHT_SEND)     ? 'S' : '-');
    arch_console_putchar((rights & CAP_RIGHT_RECV)     ? 'R' : '-');
    arch_console_putchar((rights & CAP_RIGHT_MINT)     ? 'M' : '-');
    arch_console_putchar((rights & CAP_RIGHT_DESTRUCT) ? 'D' : '-');
}

/* 把 cap type 转成短字符串 */
static const char *cap_type_name(u32 type) {
    switch (type) {
        case CAP_TYPE_NONE:    return "NONE   ";
        case CAP_TYPE_CHANNEL: return "CHANNEL";
        default:               return "UNKNOWN";
    }
}

/* ---------------------------------------------------------------
 * cap_init_subsystem — 初始化 cap 子系统全局状态
 *
 *   - 重置 lineage 计数器
 *   - 标 initialized = 1
 *
 *   在 kernel_main 里 ipc_init 之后调用。
 *   不分配内存（cspace 是 task_struct 的字段，随 task 一起分配）。 */
void cap_init_subsystem(void) {
    cap_lineage_counter = 1;   /* 从 1 开始：0 表示无效 lineage */
    cap_initialized = 1;

    /* 【Lesson 7】init task 的 cspace 也要初始化。
     *   init task 是静态分配的（init_task in sched.c），sched_init 不会调
     *   cap_cspace_init（只对动态创建的任务调）。这里手动初始化一次。
     *   init task 持有 root cap（demo 的 secure-srv channel 创建权）。 */
    if (current != NULL && current->cspace == NULL) {
        cap_cspace_init(current);
    }

    /* Capability subsystem initialized — silent for clean output */
}

/* ---------------------------------------------------------------
 * cap_cspace_init — 为任务初始化 CSpace
 *
 *   - 清空所有 slots (in_use=0)
 *   - num_used = 0
 *
 *   在 sched_create_task 里调用。
 *   不分配堆内存（cspace 是 task_struct 的字段，struct cspace
 *   作为 kmalloc 出来的 task 的一部分被分配）。
 *
 *   返回值：0 总是成功（没有可失败的操作）。
 *
 *   【为什么 cspace 是 task_struct 的字段而不是单独 kmalloc】
 *     - 简化生命周期：cspace 随 task 一起分配/释放
 *     - 减少 kmalloc 调用（每个 task 一次 kmalloc 就够）
 *     - 缺点：cspace 占用 task_struct 大小（CAP_SLOTS_PER_TASK=32
 *       × sizeof(struct cap)=40 = 1280 字节，每任务多 1.3KB）
 *     - 教学项目可接受；seL4 等用更紧凑的 cap tree */
int cap_cspace_init(struct task_struct *t) {
    if (t == NULL) {
        return CAP_ERR_INVAL;
    }
    /* cspace 是 task_struct 的字段，它在 task_struct 分配时就有了内存。
     * 这里只清零。
     * 注意：sched_create_task 在调用本函数前会把 cspace=NULL
     * 标记为 L7 字段初始值。但实际 cspace 字段就是 task_struct 的
     * 一个 struct cspace（嵌入而非指针）。
     *
     * 但 task_struct.cspace 字段类型是 `struct cspace *`（指针）！
     * 所以需要 kmalloc 一个 struct cspace，让指针指向它。 */

    struct cspace *cs = (struct cspace *)kmalloc(sizeof(struct cspace));
    if (cs == NULL) {
        return CAP_ERR_NOMEM;
    }

    for (int i = 0; i < CAP_SLOTS_PER_TASK; i++) {
        cs->slots[i].in_use  = 0;
        cs->slots[i].type    = CAP_TYPE_NONE;
        cs->slots[i].rights  = 0;
        cs->slots[i].object  = NULL;
        cs->slots[i].lineage = 0;
    }
    cs->num_used = 0;

    t->cspace = cs;
    return CAP_OK;
}

/* ---------------------------------------------------------------
 * cap_cspace_destroy — 清理任务的 CSpace
 *
 *   - kfree(t->cspace)（如果是 kmalloc 出来的）
 *   - t->cspace = NULL
 *
 *   【为什么不销毁 underlying channel】
 *     - channel 由 cap_destroy_channel 显式销毁
 *     - 任务退出时只清自己的 cap 表，不动 channel
 *     - 防止任务退出误删共享 channel */
void cap_cspace_destroy(struct task_struct *t) {
    if (t == NULL) return;
    if (t->cspace == NULL) return;

    /* 统计 in_use cap 数（调试用，将来可加日志） */
    u64 used = t->cspace->num_used;
    (void)used;   /* 防止 -Wunused-but-set-variable */

    /* 简单 kfree：cap 表里只有指针和整数，没有需要递归释放的 */
    kfree(t->cspace);
    t->cspace = NULL;
}

/* ---------------------------------------------------------------
 * 局部：在 cspace 找一个空 slot
 *
 *   返回 >= 1 的 slot 编号（slot 0 不用，约定为 invalid）。
 *   找不到返回 -1。
 *
 *   【为什么从 slot 1 开始】
 *     - CAP_INVALID_SLOT = 0
 *     - 让"无效 slot"和"未使用 slot"用同一个值
 *     - 实际 cspace.slots[0] 数组位置永远不被使用 */
static int find_free_slot(struct cspace *cs) {
    if (cs == NULL) return -1;
    for (int i = 1; i < CAP_SLOTS_PER_TASK; i++) {
        if (!cs->slots[i].in_use) {
            return i;
        }
    }
    return -1;
}

/* ---------------------------------------------------------------
 * 局部：在指定 task 的 cspace 安装一个 cap
 *
 *   不检查权限，只负责"找空 slot + 填字段"。
 *   返回 slot 编号，< 0 表示失败。 */
static int cap_install(struct task_struct *t, u32 type, u32 rights,
                        void *object, u64 lineage) {
    if (t == NULL || t->cspace == NULL) {
        return -1;
    }

    int slot = find_free_slot(t->cspace);
    if (slot < 0) {
        return -1;
    }

    struct cap *c = &t->cspace->slots[slot];
    c->in_use  = 1;
    c->type    = type;
    c->rights  = rights;
    c->object  = object;
    c->lineage = lineage;

    t->cspace->num_used++;
    return slot;
}

/* ---------------------------------------------------------------
 * 局部：清空指定 task 的 cspace 里某个 slot
 *   只清字段，不 kfree（cspace 本身的内存在 task 销毁时释放） */
static void cap_clear_slot(struct task_struct *t, int slot) {
    if (t == NULL || t->cspace == NULL) return;
    if (slot < 0 || slot >= CAP_SLOTS_PER_TASK) return;

    struct cap *c = &t->cspace->slots[slot];
    if (!c->in_use) return;

    c->in_use  = 0;
    c->type    = CAP_TYPE_NONE;
    c->rights  = 0;
    c->object  = NULL;
    c->lineage = 0;

    if (t->cspace->num_used > 0) {
        t->cspace->num_used--;
    }
}

/* ---------------------------------------------------------------
 * cap_channel_create — 创建一个 channel 并安装 root cap
 *
 *   行为：
 *     1. ipc_channel_create(name, capacity) → 拿到 channel id
 *     2. ipc_channel_lookup(id) → 拿到 channel 指针
 *     3. 分配 lineage（全局递增）
 *     4. 在 current 的 cspace 找空 slot
 *     5. 安装 cap：type=CHANNEL, rights=rights, object=chan_ptr,
 *                  lineage=刚分配的
 *
 *   返回 slot 编号（>0），CAP_INVALID_SLOT 表示失败。 */
cap_slot_t cap_channel_create(const char *name, s64 capacity, u32 rights) {
    if (!cap_initialized) {
        panic(__FILE__, __LINE__, "cap_channel_create before cap_init_subsystem");
    }
    if (current == NULL || current->cspace == NULL) {
        return CAP_INVALID_SLOT;
    }

    /* 1. 创建 channel */
    ipc_channel_id_t cid = ipc_channel_create(name, capacity);
    if (cid == IPC_INVALID_CHANNEL) {
        return CAP_INVALID_SLOT;
    }

    /* 2. 拿 channel 指针 */
    struct ipc_channel *chan = ipc_channel_lookup(cid);
    if (chan == NULL) {
        /* 不应该发生：刚 create 出来的 id 一定查得到 */
        return CAP_INVALID_SLOT;
    }

    /* 3. 分配 lineage */
    u64 lineage = cap_lineage_counter++;
    if (cap_lineage_counter == 0) {
        /* 溢出保护（u64 极不可能溢出，但防御性） */
        cap_lineage_counter = 1;
    }

    /* 4-5. 安装 cap */
    int slot = cap_install(current, CAP_TYPE_CHANNEL, rights,
                            (void *)chan, lineage);
    if (slot < 0) {
        /* cspace 满：销毁 channel 撤销 */
        ipc_channel_destroy(cid);
        return CAP_INVALID_SLOT;
    }

    return (cap_slot_t)slot;
}

/* ---------------------------------------------------------------
 * cap_lookup_check — 查找 cap 并检查权限
 *
 *   返回非 NULL = 找到的 cap 指针。
 *   返回 NULL = 找不到 / 权限不足。
 *
 *   【为什么同时检查 in_use 和权限】
 *     - 调用方每次都要做这两步，封装一次省事
 *     - 错误码丢失（不知道是 in_use 还是权限），但简单 */
struct cap *cap_lookup_check(cap_slot_t slot, u32 needed_rights) {
    if (current == NULL || current->cspace == NULL) {
        return NULL;
    }
    if (slot == CAP_INVALID_SLOT) {
        return NULL;
    }
    if (slot >= CAP_SLOTS_PER_TASK) {
        return NULL;
    }

    struct cap *c = &current->cspace->slots[slot];
    if (!c->in_use) {
        return NULL;
    }
    /* 权限检查：needed_rights 必须是 c->rights 的子集 */
    if ((c->rights & needed_rights) != needed_rights) {
        return NULL;
    }
    return c;
}

/* ---------------------------------------------------------------
 * cap_mint — 委托一个新 cap 到另一个任务
 *
 *   1. 查 src cap：在 current 的 cspace，必须 in_use + 持有 MINT 权限
 *   2. 检查 new_rights ⊆ src.rights
 *      (new_rights & ~src.rights) != 0 说明 new_rights 有 src 不具备的位
 *   3. 在 dst_task 的 cspace 找空 slot
 *   4. 安装新 cap：继承 src.type / object / lineage，rights = new_rights
 *
 *   返回 slot 编号（>=1）或错误码 (<0) */
s64 cap_mint(cap_slot_t src_slot, struct task_struct *dst_task, u32 new_rights) {
    if (dst_task == NULL || dst_task->cspace == NULL) {
        return CAP_ERR_INVAL;
    }

    /* 1. 查 src cap：必须在 current 的 cspace + 持有 MINT */
    struct cap *src = cap_lookup_check(src_slot, CAP_RIGHT_MINT);
    if (src == NULL) {
        return CAP_ERR_NORIGHT;
    }

    /* 2. 单调下降：new_rights ⊆ src.rights */
    if ((new_rights & ~src->rights) != 0) {
        return CAP_ERR_NORIGHT;
    }

    /* 3-4. 在 dst_task 安装新 cap（继承 src.type / object / lineage） */
    int slot = cap_install(dst_task, src->type, new_rights,
                            src->object, src->lineage);
    if (slot < 0) {
        return CAP_ERR_NOMEM;
    }
    return (s64)slot;
}

/* ---------------------------------------------------------------
 * cap_delete — 删除 current 的一个 cap
 *
 *   只清 cap 表项，不动 underlying channel。
 *   channel 由 cap_destroy_channel 处理。 */
int cap_delete(cap_slot_t slot) {
    if (current == NULL || current->cspace == NULL) {
        return CAP_ERR_INVAL;
    }
    if (slot == CAP_INVALID_SLOT || slot >= CAP_SLOTS_PER_TASK) {
        return CAP_ERR_INVAL;
    }
    if (!current->cspace->slots[slot].in_use) {
        return CAP_ERR_NOTFOUND;
    }
    cap_clear_slot(current, (int)slot);
    return CAP_OK;
}

/* ---------------------------------------------------------------
 * cap_revoke — 撤销从 src 派生出去的所有 cap
 *
 *   1. 查 src cap：必须在 current 的 cspace
 *   2. 拿到 src.lineage
 *   3. 遍历所有任务的所有 cap 槽，把 lineage 相同的 cap 清掉
 *      （src 自己除外）
 *
 *   【为什么遍历所有任务】
 *     - mint 出去的 cap 在其他任务的 cspace 里
 *     - 只有遍历所有任务才能找到它们
 *
 *   【为什么 src 自己不删】
 *     - revoke 是"撤销委托"，不是"销毁自己"
 *     - src 持有者调 revoke 是想"把我给出去的收回来"
 *     - src 自己保留，可以再次 mint 给别人 */
int cap_revoke(cap_slot_t src_slot) {
    if (current == NULL || current->cspace == NULL) {
        return CAP_ERR_INVAL;
    }
    if (src_slot == CAP_INVALID_SLOT || src_slot >= CAP_SLOTS_PER_TASK) {
        return CAP_ERR_INVAL;
    }

    u64 flags = arch_irq_save();

    struct cap *src = &current->cspace->slots[src_slot];
    if (!src->in_use) {
        arch_irq_restore(flags);
        return CAP_ERR_NOTFOUND;
    }

    u64 lineage = src->lineage;
    int revoked_count = 0;

    /* 遍历所有任务 */
    int total = sched_num_tasks();
    for (int i = 0; i < total; i++) {
        struct task_struct *t = sched_get_task_by_index(i);
        if (t == NULL) continue;
        if (t->cspace == NULL) continue;

        /* 遍历该任务的所有 cap slot */
        for (int s = 1; s < CAP_SLOTS_PER_TASK; s++) {
            struct cap *c = &t->cspace->slots[s];
            if (!c->in_use) continue;

            /* 跳过 src 自己（用 task 指针 + slot 双重判断） */
            if (t == current && s == (int)src_slot) continue;

            if (c->lineage == lineage) {
                cap_clear_slot(t, s);
                revoked_count++;
            }
        }
    }

    arch_irq_restore(flags);
    return revoked_count;   /* 返回撤销的数量 */
}

/* ---------------------------------------------------------------
 * cap_send — 通过 cap 向 channel 发消息（阻塞）
 *
 *   1. cap_lookup_check(slot, CAP_RIGHT_SEND) → 拿到 cap
 *   2. channel_ptr = (struct ipc_channel *)cap->object
 *   3. ipc_send_on_channel(channel_ptr, type, payload, len) */
int cap_send(cap_slot_t slot, u64 type, const void *payload, u64 len) {
    struct cap *c = cap_lookup_check(slot, CAP_RIGHT_SEND);
    if (c == NULL) {
        return CAP_ERR_NORIGHT;
    }
    if (c->type != CAP_TYPE_CHANNEL) {
        return CAP_ERR_TYPE;
    }
    struct ipc_channel *chan = (struct ipc_channel *)c->object;
    return ipc_send_on_channel(chan, type, payload, len);
}

/* ---------------------------------------------------------------
 * cap_recv — 通过 cap 从 channel 收消息（阻塞） */
int cap_recv(cap_slot_t slot, u64 *out_type, void *buf, u64 *in_out_cap) {
    struct cap *c = cap_lookup_check(slot, CAP_RIGHT_RECV);
    if (c == NULL) {
        return CAP_ERR_NORIGHT;
    }
    if (c->type != CAP_TYPE_CHANNEL) {
        return CAP_ERR_TYPE;
    }
    struct ipc_channel *chan = (struct ipc_channel *)c->object;
    return ipc_recv_on_channel(chan, out_type, buf, in_out_cap);
}

/* ---------------------------------------------------------------
 * cap_send_with_cap — 发消息时附带一个 cap 快照
 *
 *   1. 查 chan cap：必须 SEND 权限
 *   2. 查 transfer cap：必须存在（不需要 MINT —— transferring 不是 minting）
 *   3. 构造 cap_snap 快照
 *   4. ipc_send_with_cap(channel_ptr, ..., &cap_snap)
 *
 *   【为什么不需要 MINT】
 *     - 传递 cap 不等于 mint 出一个新的 cap
 *     - mint 是"我把权限委托给你，从此你独立持有"
 *     - 传递是"借我用一下，receiver 装一份快照"
 *     - 持有 SEND 就能传递任意自己的 cap（不增加权限） */
int cap_send_with_cap(cap_slot_t chan_slot, u64 type,
                       const void *payload, u64 len,
                       cap_slot_t transfer_cap_slot) {
    /* 1. 查 chan cap + SEND */
    struct cap *chan_cap = cap_lookup_check(chan_slot, CAP_RIGHT_SEND);
    if (chan_cap == NULL) {
        return CAP_ERR_NORIGHT;
    }
    if (chan_cap->type != CAP_TYPE_CHANNEL) {
        return CAP_ERR_TYPE;
    }

    /* 2. 查 transfer cap：必须存在（不需要特定权限） */
    if (transfer_cap_slot == CAP_INVALID_SLOT
        || transfer_cap_slot >= CAP_SLOTS_PER_TASK) {
        return CAP_ERR_INVAL;
    }
    struct cap *tcap = &current->cspace->slots[transfer_cap_slot];
    if (!tcap->in_use) {
        return CAP_ERR_NOTFOUND;
    }

    /* 3. 构造快照 */
    struct ipc_cap_snapshot snap;
    snap.type    = tcap->type;
    snap.rights  = tcap->rights;
    snap.object  = tcap->object;
    snap.lineage = tcap->lineage;

    /* 4. 发送 */
    struct ipc_channel *chan = (struct ipc_channel *)chan_cap->object;
    return ipc_send_with_cap(chan, type, payload, len, &snap);
}

/* ---------------------------------------------------------------
 * cap_recv_with_cap — 收消息并安装附带的 cap 快照
 *
 *   1. cap_lookup_check(slot, CAP_RIGHT_RECV)
 *   2. ipc_recv_on_channel 拿到消息 + current->ipc_recv_cap_*
 *   3. 如果 has_cap：在 current 的 cspace 找空 slot 安装新 cap
 *   4. 通过 *out_cap_slot 返回新 slot 编号
 *
 *   返回值：
 *     0    — 成功
 *     < 0  — 错误
 *     *out_cap_slot = CAP_INVALID_SLOT 表示消息没附带 cap */
int cap_recv_with_cap(cap_slot_t slot, u64 *out_type, void *buf,
                       u64 *in_out_cap, cap_slot_t *out_cap_slot) {
    if (out_cap_slot == NULL) {
        return CAP_ERR_INVAL;
    }
    *out_cap_slot = CAP_INVALID_SLOT;

    struct cap *c = cap_lookup_check(slot, CAP_RIGHT_RECV);
    if (c == NULL) {
        return CAP_ERR_NORIGHT;
    }
    if (c->type != CAP_TYPE_CHANNEL) {
        return CAP_ERR_TYPE;
    }

    struct ipc_channel *chan = (struct ipc_channel *)c->object;
    int rc = ipc_recv_on_channel(chan, out_type, buf, in_out_cap);
    if (rc != IPC_OK) {
        return rc;
    }

    /* 检查是否附带 cap */
    if (current->ipc_recv_cap_has_cap) {
        int new_slot = cap_install(current,
                                    current->ipc_recv_cap_type,
                                    current->ipc_recv_cap_rights,
                                    current->ipc_recv_cap_object,
                                    current->ipc_recv_cap_lineage);
        if (new_slot >= 0) {
            *out_cap_slot = (cap_slot_t)new_slot;
        }
        /* 即使安装失败（cspace 满）也返回 IPC_OK：
         * 消息已收到，只是 cap 没装上 */
    }

    /* 清字段：本次 recv 结束，下次开始时是干净的 */
    current->ipc_recv_cap_has_cap = 0;
    current->ipc_recv_cap_type    = 0;
    current->ipc_recv_cap_rights   = 0;
    current->ipc_recv_cap_object  = NULL;
    current->ipc_recv_cap_lineage = 0;

    return CAP_OK;
}

/* ---------------------------------------------------------------
 * cap_destroy_channel — 销毁 underlying channel
 *
 *   1. cap_lookup_check(slot, CAP_RIGHT_DESTRUCT)
 *   2. ipc_channel_destroy(channel_id)
 *   3. 遍历所有任务的 cspace，把指向同一 channel 的 cap 全清掉
 *      （防止悬空引用）
 *
 *   【为什么遍历所有任务】
 *     - 持有该 channel 的 cap 可能在多个任务里
 *     - channel 销毁后这些 cap 都成悬空引用
 *     - 必须全清，否则后续 cap_send 会 deref 已 kfree 的 channel */
int cap_destroy_channel(cap_slot_t slot) {
    u64 flags = arch_irq_save();

    struct cap *c = cap_lookup_check(slot, CAP_RIGHT_DESTRUCT);
    if (c == NULL) {
        arch_irq_restore(flags);
        return CAP_ERR_NORIGHT;
    }
    if (c->type != CAP_TYPE_CHANNEL) {
        arch_irq_restore(flags);
        return CAP_ERR_TYPE;
    }

    struct ipc_channel *chan = (struct ipc_channel *)c->object;
    ipc_channel_id_t cid = ipc_channel_id_of(chan);

    /* 先记下 channel 指针，destroy 后 in_use=0 但指针仍有效
     * （channel 在静态数组里，destroy 只是标 in_use=0，不 kfree） */
    void *chan_ptr = (void *)chan;

    /* 销毁 channel：唤醒所有阻塞的 senders/receivers */
    int rc = ipc_channel_destroy(cid);

    /* 遍历所有任务，清掉指向这个 channel 的所有 cap */
    int total = sched_num_tasks();
    for (int i = 0; i < total; i++) {
        struct task_struct *t = sched_get_task_by_index(i);
        if (t == NULL) continue;
        if (t->cspace == NULL) continue;

        for (int s = 1; s < CAP_SLOTS_PER_TASK; s++) {
            struct cap *cc = &t->cspace->slots[s];
            if (!cc->in_use) continue;
            if (cc->type == CAP_TYPE_CHANNEL && cc->object == chan_ptr) {
                cap_clear_slot(t, s);
            }
        }
    }

    arch_irq_restore(flags);
    return rc;
}

/* ---------------------------------------------------------------
 * cap_stats — 打印 current 任务的 CSpace 内容 */
void cap_stats(void) {
    arch_console_set_color(CON_COLOR_CYAN);
    arch_console_print("\nCapability Stats (task ");
    cap_print_dec(current ? current->task_id : 0);
    arch_console_print(", ");
    if (current && current->name[0]) {
        arch_console_print(current->name);
    } else {
        arch_console_print("(unnamed)");
    }
    arch_console_print("):\n");
    arch_console_set_color(CON_COLOR_DEFAULT);

    arch_console_print("  SLOT  TYPE     RIGHTS  OBJECT             LINEAGE\n");

    if (current == NULL || current->cspace == NULL) {
        arch_console_set_color(CON_COLOR_RED);
        arch_console_print("  (no cspace)\n");
        arch_console_set_color(CON_COLOR_DEFAULT);
        return;
    }

    u64 flags = arch_irq_save();

    int used = 0;
    for (int s = 1; s < CAP_SLOTS_PER_TASK; s++) {
        struct cap *c = &current->cspace->slots[s];
        if (!c->in_use) continue;
        used++;

        arch_console_print("  ");
        cap_print_dec((u64)s);
        arch_console_print("    ");
        arch_console_print(cap_type_name(c->type));
        arch_console_print("  ");
        cap_print_rights(c->rights);
        arch_console_print("  ");
        cap_print_hex((u64)c->object);
        arch_console_print("  ");
        cap_print_dec(c->lineage);
        arch_console_print("\n");
    }

    arch_console_print("\n  Used slots: ");
    cap_print_dec((u64)used);
    arch_console_print(" / ");
    cap_print_dec((u64)CAP_SLOTS_PER_TASK);
    arch_console_print("\n");

    arch_irq_restore(flags);
}

/* ---------------------------------------------------------------
 * cap_total_caps — 统计所有任务的 cap 总数 */
int cap_total_caps(void) {
    int total_caps = 0;
    u64 flags = arch_irq_save();

    int total = sched_num_tasks();
    for (int i = 0; i < total; i++) {
        struct task_struct *t = sched_get_task_by_index(i);
        if (t == NULL) continue;
        if (t->cspace == NULL) continue;

        for (int s = 1; s < CAP_SLOTS_PER_TASK; s++) {
            if (t->cspace->slots[s].in_use) {
                total_caps++;
            }
        }
    }

    arch_irq_restore(flags);
    return total_caps;
}
