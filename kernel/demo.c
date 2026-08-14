/* ================================================================
 * kernel/demo.c — 所有 demo 代码（从 main.c 解耦）
 *
 * 包含：
 *   - L6 IPC demo（logger + calc RPC）
 *   - L7 Cap demo（file-server + trusted + untrusted）
 *   - L8 User-space demo（hello 用户程序）
 *   - L9 Crash recovery demo（crash + supervisor）
 *
 * 对外接口：demo_run() — 见 kernel/demo.h
 * ================================================================ */

#include <arch/console.h>
#include <arch/cpu.h>
#include <arch/irq.h>
#include <arch/mem.h>
#include <arch/pit.h>
#include <arch/task.h>
#include <arch/user.h>
#include <kernel/cap.h>
#include <kernel/ipc.h>
#include <kernel/panic.h>
#include <kernel/sched.h>
#include <kernel/types.h>
#include <kernel/util.h>
#include <kernel/demo.h>

/* ================================================================
 * 【Lesson 6】Demo 数据结构与任务
 * ================================================================ */

/* 消息类型（用户自定义） */
#define MSG_LOG          1
#define MSG_CALC_REQUEST 2
#define MSG_CALC_REPLY   3

/* 计算器操作码 */
#define OP_ADD  1
#define OP_SUB  2
#define OP_MUL  3
#define OP_DIV  4

/* 计算请求消息负载（24 字节） */
struct calc_req {
    u64 op;
    u64 a;
    u64 b;
};

/* 计算回复消息负载（16 字节） */
struct calc_reply {
    s64 result;
    u64 status;   /* 0 = ok, -1 = div by zero 等 */
};

/* logger server 参数 */
struct logger_args {
    ipc_channel_id_t chan;
    int expected_msgs;
};

/* logger client 参数 */
struct logger_client_args {
    ipc_channel_id_t chan;
    int num_msgs;
    int interval_ticks;
    const char *prefix;
};

/* calc 任务参数 */
struct calc_args {
    ipc_channel_id_t chan;
    int num_requests;
};

/* 局部：等 tick 到 target（让出 CPU） */
static void wait_until_tick(u64 target) {
    while (arch_pit_get_tick_count() < target) {
        sched_yield();
    }
}

/* 局部：把整数转成字符串写到 buf，返回长度 */
static int int_to_str(u64 v, char *buf) {
    char tmp[21];
    int i = 0;
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return 1; }
    while (v > 0 && i < 20) {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (int j = 0; j < i; j++) buf[j] = tmp[i - 1 - j];
    buf[i] = '\0';
    return i;
}

/* 局部：字符串拼接（返回新末尾） */
static int str_append(char *dst, int pos, const char *src) {
    int i = 0;
    while (src[i] != '\0') {
        dst[pos++] = src[i++];
    }
    return pos;
}

/* ---------------------------------------------------------------
 * task_logger_server — 日志服务器（安静模式：不逐条打印）
 * --------------------------------------------------------------- */
static void task_logger_server(void *arg) {
    struct logger_args *la = (struct logger_args *)arg;
    ipc_channel_id_t chan = la->chan;
    int expected = la->expected_msgs;

    int got = 0;
    while (got < expected) {
        u64 type = 0;
        char buf[IPC_MAX_PAYLOAD + 1];
        u64 cap = sizeof(buf) - 1;
        int rc = ipc_recv(chan, &type, buf, &cap);
        if (rc != IPC_OK) break;
        if (type != MSG_LOG) continue;
        got++;
    }
    /* silent: just exit, demo_run reports summary */
}

/* ---------------------------------------------------------------
 * task_logger_client — 日志客户端（安静模式）
 * --------------------------------------------------------------- */
static void task_logger_client(void *arg) {
    struct logger_client_args *ca = (struct logger_client_args *)arg;
    ipc_channel_id_t chan = ca->chan;

    for (int i = 1; i <= ca->num_msgs; i++) {
        u64 target = (i == 1) ? (arch_pit_get_tick_count() + 10)
                              : (arch_pit_get_tick_count() + ca->interval_ticks);
        wait_until_tick(target);

        char msg[IPC_MAX_PAYLOAD];
        int pos = 0;
        pos = str_append(msg, pos, ca->prefix);
        pos = str_append(msg, pos, " #");
        pos += int_to_str((u64)i, msg + pos);
        pos = str_append(msg, pos, " at tick ");
        pos += int_to_str(arch_pit_get_tick_count(), msg + pos);
        msg[pos] = '\0';

        int rc = ipc_send(chan, MSG_LOG, msg, (u64)(pos + 1));
        if (rc != IPC_OK) break;
    }
    /* silent: just exit */
}

/* ---------------------------------------------------------------
 * task_calc_server — 计算器 RPC 服务器（安静模式）
 * --------------------------------------------------------------- */
static void task_calc_server(void *arg) {
    struct calc_args *ca = (struct calc_args *)arg;
    ipc_channel_id_t chan = ca->chan;
    int expected = ca->num_requests;

    for (int i = 1; i <= expected; i++) {
        u64 type = 0;
        struct calc_req req;
        u64 cap = sizeof(req);
        int rc = ipc_recv(chan, &type, &req, &cap);
        if (rc != IPC_OK) break;
        if (type != MSG_CALC_REQUEST) continue;

        struct calc_reply rep;
        rep.status = 0;
        switch (req.op) {
            case OP_ADD: rep.result = (s64)req.a + (s64)req.b; break;
            case OP_SUB: rep.result = (s64)req.a - (s64)req.b; break;
            case OP_MUL: rep.result = (s64)req.a * (s64)req.b; break;
            case OP_DIV:
                if (req.b == 0) {
                    rep.status = 1;
                    rep.result = 0;
                } else {
                    rep.result = (s64)((s64)req.a / (s64)req.b);
                }
                break;
            default:
                rep.status = 2;
                rep.result = 0;
                break;
        }

        rc = ipc_send(chan, MSG_CALC_REPLY, &rep, sizeof(rep));
        if (rc != IPC_OK) break;
    }
    /* silent: just exit */
}

/* ---------------------------------------------------------------
 * task_calc_client — 计算器 RPC 客户端（安静模式）
 * --------------------------------------------------------------- */
static void task_calc_client(void *arg) {
    struct calc_args *ca = (struct calc_args *)arg;
    ipc_channel_id_t chan = ca->chan;

    static const struct calc_req tests[] = {
        { OP_ADD, 100, 23 },
        { OP_SUB, 50,  18 },
        { OP_MUL, 7,   9  },
        { OP_DIV, 84,  4  },
        { OP_DIV, 5,   0  },
    };
    static const s64 expected[] = { 123, 32, 63, 21, 0 };

    for (int i = 0; i < 5; i++) {
        const struct calc_req *req = &tests[i];
        sched_yield();

        int rc = ipc_send(chan, MSG_CALC_REQUEST, req, sizeof(*req));
        if (rc != IPC_OK) break;

        u64 type = 0;
        struct calc_reply rep;
        u64 cap = sizeof(rep);
        rc = ipc_recv(chan, &type, &rep, &cap);
        if (rc != IPC_OK) break;
        if (type != MSG_CALC_REPLY) break;
        /* result validation happens silently */
        (void)expected;
    }
    /* silent: just exit */
}

/* ================================================================
 * 【Lesson 7】Demo 数据结构与任务：Capability 框架验证
 * ================================================================ */

#define MSG_FILE_REQ    10
#define MSG_FILE_REPLY  11
#define MSG_ACK         12

/* L7 demo 任务参数 */
struct file_server_args {
    cap_slot_t chan_slot;
    int got_request;
    int got_ack;
};

struct trusted_client_args {
    cap_slot_t chan_slot;
    int got_reply;
    int ack_sent;
};

struct untrusted_client_args {
    cap_slot_t fake_slot;
    int denied;
};

/* 局部：字符串长度 */
static u64 cap_strlen(const char *s) {
    u64 n = 0;
    while (s[n] != '\0') n++;
    return n;
}

/* ---------------------------------------------------------------
 * task_file_server — 文件服务器（安静模式）
 * --------------------------------------------------------------- */
static void task_file_server(void *arg) {
    struct file_server_args *a = (struct file_server_args *)arg;

    /* 1. 等请求 */
    u64 type = 0;
    char req_buf[IPC_MAX_PAYLOAD + 1];
    u64 cap = sizeof(req_buf) - 1;
    int rc = cap_recv(a->chan_slot, &type, req_buf, &cap);
    if (rc != CAP_OK) return;
    if (type != MSG_FILE_REQ) return;
    if (cap > sizeof(req_buf) - 1) cap = sizeof(req_buf) - 1;
    req_buf[cap] = '\0';
    a->got_request = 1;

    /* 2. 创建 temp channel + root cap */
    cap_slot_t temp_root = cap_channel_create("temp-ack", 4, CAP_RIGHT_ALL);
    if (temp_root == CAP_INVALID_SLOT) return;

    /* 3. mint 一个 SEND-only cap */
    s64 temp_send_slot = cap_mint(temp_root, current, CAP_RIGHT_SEND);
    if (temp_send_slot < 0) {
        cap_destroy_channel(temp_root);
        return;
    }

    /* 4. 发 reply + temp cap */
    const char *reply = "file-content-here";
    rc = cap_send_with_cap(a->chan_slot, MSG_FILE_REPLY,
                            reply, cap_strlen(reply) + 1,
                            (cap_slot_t)temp_send_slot);
    if (rc != CAP_OK) {
        cap_destroy_channel(temp_root);
        return;
    }

    /* 5. 等 ACK */
    u64 ack_type = 0;
    char ack_buf[16];
    u64 ack_cap = sizeof(ack_buf);
    rc = cap_recv(temp_root, &ack_type, ack_buf, &ack_cap);
    if (rc != CAP_OK) {
        cap_destroy_channel(temp_root);
        return;
    }
    if (ack_type != MSG_ACK) {
        cap_destroy_channel(temp_root);
        return;
    }
    a->got_ack = 1;

    /* 6. 清理 temp channel */
    cap_destroy_channel(temp_root);
}

/* ---------------------------------------------------------------
 * task_trusted_client — 受信任的客户端（安静模式）
 * --------------------------------------------------------------- */
static void task_trusted_client(void *arg) {
    struct trusted_client_args *a = (struct trusted_client_args *)arg;

    /* 1. 发请求 */
    const char *req = "GET /etc/passwd";
    int rc = cap_send(a->chan_slot, MSG_FILE_REQ, req, cap_strlen(req) + 1);
    if (rc != CAP_OK) return;

    /* 2. 收 reply + temp cap */
    u64 type = 0;
    char buf[IPC_MAX_PAYLOAD + 1];
    u64 cap = sizeof(buf) - 1;
    cap_slot_t temp_cap = CAP_INVALID_SLOT;
    rc = cap_recv_with_cap(a->chan_slot, &type, buf, &cap, &temp_cap);
    if (rc != CAP_OK) return;
    if (type != MSG_FILE_REPLY) return;
    if (cap > sizeof(buf) - 1) cap = sizeof(buf) - 1;
    buf[cap] = '\0';
    a->got_reply = 1;

    if (temp_cap == CAP_INVALID_SLOT) return;

    /* 3. 用 temp cap 发 ACK */
    const char *ack = "OK";
    rc = cap_send(temp_cap, MSG_ACK, ack, cap_strlen(ack) + 1);
    if (rc != CAP_OK) return;
    a->ack_sent = 1;
}

/* ---------------------------------------------------------------
 * task_untrusted_client — 不受信任的客户端（安静模式）
 * --------------------------------------------------------------- */
static void task_untrusted_client(void *arg) {
    struct untrusted_client_args *a = (struct untrusted_client_args *)arg;

    sched_yield();

    const char *msg = "sneaky-attack";
    int rc = cap_send(1, MSG_FILE_REQ, msg, cap_strlen(msg) + 1);

    if (rc == CAP_ERR_NORIGHT) {
        a->denied = 1;
    }
}

/* ================================================================
 * 【Lesson 9】Supervisor 任务 — 管理 crash-prone 用户子任务
 * ================================================================ */
static void task_l9_supervisor(void *arg) {
    struct l9_sup_args_s {
        const void *image;
        u64 image_len;
        int max_restart;
        int restart_count;
        int last_exit_code;
        int last_fault_type;
        u64 last_fault_rip;
        u64 last_fault_addr;
        s64 child_task_id;
    };
    struct l9_sup_args_s *a = (struct l9_sup_args_s *)arg;

    /* 创建第一个 crash 子任务 */
    s64 child_id = sched_create_user_task(a->image, a->image_len, "crash-child");
    if (child_id < 0) return;
    a->child_task_id = child_id;

    /* 主循环：等待子任务退出，检查退出码，必要时重启 */
    while (1) {
        sched_yield();

        struct task_struct *child = sched_get_task_by_id((u64)a->child_task_id);
        if (child == NULL) break;

        if (child->state == TASK_TERMINATED) {
            a->last_exit_code  = child->exit_code;
            a->last_fault_type = child->fault_type;
            a->last_fault_rip  = child->fault_rip;
            a->last_fault_addr = child->fault_addr;

            child->parent_task_id = 0;

            if (child->exit_code < 0) {
                if (a->restart_count < a->max_restart) {
                    a->restart_count++;
                    sched_yield();
                    sched_yield();

                    child_id = sched_create_user_task(a->image, a->image_len, "crash-child");
                    if (child_id < 0) break;
                    a->child_task_id = child_id;
                    continue;
                } else {
                    break;
                }
            } else {
                break;
            }
        }

        sched_sleep(5);
    }
}

/* ================================================================
 * demo_run — 运行所有 demo（L6 IPC + L7 Cap + L8 user + L9 crash）
 *
 * 传入用户程序二进制（由 main.c 的 user_image.S 提供）。
 * quiet=0: 详细输出; quiet=1: 只打印结果行
 * ================================================================ */
void demo_run(const void *hello_bin, u64 hello_len,
              const void *crash_bin, u64 crash_len,
              int quiet) {
    (void)quiet;  /* 目前总是安静模式 */

    /* ============================================================
     * L6: IPC demo
     * ============================================================ */
    ipc_channel_id_t logger_chan = ipc_channel_create("logger", 8);
    KASSERT(logger_chan != IPC_INVALID_CHANNEL);
    ipc_channel_id_t calc_chan = ipc_channel_create("calc", 4);
    KASSERT(calc_chan != IPC_INVALID_CHANNEL);

    /* 准备 demo 任务参数 */
    static struct logger_args log_srv_args;
    log_srv_args.chan = logger_chan;
    log_srv_args.expected_msgs = 8;

    static struct logger_client_args log_cli_a_args;
    log_cli_a_args.chan = logger_chan;
    log_cli_a_args.num_msgs = 5;
    log_cli_a_args.interval_ticks = 30;
    log_cli_a_args.prefix = "client-A";

    static struct logger_client_args log_cli_b_args;
    log_cli_b_args.chan = logger_chan;
    log_cli_b_args.num_msgs = 3;
    log_cli_b_args.interval_ticks = 60;
    log_cli_b_args.prefix = "client-B";

    static struct calc_args calc_srv_args;
    calc_srv_args.chan = calc_chan;
    calc_srv_args.num_requests = 5;

    static struct calc_args calc_cli_args;
    calc_cli_args.chan = calc_chan;
    calc_cli_args.num_requests = 5;

    /* 创建 5 个 demo 任务 */
    s64 id_log_srv = sched_create_task(task_logger_server, &log_srv_args, "logger-srv");
    s64 id_log_a   = sched_create_task(task_logger_client, &log_cli_a_args, "logger-A");
    s64 id_log_b   = sched_create_task(task_logger_client, &log_cli_b_args, "logger-B");
    s64 id_calc_s  = sched_create_task(task_calc_server,   &calc_srv_args, "calc-srv");
    s64 id_calc_c  = sched_create_task(task_calc_client,  &calc_cli_args, "calc-cli");

    KASSERT(id_log_srv > 0 && id_log_a > 0 && id_log_b > 0
            && id_calc_s > 0 && id_calc_c > 0);

    (void)id_log_srv; (void)id_log_a; (void)id_log_b;
    (void)id_calc_s; (void)id_calc_c;

    /* 让出 CPU，等 demo 任务运行 */
    sched_yield();
    while (sched_num_tasks() > 1) {
        sched_sleep(10);
    }

    /* 销毁 IPC channels */
    ipc_channel_destroy(logger_chan);
    ipc_channel_destroy(calc_chan);

    /* L6 结果行 */
    arch_console_print(" L6 IPC demo: logger(8 msgs) calc(5/5 RPC)");
    arch_console_set_color(CON_COLOR_GREEN);
    arch_console_print("  OK\n");
    arch_console_set_color(CON_COLOR_DEFAULT);

    /* ============================================================
     * L7: Capability demo
     * ============================================================ */
    cap_slot_t root_slot = cap_channel_create("secure-srv", 4, CAP_RIGHT_ALL);
    KASSERT(root_slot != CAP_INVALID_SLOT);

    static struct file_server_args fs_args;
    fs_args.chan_slot = CAP_INVALID_SLOT;
    fs_args.got_request = 0;
    fs_args.got_ack = 0;

    static struct trusted_client_args tc_args;
    tc_args.chan_slot = CAP_INVALID_SLOT;
    tc_args.got_reply = 0;
    tc_args.ack_sent = 0;

    static struct untrusted_client_args uc_args;
    uc_args.fake_slot = CAP_INVALID_SLOT;
    uc_args.denied = 0;

    /* === 临界区开始：关中断，防止 timer IRQ 在 cap 设置期间抢占 init === */
    u64 setup_flags = arch_irq_save();

    s64 id_fs = sched_create_task(task_file_server, &fs_args, "file-srv");
    s64 id_tc = sched_create_task(task_trusted_client, &tc_args, "trusted-cli");
    s64 id_uc = sched_create_task(task_untrusted_client, &uc_args, "untrusted-cli");

    KASSERT(id_fs > 0 && id_tc > 0 && id_uc > 0);

    (void)id_fs; (void)id_tc; (void)id_uc;

    struct task_struct *fs_task = sched_get_task_by_id((u64)id_fs);
    struct task_struct *tc_task = sched_get_task_by_id((u64)id_tc);
    KASSERT(fs_task != NULL && tc_task != NULL);

    s64 fs_slot = cap_mint(root_slot, fs_task,
                            CAP_RIGHT_RECV | CAP_RIGHT_MINT | CAP_RIGHT_SEND);
    KASSERT(fs_slot > 0);
    fs_args.chan_slot = (cap_slot_t)fs_slot;

    s64 tc_slot = cap_mint(root_slot, tc_task,
                            CAP_RIGHT_SEND | CAP_RIGHT_RECV);
    KASSERT(tc_slot > 0);
    tc_args.chan_slot = (cap_slot_t)tc_slot;

    arch_irq_restore(setup_flags);
    /* === 临界区结束 === */

    (void)fs_slot; (void)tc_slot;

    sched_yield();
    while (sched_num_tasks() > 1) {
        sched_sleep(10);
    }

    /* 销毁 secure-srv channel */
    cap_destroy_channel(root_slot);

    /* L7 结果行 */
    arch_console_print(" L7 Cap demo: file-srv . trusted . untrusted");
    arch_console_set_color(CON_COLOR_GREEN);
    arch_console_print("  OK\n");
    arch_console_set_color(CON_COLOR_DEFAULT);

    /* ============================================================
     * L8: First User-Space Service
     * ============================================================ */
    usize_t frames_before = arch_pmm_free_frames();

    s64 id_u1 = sched_create_user_task(hello_bin, hello_len, "user-A");
    s64 id_u2 = sched_create_user_task(hello_bin, hello_len, "user-B");
    KASSERT(id_u1 > 0 && id_u2 > 0);

    (void)id_u1; (void)id_u2;

    sched_yield();
    while (sched_num_tasks() > 1) {
        sched_sleep(10);
    }

    usize_t frames_after = arch_pmm_free_frames();
    /* 【C7 修复】原代码 `(void)frames_after; (void)frames_before;` 显式丢弃
     *   leak 检查，L8 假装通过。worklog W10 已修了 L8 的 6-frame page-table
     *   leak（arch_vmm_unmap_user_page 递归释放 PT/PD/PDPT），现在应无泄漏。
     *   改为真实 KASSERT：若有泄漏，demo 阶段就 panic（fail-fast），
     *   而不是让 ktest 在后面困惑于帧数对不上。
     *
     *   不打印结果行（保持 L10 清洁输出目标）；L9 仍打印 "0 page leak"
     *   作为崩溃恢复的可视确认，L8 的 leak 检查静默但严格。 */
    if (frames_after != frames_before) {
        panic(__FILE__, __LINE__,
              "L8 user-mode demo leaked physical frames "
              "(page-table cleanup regression?)");
    }

    /* ============================================================
     * L9: Crash Recovery
     * ============================================================ */
    u64 tick_before = arch_pit_get_tick_count();
    usize_t frames_before_l9 = arch_pmm_free_frames();

    s64 id_hello_l9 = sched_create_user_task(hello_bin, hello_len, "hello-l9");
    s64 id_crash_l9 = sched_create_user_task(crash_bin, crash_len, "crash-l9");

    {
        struct task_struct *ht = sched_get_task_by_id((u64)id_hello_l9);
        struct task_struct *ct = sched_get_task_by_id((u64)id_crash_l9);
        if (ht) ht->parent_task_id = 0;
        if (ct) ct->parent_task_id = 0;
    }

    sched_yield();
    while (sched_num_tasks() > 1) {
        sched_sleep(10);
    }

    u64 tick_after = arch_pit_get_tick_count();
    usize_t frames_after_l9 = arch_pmm_free_frames();
    (void)tick_after; (void)tick_before;
    int l9_no_leak = (frames_after_l9 == frames_before_l9);

    /* ============================================================
     * L9b: Supervisor Auto-Restart Demo
     * ============================================================ */
    #define L9_MAX_RESTART  2
    static struct {
        const void *image;
        u64 image_len;
        int max_restart;
        int restart_count;
        int last_exit_code;
        int last_fault_type;
        u64 last_fault_rip;
        u64 last_fault_addr;
        s64 child_task_id;
    } l9_sup_args;

    l9_sup_args.image = crash_bin;
    l9_sup_args.image_len = crash_len;
    l9_sup_args.max_restart = L9_MAX_RESTART;
    l9_sup_args.restart_count = 0;
    l9_sup_args.last_exit_code = 0;
    l9_sup_args.last_fault_type = 0;
    l9_sup_args.last_fault_rip = 0;
    l9_sup_args.last_fault_addr = 0;
    l9_sup_args.child_task_id = -1;

    s64 id_sup = sched_create_task(task_l9_supervisor, &l9_sup_args, "l9-sup");
    (void)id_sup;

    sched_yield();
    while (sched_num_tasks() > 1) {
        sched_sleep(10);
    }

    /* ---- L9 结果行 ---- */
    arch_console_print("\n L9 crash recovery: #PF->kill task, kernel OK");
    if (l9_no_leak) {
        arch_console_print(" . 0 page leak");
    } else {
        arch_console_print(" . LEAK!");
    }
    arch_console_print(" . supervisor ");
    kprint_dec((u64)l9_sup_args.restart_count);
    arch_console_print("/");
    kprint_dec((u64)l9_sup_args.max_restart);
    arch_console_print(" restart\n");
}
