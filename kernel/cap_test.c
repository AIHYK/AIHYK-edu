/* ================================================================
 * kernel/cap_test.c — L7 Capability 框架的边界测试 + 压力测试
 *
 * 【Lesson 7 测试套件】
 *
 * 实现 include/kernel/cap_test.h 的 cap_test_run_all()。
 *
 * 测试分 7 个 section：
 *   A. 边界 — 非法参数（NULL / 越界 slot / 未使用 slot）
 *   B. 边界 — 权限强制（无 SEND/RECV/MINT/DESTRUCT → NORIGHT）
 *   C. 边界 — CSpace 耗尽（31 槽全满 + 槽位复用）
 *   D. 生命周期 — 双操作 / 删除后再用 / 销毁后悬空 cap
 *   E. 跨任务 — revoke / destroy_channel 在其他任务的 cspace 生效
 *   F. cap 传递 — send_with_cap / recv_with_cap 边界 + 正常路径
 *   G. 压力 — 反复 create/destroy、mint/revoke、leak 检测
 *
 * 【测试哲学】
 *   - 边界测试：每个 API 的每个错误返回路径都要被覆盖到
 *   - 单任务优先：能在 init task 里测的就不开 helper（少一个变维度）
 *   - 多任务最小化：helper 只做"让出 + 退出"，init 直接读 helper 的
 *     cspace 字段验证状态（cap_revoke / cap_destroy_channel 会遍历
 *     所有任务的 cspace，所以 helper 不需要真的跑）
 *   - 不崩溃：测试用 TEST_CHECK_INT 比较，不用 KASSERT
 *   - 自清理：每个 case 创建的 channel / cap 最后都 destroy/delete
 *   - Leak 检测：压力测试前后对比 cap_total_caps() 看是否回到基线
 * ================================================================ */

#include <arch/console.h>
#include <arch/cpu.h>
#include <arch/task.h>
#include <kernel/cap.h>
#include <kernel/cap_test.h>
#include <kernel/ipc.h>
#include <kernel/mm.h>
#include <kernel/panic.h>
#include <kernel/sched.h>
#include <kernel/types.h>
#include <kernel/util.h>

/* ================================================================
 * 测试框架（局部工具）
 * ================================================================ */

static int g_pass = 0;   /* 全局 PASS 计数 */
static int g_fail = 0;   /* 全局 FAIL 计数 */
static int g_quiet = 0;  /* 安静模式：只打印最终汇总 */

/* 【C8 修复】原 ct_print_dec / ct_print_dec_s 已删除，
 *   统一用 <kernel/util.h> 的 kprint_dec / kprint_dec_s。
 *   顺便修了原 ct_print_dec_s 的 INT64_MIN UB（原 (u64)(-v) 溢出，
 *   kprint_dec_s 用 -(u64)v 良定义）。 */

/* 局部 strlen */
static u64 ct_strlen(const char *s) {
    u64 n = 0;
    while (s[n] != '\0') n++;
    return n;
}

/* 测试用消息类型 */
#define MSG_TX_TEST  20

/* ----------------------------------------------------------------
 * TEST_PASS / TEST_FAIL — 打印测试结果
 *
 *   每个 case 用 TEST_CHECK_INT(name, expected, got) 比较，
 *   相等 → [PASS]，不等 → [FAIL] + 打印 expected vs got。
 * ---------------------------------------------------------------- */

static void test_print_pass(const char *name) {
    if (g_quiet) { g_pass++; return; }
    arch_console_set_color(CON_COLOR_GREEN);
    arch_console_print("    [PASS] ");
    arch_console_set_color(CON_COLOR_DEFAULT);
    arch_console_print(name);
    arch_console_print("\n");
    g_pass++;
}

static void test_print_fail(const char *name, s64 expected, s64 got) {
    /* FAIL always prints (even in quiet mode) so failures are visible */
    arch_console_set_color(CON_COLOR_RED);
    arch_console_print("    [FAIL] ");
    arch_console_set_color(CON_COLOR_DEFAULT);
    arch_console_print(name);
    arch_console_print("  expected=");
    kprint_dec_s(expected);
    arch_console_print(" got=");
    kprint_dec_s(got);
    arch_console_print("\n");
    g_fail++;
}

/* 比较期望值和实际值（有符号，因为错误码是负数） */
#define TEST_CHECK_INT(name, expected, got) \
    do { \
        s64 _e = (s64)(expected); \
        s64 _g = (s64)(got); \
        if (_e == _g) { \
            test_print_pass(name); \
        } else { \
            test_print_fail(name, _e, _g); \
        } \
    } while (0)

/* 检查布尔条件 */
#define TEST_CHECK_BOOL(name, cond) \
    do { \
        if (cond) { \
            test_print_pass(name); \
        } else { \
            test_print_fail(name, 1, 0); \
        } \
    } while (0)

/* section 头 */
static void section_header(const char *title) {
    if (g_quiet) return;
    arch_console_set_color(CON_COLOR_CYAN);
    arch_console_print("\n----------------------------------------\n");
    arch_console_print(title);
    arch_console_print("\n----------------------------------------\n");
    arch_console_set_color(CON_COLOR_DEFAULT);
}

/* section 汇总 */
static void section_summary(const char *title, int pass, int fail) {
    if (g_quiet) return;
    arch_console_print("  ");
    arch_console_print(title);
    arch_console_print(": ");
    kprint_dec((u64)pass);
    arch_console_print(" PASS, ");
    kprint_dec((u64)fail);
    arch_console_print(" FAIL\n");
}

/* ================================================================
 * Section A: 边界 — 非法参数
 *
 *   测每个 API 收到 NULL / 越界 slot / 未使用 slot 时是否
 *   返回正确的错误码而不崩溃。
 * ================================================================ */

/* 保存/恢复全局计数器（每个 section 独立计数） */
#define SAVE_COUNTS() \
    int _saved_pass = g_pass, _saved_fail = g_fail
#define RESTORE_SECTION(title) do { \
    section_summary(title, g_pass - _saved_pass, g_fail - _saved_fail); \
} while (0)

/* A1: cap_cspace_init(NULL) → CAP_ERR_INVAL */
static void test_a01(void) {
    int rc = cap_cspace_init(NULL);
    TEST_CHECK_INT("A01 cap_cspace_init(NULL) returns INVAL",
                   CAP_ERR_INVAL, rc);
}

/* A2: cap_cspace_destroy(NULL) → 不崩溃（无返回值） */
static void test_a02(void) {
    cap_cspace_destroy(NULL);   /* 不崩溃即 PASS */
    if (!g_quiet) arch_console_print("    [PASS] A02 cap_cspace_destroy(NULL) no crash\n");
    g_pass++;
}

/* A3: cap_lookup_check 在非法 slot 上返回 NULL
 *   - slot = 0 (CAP_INVALID_SLOT)
 *   - slot = CAP_SLOTS_PER_TASK (越界)
 *   - slot = CAP_SLOTS_PER_TASK + 10 (远越界)
 *   - slot = 1 在空 cspace 里（未使用）
 *   全部应返回 NULL。 */
static void test_a03(void) {
    /* current 是 init，cspace 应已存在（cap_init_subsystem 初始化） */
    struct cap *c;
    int ok = 1;

    c = cap_lookup_check(0, CAP_RIGHT_SEND);
    if (c != NULL) ok = 0;

    c = cap_lookup_check(CAP_SLOTS_PER_TASK, CAP_RIGHT_SEND);
    if (c != NULL) ok = 0;

    c = cap_lookup_check(CAP_SLOTS_PER_TASK + 10, CAP_RIGHT_SEND);
    if (c != NULL) ok = 0;

    c = cap_lookup_check(1, CAP_RIGHT_SEND);   /* slot 1 未使用 */
    if (c != NULL) ok = 0;

    TEST_CHECK_BOOL("A03 cap_lookup_check invalid slots return NULL", ok);
}

/* A4: cap_send 在空 cspace 上 → CAP_ERR_NORIGHT (slot 未使用) */
static void test_a04(void) {
    int rc = cap_send(1, MSG_TX_TEST, "x", 2);
    TEST_CHECK_INT("A04 cap_send on empty cspace slot", CAP_ERR_NORIGHT, rc);
}

/* A5: cap_recv 在空 cspace 上 → CAP_ERR_NORIGHT */
static void test_a05(void) {
    u64 type = 0; char buf[8]; u64 cap = sizeof(buf);
    int rc = cap_recv(1, &type, buf, &cap);
    TEST_CHECK_INT("A05 cap_recv on empty cspace slot", CAP_ERR_NORIGHT, rc);
}

/* A6: cap_delete 在非法 slot → CAP_ERR_INVAL */
static void test_a06(void) {
    int rc1 = cap_delete(0);                    /* CAP_INVALID_SLOT */
    int rc2 = cap_delete(CAP_SLOTS_PER_TASK);   /* 越界 */
    int rc3 = cap_delete(CAP_SLOTS_PER_TASK + 5);
    TEST_CHECK_INT("A06a cap_delete(0) returns INVAL", CAP_ERR_INVAL, rc1);
    TEST_CHECK_INT("A06b cap_delete(SLOTS) returns INVAL", CAP_ERR_INVAL, rc2);
    TEST_CHECK_INT("A06c cap_delete(SLOTS+5) returns INVAL", CAP_ERR_INVAL, rc3);
}

/* A7: cap_delete 在未使用 slot → CAP_ERR_NOTFOUND */
static void test_a07(void) {
    int rc = cap_delete(1);   /* slot 1 未使用 */
    TEST_CHECK_INT("A07 cap_delete unused slot returns NOTFOUND",
                   CAP_ERR_NOTFOUND, rc);
}

/* A8: cap_revoke 在非法 slot → CAP_ERR_INVAL */
static void test_a08(void) {
    int rc1 = cap_revoke(0);
    int rc2 = cap_revoke(CAP_SLOTS_PER_TASK);
    TEST_CHECK_INT("A08a cap_revoke(0) returns INVAL", CAP_ERR_INVAL, rc1);
    TEST_CHECK_INT("A08b cap_revoke(SLOTS) returns INVAL", CAP_ERR_INVAL, rc2);
}

/* A9: cap_revoke 在未使用 slot → CAP_ERR_NOTFOUND */
static void test_a09(void) {
    int rc = cap_revoke(1);
    TEST_CHECK_INT("A09 cap_revoke unused slot returns NOTFOUND",
                   CAP_ERR_NOTFOUND, rc);
}

/* A10: cap_mint 到 NULL task → CAP_ERR_INVAL */
static void test_a10(void) {
    /* 需要一个 src cap。创建临时 channel + root cap。 */
    cap_slot_t root = cap_channel_create("test-a10", 4, CAP_RIGHT_ALL);
    if (root == CAP_INVALID_SLOT) {
        arch_console_print("    [FAIL] A10 setup failed (cap_channel_create)\n");
        g_fail++;
        return;
    }
    s64 rc = cap_mint(root, NULL, CAP_RIGHT_SEND);
    TEST_CHECK_INT("A10 cap_mint to NULL task returns INVAL",
                   CAP_ERR_INVAL, rc);
    cap_destroy_channel(root);
}

/* A11: cap_send_with_cap 非法 transfer slot → CAP_ERR_INVAL
 *   - transfer = 0 (CAP_INVALID_SLOT)
 *   - transfer = CAP_SLOTS_PER_TASK (越界) */
static void test_a11(void) {
    cap_slot_t root = cap_channel_create("test-a11", 4, CAP_RIGHT_ALL);
    if (root == CAP_INVALID_SLOT) {
        arch_console_print("    [FAIL] A11 setup failed\n");
        g_fail++;
        return;
    }
    /* root 有 SEND，所以 cap_send_with_cap 会过 chan 检查，
     * 但 transfer 检查会失败（在调 ipc_send 之前）→ 不阻塞 */
    int rc1 = cap_send_with_cap(root, MSG_TX_TEST, "x", 2, 0);
    int rc2 = cap_send_with_cap(root, MSG_TX_TEST, "x", 2,
                                 CAP_SLOTS_PER_TASK);
    TEST_CHECK_INT("A11a send_with_cap transfer=0 returns INVAL",
                   CAP_ERR_INVAL, rc1);
    TEST_CHECK_INT("A11b send_with_cap transfer=SLOTS returns INVAL",
                   CAP_ERR_INVAL, rc2);
    cap_destroy_channel(root);
}

/* A12: cap_send_with_cap transfer 指向未使用 slot → CAP_ERR_NOTFOUND */
static void test_a12(void) {
    cap_slot_t root = cap_channel_create("test-a12", 4, CAP_RIGHT_ALL);
    if (root == CAP_INVALID_SLOT) {
        arch_console_print("    [FAIL] A12 setup failed\n");
        g_fail++;
        return;
    }
    /* slot 5 在 init cspace 里未使用 */
    int rc = cap_send_with_cap(root, MSG_TX_TEST, "x", 2, 5);
    TEST_CHECK_INT("A12 send_with_cap unused transfer slot returns NOTFOUND",
                   CAP_ERR_NOTFOUND, rc);
    cap_destroy_channel(root);
}

/* A13: cap_recv_with_cap out_cap_slot==NULL → CAP_ERR_INVAL */
static void test_a13(void) {
    cap_slot_t root = cap_channel_create("test-a13", 4, CAP_RIGHT_ALL);
    if (root == CAP_INVALID_SLOT) {
        arch_console_print("    [FAIL] A13 setup failed\n");
        g_fail++;
        return;
    }
    u64 type = 0; char buf[8]; u64 cap = sizeof(buf);
    int rc = cap_recv_with_cap(root, &type, buf, &cap, NULL);
    TEST_CHECK_INT("A13 cap_recv_with_cap NULL out_cap_slot returns INVAL",
                   CAP_ERR_INVAL, rc);
    cap_destroy_channel(root);
}

/* A14: cap_recv_with_cap 在没有 RECV 权限的 cap 上 → CAP_ERR_NORIGHT
 *   （早返回，不阻塞） */
static void test_a14(void) {
    cap_slot_t root = cap_channel_create("test-a14", 4, CAP_RIGHT_ALL);
    if (root == CAP_INVALID_SLOT) {
        arch_console_print("    [FAIL] A14 setup failed\n");
        g_fail++;
        return;
    }
    /* mint 一个 SEND-only cap（无 RECV） */
    s64 send_only = cap_mint(root, current, CAP_RIGHT_SEND);
    if (send_only < 0) {
        arch_console_print("    [FAIL] A14 mint failed\n");
        g_fail++;
        cap_destroy_channel(root);
        return;
    }
    u64 type = 0; char buf[8]; u64 cap = sizeof(buf);
    cap_slot_t out = CAP_INVALID_SLOT;
    int rc = cap_recv_with_cap((cap_slot_t)send_only, &type, buf, &cap, &out);
    TEST_CHECK_INT("A14 recv_with_cap on SEND-only cap returns NORIGHT",
                   CAP_ERR_NORIGHT, rc);
    cap_destroy_channel(root);
}

/* A15: cap_stats() 在空 cspace 上不崩溃 */
static void test_a15(void) {
    /* 此时 init cspace 应是空的（前面的 case 都自清理了） */
    /* cap_stats still runs for no-crash verification, but output suppressed in quiet mode */
    if (!g_quiet) cap_stats();
    if (!g_quiet) arch_console_print("    [PASS] A15 cap_stats() on empty cspace no crash\n");
    g_pass++;
}

/* A16: cap_total_caps() 返回合理值（>= 0） */
static void test_a16(void) {
    int total = cap_total_caps();
    TEST_CHECK_BOOL("A16 cap_total_caps() returns non-negative",
                    total >= 0);
}

static void run_section_a(void) {
    SAVE_COUNTS();
    section_header("Section A: Boundary — Invalid Parameters");
    test_a01(); test_a02(); test_a03(); test_a04();
    test_a05(); test_a06(); test_a07(); test_a08();
    test_a09(); test_a10(); test_a11(); test_a12();
    test_a13(); test_a14(); test_a15(); test_a16();
    RESTORE_SECTION("Section A");
}

/* ================================================================
 * Section B: 边界 — 权限强制
 *
 *   测"无权限 → CAP_ERR_NORIGHT"的每条路径。
 *   用一个 ALL-rights 的 root cap + 几个 limited-rights 的 mint cap。
 * ================================================================ */

static void run_section_b(void) {
    SAVE_COUNTS();
    section_header("Section B: Boundary — Permission Enforcement");

    /* 创建一个 root cap（ALL rights） */
    cap_slot_t root = cap_channel_create("test-b", 4, CAP_RIGHT_ALL);
    if (root == CAP_INVALID_SLOT) {
        arch_console_print("    [FAIL] Section B setup failed\n");
        g_fail++;
        goto b_done;
    }

    /* B1: mint 一个 SEND-only cap，然后测各操作 */
    s64 send_slot = cap_mint(root, current, CAP_RIGHT_SEND);
    TEST_CHECK_BOOL("B01 mint SEND-only cap succeeds", send_slot > 0);

    if (send_slot > 0) {
        /* B2: cap_recv on SEND-only cap → NORIGHT（无 RECV，早返回） */
        u64 t = 0; char b[8]; u64 c = sizeof(b);
        int rc = cap_recv((cap_slot_t)send_slot, &t, b, &c);
        TEST_CHECK_INT("B02 cap_recv on SEND-only returns NORIGHT",
                       CAP_ERR_NORIGHT, rc);

        /* B3: cap_mint from SEND-only cap → NORIGHT（无 MINT） */
        s64 rc2 = cap_mint((cap_slot_t)send_slot, current, CAP_RIGHT_SEND);
        TEST_CHECK_INT("B03 cap_mint from SEND-only returns NORIGHT",
                       CAP_ERR_NORIGHT, rc2);

        /* B4: cap_destroy_channel on SEND-only → NORIGHT（无 DESTRUCT） */
        int rc3 = cap_destroy_channel((cap_slot_t)send_slot);
        TEST_CHECK_INT("B04 cap_destroy_channel on SEND-only returns NORIGHT",
                       CAP_ERR_NORIGHT, rc3);
    }

    /* B5: mint 一个 RECV-only cap，测 cap_send → NORIGHT */
    s64 recv_slot = cap_mint(root, current, CAP_RIGHT_RECV);
    TEST_CHECK_BOOL("B05 mint RECV-only cap succeeds", recv_slot > 0);

    if (recv_slot > 0) {
        /* B6: cap_send on RECV-only → NORIGHT（无 SEND，早返回） */
        int rc = cap_send((cap_slot_t)recv_slot, MSG_TX_TEST, "x", 2);
        TEST_CHECK_INT("B06 cap_send on RECV-only returns NORIGHT",
                       CAP_ERR_NORIGHT, rc);

        /* B7: cap_mint from RECV-only → NORIGHT（无 MINT） */
        s64 rc2 = cap_mint((cap_slot_t)recv_slot, current, CAP_RIGHT_RECV);
        TEST_CHECK_INT("B07 cap_mint from RECV-only returns NORIGHT",
                       CAP_ERR_NORIGHT, rc2);

        /* B8: cap_destroy_channel on RECV-only → NORIGHT */
        int rc3 = cap_destroy_channel((cap_slot_t)recv_slot);
        TEST_CHECK_INT("B08 cap_destroy_channel on RECV-only returns NORIGHT",
                       CAP_ERR_NORIGHT, rc3);
    }

    /* B9: 单调下降 — cap_mint 用 new_rights 含 src 不具备的位 → NORIGHT
     *   src = root (rights = 0x0F = ALL)
     *   new_rights = 0x10 (一个 src 没有的位)
     *   (0x10 & ~0x0F) = 0x10 ≠ 0 → 拒绝 */
    s64 rc_mono = cap_mint(root, current, 0x10);
    TEST_CHECK_INT("B09 cap_mint with non-subset rights returns NORIGHT",
                   CAP_ERR_NORIGHT, rc_mono);

    /* B10: 单调下降 — src=SEND(0x01), new_rights=SEND|RECV(0x03)
     *   (0x03 & ~0x01) = 0x02 ≠ 0 → 拒绝 */
    if (send_slot > 0) {
        s64 rc_mono2 = cap_mint((cap_slot_t)send_slot, current,
                                CAP_RIGHT_SEND | CAP_RIGHT_RECV);
        TEST_CHECK_INT("B10 cap_mint SEND→SEND|RECV returns NORIGHT",
                       CAP_ERR_NORIGHT, rc_mono2);
    }

    /* B11: 单调下降 OK 路径 — src=ALL, new_rights=SEND → 成功
     *   (SEND & ~ALL) = 0 → 允许 */
    s64 rc_ok = cap_mint(root, current, CAP_RIGHT_SEND);
    TEST_CHECK_BOOL("B11 cap_mint ALL→SEND succeeds (subset OK)",
                    rc_ok > 0);

    /* 清理：destroy channel 会清掉 root + 所有 mint 出来的 cap */
    cap_destroy_channel(root);

b_done:
    RESTORE_SECTION("Section B");
}

/* ================================================================
 * Section C: 边界 — CSpace 耗尽 + 槽位复用
 *
 *   CAP_SLOTS_PER_TASK=32，slot 0 保留，可用 1..31 = 31 个。
 *   创建 31 个 channel + cap，第 32 个失败。
 *   然后全部删除，再创建验证 slot 被复用。
 * ================================================================ */

static void run_section_c(void) {
    SAVE_COUNTS();
    section_header("Section C: Boundary — CSpace Exhaustion + Slot Reuse");

    /* C1: 填满 cspace（31 个 channel cap） */
    cap_slot_t slots[CAP_SLOTS_PER_TASK];
    int created = 0;
    for (int i = 1; i < CAP_SLOTS_PER_TASK; i++) {
        char name[] = "cx00";
        name[2] = (char)('0' + (i / 10));
        name[3] = (char)('0' + (i % 10));
        slots[i] = cap_channel_create(name, 2, CAP_RIGHT_ALL);
        if (slots[i] != CAP_INVALID_SLOT) {
            created++;
        } else {
            break;
        }
    }
    TEST_CHECK_INT("C01 fill 31 slots (created count)",
                   CAP_SLOTS_PER_TASK - 1, created);

    /* C2: cspace 满后再 cap_channel_create → CAP_INVALID_SLOT */
    cap_slot_t overflow = cap_channel_create("overflow", 2, CAP_RIGHT_ALL);
    TEST_CHECK_INT("C02 cap_channel_create when cspace full returns INVALID",
                   (s64)CAP_INVALID_SLOT, (s64)overflow);

    /* C3: cap_total_caps 验证：至少 created 个 cap（其他 task 可能有） */
    int total = cap_total_caps();
    TEST_CHECK_BOOL("C03 cap_total_caps >= created after fill",
                    total >= created);

    /* C4: 删除所有 cap（通过 destroy channel，会清所有指向的 cap） */
    int destroyed = 0;
    for (int i = 1; i < CAP_SLOTS_PER_TASK; i++) {
        if (slots[i] != CAP_INVALID_SLOT) {
            int rc = cap_destroy_channel(slots[i]);
            if (rc == 0) destroyed++;
        }
    }
    TEST_CHECK_INT("C04 destroy all channels", created, destroyed);

    /* C5: cspace 应该回到空（cap_total_caps 0 或接近 0） */
    int total_after = cap_total_caps();
    TEST_CHECK_INT("C05 cap_total_caps after cleanup (expect 0)",
                   0, total_after);

    /* C6: 槽位复用 — 再创建一个 cap，应该拿到 slot 1（最低空 slot） */
    cap_slot_t reuse = cap_channel_create("reuse-test", 2, CAP_RIGHT_ALL);
    TEST_CHECK_BOOL("C06 slot reuse after cleanup succeeds",
                    reuse != CAP_INVALID_SLOT);
    if (reuse != CAP_INVALID_SLOT) {
        /* 验证 slot 编号在 1..31 范围 */
        TEST_CHECK_BOOL("C06a reused slot in valid range",
                        reuse >= 1 && reuse < CAP_SLOTS_PER_TASK);
        cap_destroy_channel(reuse);
    }

    RESTORE_SECTION("Section C");
}

/* ================================================================
 * Section D: 生命周期 — 双操作 / 删除后再用 / 销毁后悬空 cap
 * ================================================================ */

static void run_section_d(void) {
    SAVE_COUNTS();
    section_header("Section D: Lifecycle — Double Ops + Dangling Caps");

    /* D1: cap_delete 同一个 slot 两次 → 第一次 OK，第二次 NOTFOUND */
    cap_slot_t root = cap_channel_create("test-d1", 4, CAP_RIGHT_ALL);
    TEST_CHECK_BOOL("D01a setup cap_channel_create", root != CAP_INVALID_SLOT);
    if (root == CAP_INVALID_SLOT) goto d_done;

    /* 先 mint 一个 cap（用于 delete 测试），channel 保留不销毁 */
    s64 del_slot = cap_mint(root, current, CAP_RIGHT_SEND);
    TEST_CHECK_BOOL("D01b mint cap for delete test", del_slot > 0);

    int rc1 = cap_delete((cap_slot_t)del_slot);
    TEST_CHECK_INT("D01c cap_delete first time returns OK",
                   CAP_OK, rc1);

    int rc2 = cap_delete((cap_slot_t)del_slot);
    TEST_CHECK_INT("D01d cap_delete second time returns NOTFOUND",
                   CAP_ERR_NOTFOUND, rc2);

    /* D2: cap_revoke on 刚 delete 的 slot → NOTFOUND
     *   （因为 cap_delete 清了 in_use，cap_revoke 检查 in_use 时失败） */
    int rc3 = cap_revoke((cap_slot_t)del_slot);
    TEST_CHECK_INT("D02 cap_revoke on deleted slot returns NOTFOUND",
                   CAP_ERR_NOTFOUND, rc3);

    /* D3: mint → delete → mint again（slot 复用）
     *   del_slot 被删除后，再 mint 应该能拿到同一个 slot（或更小的） */
    s64 re_mint = cap_mint(root, current, CAP_RIGHT_SEND);
    TEST_CHECK_BOOL("D03 re-mint after delete succeeds", re_mint > 0);
    if (re_mint > 0) {
        /* 不一定是同一个 slot，但应该在有效范围 */
        TEST_CHECK_BOOL("D03a re-mint slot in valid range",
                        re_mint >= 1 && re_mint < CAP_SLOTS_PER_TASK);
    }

    /* D4: cap_destroy_channel 后，用旧 slot 做 cap_send → NORIGHT
     *   （destroy 清掉了所有指向该 channel 的 cap） */
    /* 先创建第二个 channel 做 destroy 测试 */
    cap_slot_t root2 = cap_channel_create("test-d4", 4, CAP_RIGHT_ALL);
    TEST_CHECK_BOOL("D04a setup second channel", root2 != CAP_INVALID_SLOT);
    if (root2 != CAP_INVALID_SLOT) {
        /* 记住 slot 编号，destroy 后用这个 slot 试 send */
        cap_slot_t dangling_slot = root2;
        int rc_destroy = cap_destroy_channel(root2);
        TEST_CHECK_INT("D04b cap_destroy_channel returns OK",
                       CAP_OK, rc_destroy);
        /* 现在 dangling_slot 在 init 的 cspace 里应该被清掉了 */
        int rc_send = cap_send(dangling_slot, MSG_TX_TEST, "x", 2);
        TEST_CHECK_INT("D04c cap_send on destroyed cap returns NORIGHT",
                       CAP_ERR_NORIGHT, rc_send);
    }

    /* 清理 D1/D3 创建的 channel（root） */
    cap_destroy_channel(root);

d_done:
    RESTORE_SECTION("Section D");
}

/* ================================================================
 * Section E: 跨任务 — revoke / destroy_channel 在其他任务的 cspace 生效
 *
 *   【关键模式】
 *     1. init 创建 helper 任务（cspace 已初始化，但 helper 还没跑）
 *     2. init cap_mint(root, helper_task, ...) 把 cap 装到 helper 的 cspace
 *     3. init 直接读 helper_task->cspace->slots[s].in_use 验证 cap 在
 *     4. init cap_revoke / cap_destroy_channel（会遍历所有 task 包括 helper）
 *     5. init 再读 helper_task->cspace->slots[s].in_use 验证 cap 被清
 *     6. init sched_yield → helper 跑（cspace 已空）→ 立即 exit → 被 reap
 *
 *   helper 任务什么都不做：sched_yield + return。
 * ================================================================ */

/* helper 任务：让出 CPU 后立即退出（让 init 检查 cspace） */
static void helper_yield_exit(void *arg) {
    (void)arg;
    sched_yield();
    /* task_trampoline 会自动 sched_exit */
}

/* E1: cap_mint 到 helper，验证 helper 的 cspace 有 cap
 *   然后 cap_revoke，验证 helper 的 cap 被清、init 的 src 保留 */
static void test_e01(void) {
    cap_slot_t root = cap_channel_create("test-e1", 4, CAP_RIGHT_ALL);
    if (root == CAP_INVALID_SLOT) {
        arch_console_print("    [FAIL] E01 setup failed\n");
        g_fail++;
        return;
    }

    /* 创建 helper（不 yield，helper 还没跑） */
    s64 hid = sched_create_task(helper_yield_exit, NULL, "helper-e1");
    if (hid <= 0) {
        arch_console_print("    [FAIL] E01 sched_create_task failed\n");
        g_fail++;
        cap_destroy_channel(root);
        return;
    }
    struct task_struct *helper = sched_get_task_by_id((u64)hid);
    TEST_CHECK_BOOL("E01a helper task created", helper != NULL);

    if (helper == NULL) {
        cap_destroy_channel(root);
        return;
    }

    /* mint 一个 cap 到 helper */
    s64 h_slot = cap_mint(root, helper, CAP_RIGHT_SEND | CAP_RIGHT_RECV);
    TEST_CHECK_BOOL("E01b cap_mint to helper succeeds", h_slot > 0);

    /* 验证 helper 的 cspace 确实有 cap */
    int helper_has_cap = 0;
    if (h_slot > 0) {
        helper_has_cap = helper->cspace->slots[h_slot].in_use ? 1 : 0;
    }
    TEST_CHECK_BOOL("E01c helper cspace has the minted cap", helper_has_cap);

    /* 现在 init 调 cap_revoke(root) → 应该清掉 helper 那个 cap，
     * 但 init 自己的 root 保留 */
    int revoked = cap_revoke(root);
    TEST_CHECK_BOOL("E01d cap_revoke returns count >= 1", revoked >= 1);

    /* 验证 helper 的 cap 被清 */
    int helper_cleared = 0;
    if (h_slot > 0) {
        helper_cleared = helper->cspace->slots[h_slot].in_use ? 0 : 1;
    }
    TEST_CHECK_BOOL("E01e helper cap cleared by revoke", helper_cleared);

    /* 验证 init 的 root 仍在 */
    int init_kept = current->cspace->slots[root].in_use ? 1 : 0;
    TEST_CHECK_BOOL("E01f init root cap kept after revoke", init_kept);

    /* 清理 + 让 helper 跑完退出 */
    cap_destroy_channel(root);
    sched_yield();   /* 让 helper 跑，它会 yield+exit */
    /* 等 helper 被 reap（最多等 20 tick = 200ms） */
    for (int i = 0; i < 20 && sched_num_tasks() > 1; i++) {
        sched_sleep(1);
    }
}

/* E2: mint 到 2 个 helper，revoke 一次性清两个 */
static void test_e02(void) {
    cap_slot_t root = cap_channel_create("test-e2", 4, CAP_RIGHT_ALL);
    if (root == CAP_INVALID_SLOT) {
        arch_console_print("    [FAIL] E02 setup failed\n");
        g_fail++;
        return;
    }

    s64 h1 = sched_create_task(helper_yield_exit, NULL, "helper-e2a");
    s64 h2 = sched_create_task(helper_yield_exit, NULL, "helper-e2b");
    if (h1 <= 0 || h2 <= 0) {
        arch_console_print("    [FAIL] E02 helper create failed\n");
        g_fail++;
        cap_destroy_channel(root);
        return;
    }
    struct task_struct *hp1 = sched_get_task_by_id((u64)h1);
    struct task_struct *hp2 = sched_get_task_by_id((u64)h2);

    s64 s1 = cap_mint(root, hp1, CAP_RIGHT_SEND);
    s64 s2 = cap_mint(root, hp2, CAP_RIGHT_RECV);
    TEST_CHECK_BOOL("E02a mint to helper1 succeeds", s1 > 0);
    TEST_CHECK_BOOL("E02b mint to helper2 succeeds", s2 > 0);

    /* revoke 应该清 2 个（两个 helper 各 1 个） */
    int revoked = cap_revoke(root);
    TEST_CHECK_BOOL("E02c cap_revoke returns count >= 2", revoked >= 2);

    /* 验证两个 helper 的 cap 都被清 */
    int both_cleared = 0;
    if (s1 > 0 && s2 > 0 && hp1 && hp2) {
        both_cleared = (!hp1->cspace->slots[s1].in_use &&
                        !hp2->cspace->slots[s2].in_use) ? 1 : 0;
    }
    TEST_CHECK_BOOL("E02d both helpers' caps cleared", both_cleared);

    cap_destroy_channel(root);
    sched_yield();
    for (int i = 0; i < 20 && sched_num_tasks() > 1; i++) sched_sleep(1);
}

/* E3: cap_destroy_channel 清掉其他任务里指向该 channel 的所有 cap */
static void test_e03(void) {
    cap_slot_t root = cap_channel_create("test-e3", 4, CAP_RIGHT_ALL);
    if (root == CAP_INVALID_SLOT) {
        arch_console_print("    [FAIL] E03 setup failed\n");
        g_fail++;
        return;
    }

    s64 h = sched_create_task(helper_yield_exit, NULL, "helper-e3");
    if (h <= 0) {
        arch_console_print("    [FAIL] E03 helper create failed\n");
        g_fail++;
        cap_destroy_channel(root);
        return;
    }
    struct task_struct *hp = sched_get_task_by_id((u64)h);

    /* mint 多个 cap 到 helper（都指向 root 的 channel） */
    s64 s1 = cap_mint(root, hp, CAP_RIGHT_SEND);
    s64 s2 = cap_mint(root, hp, CAP_RIGHT_RECV);
    s64 s3 = cap_mint(root, hp, CAP_RIGHT_SEND | CAP_RIGHT_RECV);
    TEST_CHECK_BOOL("E03a mint 3 caps to helper", s1 > 0 && s2 > 0 && s3 > 0);

    /* destroy channel：应该清掉 init 的 root + helper 的 3 个 cap */
    int rc = cap_destroy_channel(root);
    TEST_CHECK_INT("E03b cap_destroy_channel returns OK", CAP_OK, rc);

    /* 验证 helper 的 3 个 cap 都被清 */
    int all_cleared = 0;
    if (s1 > 0 && s2 > 0 && s3 > 0 && hp) {
        all_cleared = (!hp->cspace->slots[s1].in_use &&
                       !hp->cspace->slots[s2].in_use &&
                       !hp->cspace->slots[s3].in_use) ? 1 : 0;
    }
    TEST_CHECK_BOOL("E03c all 3 helper caps cleared by destroy", all_cleared);

    /* 验证 init 的 root cap 也被清 */
    int init_cleared = current->cspace->slots[root].in_use ? 0 : 1;
    TEST_CHECK_BOOL("E03d init root cap also cleared", init_cleared);

    sched_yield();
    for (int i = 0; i < 20 && sched_num_tasks() > 1; i++) sched_sleep(1);
}

/* E4: destroy_channel 后，helper 的悬空 cap 做 cap_send → NORIGHT
 *   （这是 E3 的延伸：不仅 cap 被清，而且 cap_send 应该拒绝）
 *   注意：E3 已经验证 cap 被清，这里额外验证 cap_send 行为 */
static void test_e04(void) {
    cap_slot_t root = cap_channel_create("test-e4", 4, CAP_RIGHT_ALL);
    if (root == CAP_INVALID_SLOT) {
        arch_console_print("    [FAIL] E04 setup failed\n");
        g_fail++;
        return;
    }

    s64 h = sched_create_task(helper_yield_exit, NULL, "helper-e4");
    if (h <= 0) {
        cap_destroy_channel(root);
        return;
    }
    struct task_struct *hp = sched_get_task_by_id((u64)h);
    s64 hs = cap_mint(root, hp, CAP_RIGHT_SEND);
    TEST_CHECK_BOOL("E04a mint to helper", hs > 0);

    /* destroy channel → helper 的 cap 被清 */
    cap_destroy_channel(root);

    /* 验证 helper 的 cap 已不可用（in_use=0）
     *   注意：我们不能让 helper 调 cap_send（helper 还没跑且 cspace 已空）
     *   所以这里直接检查 cspace 字段 */
    int helper_cap_dead = 0;
    if (hs > 0 && hp) {
        helper_cap_dead = hp->cspace->slots[hs].in_use ? 0 : 1;
    }
    TEST_CHECK_BOOL("E04b helper cap cleared after destroy", helper_cap_dead);

    sched_yield();
    for (int i = 0; i < 20 && sched_num_tasks() > 1; i++) sched_sleep(1);
}

static void run_section_e(void) {
    SAVE_COUNTS();
    section_header("Section E: Cross-Task — revoke + destroy_channel");
    test_e01();
    test_e02();
    test_e03();
    test_e04();
    RESTORE_SECTION("Section E");
}

/* ================================================================
 * Section F: cap 传递 — send_with_cap / recv_with_cap
 *
 *   F1-F3 已在 Section A 测过边界（invalid/unused transfer slot）。
 *   这里测正常路径 + "无 cap 传递"路径，需要 sender + receiver。
 * ================================================================ */

/* sender 参数 */
struct tx_sender_args {
    cap_slot_t chan_slot;        /* 发消息用的 SEND cap */
    cap_slot_t transfer_slot;    /* 附带传递的 cap（CAP_INVALID_SLOT 表示不传） */
    int use_with_cap;            /* 1 = cap_send_with_cap, 0 = cap_send */
    int result;                  /* 返回码 */
};

/* receiver 参数 */
struct tx_receiver_args {
    cap_slot_t chan_slot;        /* 收消息用的 RECV cap */
    cap_slot_t out_cap_slot;     /* 收到的 cap slot（CAP_INVALID_SLOT 表示没附带） */
    u64 msg_type;                /* 收到的消息类型 */
    char buf[IPC_MAX_PAYLOAD + 1];
    u64 buf_len;
    int result;                  /* cap_recv_with_cap 返回码 */
    /* 收到 cap 后，receiver 在自己还活着时把 cap 元数据记到这些字段，
     * 这样 init 在 receiver 退出（cspace 被 kfree）之后还能验证。
     * 否则 init 读 rcv->cspace->slots[...] 会读到已释放的堆内存。 */
    u32 cap_type;                /* 收到的 cap 的 type */
    u32 cap_rights;              /* 收到的 cap 的 rights */
    int cap_in_use;              /* 收到的 cap slot 是否 in_use */
};

static void task_tx_sender(void *arg) {
    struct tx_sender_args *a = (struct tx_sender_args *)arg;
    const char *msg = "tx-test-payload";
    u64 len = ct_strlen(msg) + 1;
    if (a->use_with_cap) {
        a->result = cap_send_with_cap(a->chan_slot, MSG_TX_TEST,
                                       msg, len, a->transfer_slot);
    } else {
        a->result = cap_send(a->chan_slot, MSG_TX_TEST, msg, len);
    }
}

static void task_tx_receiver(void *arg) {
    struct tx_receiver_args *a = (struct tx_receiver_args *)arg;
    a->out_cap_slot = CAP_INVALID_SLOT;
    a->buf_len = sizeof(a->buf) - 1;
    a->cap_type = 0;
    a->cap_rights = 0;
    a->cap_in_use = 0;
    a->result = cap_recv_with_cap(a->chan_slot, &a->msg_type,
                                   a->buf, &a->buf_len, &a->out_cap_slot);
    /* 收到 cap 后，趁 receiver 还活着（cspace 未被 kfree），
     * 把 cap 元数据读到 args 里，让 init 事后能验证 */
    if (a->out_cap_slot != CAP_INVALID_SLOT && current && current->cspace) {
        struct cap *c = &current->cspace->slots[a->out_cap_slot];
        a->cap_type   = c->type;
        a->cap_rights = c->rights;
        a->cap_in_use = c->in_use;
    }
}

/* F1: 正常 cap 传递 — sender 传一个 cap，receiver 收到并安装 */
static void test_f01(void) {
    /* 主通道：sender 用 SEND，receiver 用 RECV */
    cap_slot_t main_root = cap_channel_create("f1-main", 4, CAP_RIGHT_ALL);
    if (main_root == CAP_INVALID_SLOT) {
        arch_console_print("    [FAIL] F01 setup main channel failed\n");
        g_fail++;
        return;
    }
    /* 辅助通道：要传递的 cap 指向它（SEND 权限） */
    cap_slot_t aux_root = cap_channel_create("f1-aux", 4, CAP_RIGHT_ALL);
    if (aux_root == CAP_INVALID_SLOT) {
        arch_console_print("    [FAIL] F01 setup aux channel failed\n");
        g_fail++;
        cap_destroy_channel(main_root);
        return;
    }

    /* mint SEND 给 sender（主通道）+ mint SEND 给 sender（辅助通道，用于传递） */
    /* 但 sender 需要两个 cap：一个用来发消息（main），一个用来传递（aux） */
    /* 先在 init cspace 里 mint 一个 aux 的 SEND cap，再 mint 给 sender */
    /* 实际上：直接从 aux_root mint 到 sender 即可（aux_root 有 MINT） */

    static struct tx_sender_args sargs;
    static struct tx_receiver_args rargs;
    sargs.chan_slot = CAP_INVALID_SLOT;
    sargs.transfer_slot = CAP_INVALID_SLOT;
    sargs.use_with_cap = 1;
    sargs.result = -999;
    rargs.chan_slot = CAP_INVALID_SLOT;
    rargs.out_cap_slot = CAP_INVALID_SLOT;
    rargs.result = -999;
    rargs.cap_type = 0;
    rargs.cap_rights = 0;
    rargs.cap_in_use = 0;

    /* 创建 sender / receiver（cspace 已初始化，但还没跑） */
    s64 sid = sched_create_task(task_tx_sender, &sargs, "tx-snd-f1");
    s64 rid = sched_create_task(task_tx_receiver, &rargs, "tx-rcv-f1");
    if (sid <= 0 || rid <= 0) {
        arch_console_print("    [FAIL] F01 task create failed\n");
        g_fail++;
        cap_destroy_channel(main_root);
        cap_destroy_channel(aux_root);
        return;
    }
    struct task_struct *snd = sched_get_task_by_id((u64)sid);
    struct task_struct *rcv = sched_get_task_by_id((u64)rid);

    /* mint SEND on main_chan to sender */
    s64 snd_main = cap_mint(main_root, snd, CAP_RIGHT_SEND);
    s64 rcv_main = cap_mint(main_root, rcv, CAP_RIGHT_RECV);
    TEST_CHECK_BOOL("F01a mint SEND to sender", snd_main > 0);
    TEST_CHECK_BOOL("F01b mint RECV to receiver", rcv_main > 0);

    /* mint SEND on aux_chan to sender（这个 cap 会被传递） */
    s64 snd_aux = cap_mint(aux_root, snd, CAP_RIGHT_SEND);
    TEST_CHECK_BOOL("F01c mint aux SEND to sender", snd_aux > 0);

    sargs.chan_slot = (cap_slot_t)snd_main;
    sargs.transfer_slot = (cap_slot_t)snd_aux;
    rargs.chan_slot = (cap_slot_t)rcv_main;

    /* 让 sender / receiver 跑 */
    sched_yield();
    /* 等两个任务都退出 */
    for (int i = 0; i < 30 && sched_num_tasks() > 1; i++) sched_sleep(1);

    /* 验证 sender 成功发送 */
    TEST_CHECK_INT("F01d sender cap_send_with_cap returns OK",
                   CAP_OK, sargs.result);

    /* 验证 receiver 成功接收 */
    TEST_CHECK_INT("F01e receiver cap_recv_with_cap returns OK",
                   CAP_OK, rargs.result);

    /* 验证 receiver 收到了 cap（out_cap_slot != INVALID） */
    TEST_CHECK_BOOL("F01f receiver got cap installed (out_cap_slot != 0)",
                    rargs.out_cap_slot != CAP_INVALID_SLOT);

    /* 验证 receiver cspace 里那个 slot 确实在用
     *   注意：receiver 退出后 cspace 被 kfree，不能直接读 rcv->cspace。
     *   receiver 在退出前已把 cap 元数据存到 rargs.cap_* 字段里。 */
    if (rargs.out_cap_slot != CAP_INVALID_SLOT) {
        TEST_CHECK_BOOL("F01g received cap slot is in_use",
                        rargs.cap_in_use);

        /* 验证收到的 cap 类型是 CHANNEL */
        TEST_CHECK_INT("F01h received cap type is CHANNEL",
                       CAP_TYPE_CHANNEL, (s64)rargs.cap_type);

        /* 验证收到的 cap 权限是 SEND（我们传的是 SEND-only） */
        TEST_CHECK_INT("F01i received cap rights is SEND",
                       CAP_RIGHT_SEND, (s64)rargs.cap_rights);
    }

    /* 清理 */
    cap_destroy_channel(main_root);
    cap_destroy_channel(aux_root);
    /* 万一任务还在，再 yield 一次让它们退出 */
    sched_yield();
    for (int i = 0; i < 10 && sched_num_tasks() > 1; i++) sched_sleep(1);
}

/* F2: 消息不带 cap — receiver 的 out_cap_slot 应该是 INVALID */
static void test_f02(void) {
    cap_slot_t root = cap_channel_create("f2-main", 4, CAP_RIGHT_ALL);
    if (root == CAP_INVALID_SLOT) {
        arch_console_print("    [FAIL] F02 setup failed\n");
        g_fail++;
        return;
    }

    static struct tx_sender_args sargs;
    static struct tx_receiver_args rargs;
    sargs.chan_slot = CAP_INVALID_SLOT;
    sargs.transfer_slot = CAP_INVALID_SLOT;
    sargs.use_with_cap = 0;   /* 用 cap_send（不传 cap） */
    sargs.result = -999;
    rargs.chan_slot = CAP_INVALID_SLOT;
    rargs.out_cap_slot = CAP_INVALID_SLOT;
    rargs.result = -999;

    s64 sid = sched_create_task(task_tx_sender, &sargs, "tx-snd-f2");
    s64 rid = sched_create_task(task_tx_receiver, &rargs, "tx-rcv-f2");
    if (sid <= 0 || rid <= 0) {
        arch_console_print("    [FAIL] F02 task create failed\n");
        g_fail++;
        cap_destroy_channel(root);
        return;
    }
    struct task_struct *snd = sched_get_task_by_id((u64)sid);
    struct task_struct *rcv = sched_get_task_by_id((u64)rid);

    s64 ss = cap_mint(root, snd, CAP_RIGHT_SEND);
    s64 rs = cap_mint(root, rcv, CAP_RIGHT_RECV);
    TEST_CHECK_BOOL("F02a mint to sender+receiver", ss > 0 && rs > 0);

    sargs.chan_slot = (cap_slot_t)ss;
    rargs.chan_slot = (cap_slot_t)rs;

    sched_yield();
    for (int i = 0; i < 30 && sched_num_tasks() > 1; i++) sched_sleep(1);

    TEST_CHECK_INT("F02b sender cap_send (plain) returns OK",
                   CAP_OK, sargs.result);
    TEST_CHECK_INT("F02c receiver cap_recv_with_cap returns OK",
                   CAP_OK, rargs.result);
    /* 关键验证：out_cap_slot 应该是 INVALID（消息没附带 cap） */
    TEST_CHECK_INT("F02d receiver out_cap_slot is INVALID (no cap sent)",
                   (s64)CAP_INVALID_SLOT, (s64)rargs.out_cap_slot);

    cap_destroy_channel(root);
    sched_yield();
    for (int i = 0; i < 10 && sched_num_tasks() > 1; i++) sched_sleep(1);
}

/* F3: cap_send_with_cap 用一个 ALL-rights cap 做 transfer
 *   验证 receiver 收到的 cap 权限 = src cap 的权限（快照模型） */
static void test_f03(void) {
    cap_slot_t main_root = cap_channel_create("f3-main", 4, CAP_RIGHT_ALL);
    cap_slot_t aux_root = cap_channel_create("f3-aux", 4, CAP_RIGHT_ALL);
    if (main_root == CAP_INVALID_SLOT || aux_root == CAP_INVALID_SLOT) {
        arch_console_print("    [FAIL] F03 setup failed\n");
        g_fail++;
        if (main_root != CAP_INVALID_SLOT) cap_destroy_channel(main_root);
        if (aux_root != CAP_INVALID_SLOT) cap_destroy_channel(aux_root);
        return;
    }

    static struct tx_sender_args sargs;
    static struct tx_receiver_args rargs;
    sargs.use_with_cap = 1;
    sargs.result = -999;
    rargs.out_cap_slot = CAP_INVALID_SLOT;
    rargs.result = -999;
    rargs.cap_type = 0;
    rargs.cap_rights = 0;
    rargs.cap_in_use = 0;

    s64 sid = sched_create_task(task_tx_sender, &sargs, "tx-snd-f3");
    s64 rid = sched_create_task(task_tx_receiver, &rargs, "tx-rcv-f3");
    if (sid <= 0 || rid <= 0) {
        arch_console_print("    [FAIL] F03 task create failed\n");
        g_fail++;
        cap_destroy_channel(main_root);
        cap_destroy_channel(aux_root);
        return;
    }
    struct task_struct *snd = sched_get_task_by_id((u64)sid);
    struct task_struct *rcv = sched_get_task_by_id((u64)rid);

    s64 snd_main = cap_mint(main_root, snd, CAP_RIGHT_SEND);
    s64 rcv_main = cap_mint(main_root, rcv, CAP_RIGHT_RECV);
    /* 传一个 ALL-rights 的 aux cap（注意：aux_root 自己是 ALL） */
    /* 直接传 aux_root（init 的 cap），但 sender 需要持有它才能传 */
    /* 所以先 mint aux_root 到 sender */
    s64 snd_aux = cap_mint(aux_root, snd, CAP_RIGHT_ALL);
    TEST_CHECK_BOOL("F03a mint aux ALL to sender", snd_aux > 0);

    sargs.chan_slot = (cap_slot_t)snd_main;
    sargs.transfer_slot = (cap_slot_t)snd_aux;
    rargs.chan_slot = (cap_slot_t)rcv_main;

    sched_yield();
    for (int i = 0; i < 30 && sched_num_tasks() > 1; i++) sched_sleep(1);

    TEST_CHECK_INT("F03b sender returns OK", CAP_OK, sargs.result);
    TEST_CHECK_INT("F03c receiver returns OK", CAP_OK, rargs.result);
    TEST_CHECK_BOOL("F03d receiver got cap installed",
                    rargs.out_cap_slot != CAP_INVALID_SLOT);

    /* receiver 退出前已把 cap 元数据存到 rargs.cap_* 字段 */
    if (rargs.out_cap_slot != CAP_INVALID_SLOT) {
        TEST_CHECK_INT("F03e received cap rights = ALL (snapshot)",
                       CAP_RIGHT_ALL, (s64)rargs.cap_rights);
    } else {
        arch_console_print("    [FAIL] F03e cannot verify rights (no cap)\n");
        g_fail++;
    }

    cap_destroy_channel(main_root);
    cap_destroy_channel(aux_root);
    sched_yield();
    for (int i = 0; i < 10 && sched_num_tasks() > 1; i++) sched_sleep(1);
}

static void run_section_f(void) {
    SAVE_COUNTS();
    section_header("Section F: Cap Transfer — send_with_cap + recv_with_cap");
    test_f01();
    test_f02();
    test_f03();
    RESTORE_SECTION("Section F");
}

/* ================================================================
 * Section G: 压力测试
 *
 *   - G1: N× create/destroy channel，验证 cap_total_caps 回到基线（无 leak）
 *   - G2: N× mint/delete cap，验证无 leak
 *   - G3: N× mint/revoke cycle，验证 src 保留 + 派生被清
 *   - G4: fill + delete + refill 循环（slot 复用压力）
 *   - G5: N× cap transfer round-trip，验证 cap 总数稳定
 * ================================================================ */

/* G1: 反复 create/destroy channel */
static void test_g01(void) {
    int N = 50;
    int baseline = cap_total_caps();
    int ok = 1;

    for (int i = 0; i < N; i++) {
        cap_slot_t s = cap_channel_create("g01", 4, CAP_RIGHT_ALL);
        if (s == CAP_INVALID_SLOT) { ok = 0; break; }
        int rc = cap_destroy_channel(s);
        if (rc != CAP_OK) { ok = 0; break; }
    }
    int after = cap_total_caps();

    TEST_CHECK_BOOL("G01a 50x create/destroy all succeed", ok);
    TEST_CHECK_INT("G01b cap_total_caps returns to baseline (no leak)",
                   baseline, after);
}

/* G2: 反复 mint/delete cap（不 destroy channel） */
static void test_g02(void) {
    int N = 50;
    cap_slot_t root = cap_channel_create("g02", 4, CAP_RIGHT_ALL);
    if (root == CAP_INVALID_SLOT) {
        arch_console_print("    [FAIL] G02 setup failed\n");
        g_fail++;
        return;
    }
    int baseline = cap_total_caps();
    int ok = 1;

    for (int i = 0; i < N; i++) {
        s64 s = cap_mint(root, current, CAP_RIGHT_SEND);
        if (s < 0) { ok = 0; break; }
        int rc = cap_delete((cap_slot_t)s);
        if (rc != CAP_OK) { ok = 0; break; }
    }
    int after = cap_total_caps();

    TEST_CHECK_BOOL("G02a 50x mint/delete all succeed", ok);
    /* after 应该 == baseline（mint+delete 抵消，root 一直在） */
    TEST_CHECK_INT("G02b cap_total_caps stable (mint/delete balanced)",
                   baseline, after);

    cap_destroy_channel(root);
}

/* G3: 反复 mint/revoke cycle */
static void test_g03(void) {
    int N = 30;
    cap_slot_t root = cap_channel_create("g03", 4, CAP_RIGHT_ALL);
    if (root == CAP_INVALID_SLOT) {
        arch_console_print("    [FAIL] G03 setup failed\n");
        g_fail++;
        return;
    }
    int ok = 1;
    int revoke_total = 0;

    /* 每轮：mint 一个 cap，revoke，验证 root 还在 */
    for (int i = 0; i < N; i++) {
        s64 s = cap_mint(root, current, CAP_RIGHT_SEND);
        if (s < 0) { ok = 0; break; }
        int rc = cap_revoke(root);
        if (rc < 1) { ok = 0; break; }   /* 至少 revoke 1 个（刚 mint 的） */
        revoke_total += rc;
    }
    /* 验证 root 仍在 */
    int root_alive = current->cspace->slots[root].in_use ? 1 : 0;

    TEST_CHECK_BOOL("G03a 30x mint/revoke all succeed", ok);
    TEST_CHECK_BOOL("G03b root cap still alive after 30 revokes", root_alive);
    TEST_CHECK_BOOL("G03c total revoked >= 30",
                    revoke_total >= 30);

    cap_destroy_channel(root);
}

/* G4: fill + delete + refill 循环（slot 复用压力） */
static void test_g04(void) {
    int N = 5;   /* 5 轮 fill + clear */
    int ok = 1;

    for (int round = 0; round < N; round++) {
        /* fill */
        cap_slot_t slots[CAP_SLOTS_PER_TASK];
        int created = 0;
        for (int i = 1; i < CAP_SLOTS_PER_TASK; i++) {
            slots[i] = cap_channel_create("g04", 2, CAP_RIGHT_ALL);
            if (slots[i] == CAP_INVALID_SLOT) break;
            created++;
        }
        if (created != CAP_SLOTS_PER_TASK - 1) { ok = 0; break; }

        /* clear all */
        for (int i = 1; i < CAP_SLOTS_PER_TASK; i++) {
            if (slots[i] != CAP_INVALID_SLOT) {
                cap_destroy_channel(slots[i]);
            }
        }
        /* 验证 cspace 回到空 */
        int total = cap_total_caps();
        if (total != 0) { ok = 0; break; }
    }

    TEST_CHECK_BOOL("G04 5x fill(31) + clear cycles, slot reuse OK", ok);
}

/* G5: N× cap transfer round-trip，验证 cap 总数稳定
 *   每轮：sender 传 cap → receiver 收 → receiver 删 cap → 全部清理 */
static void test_g05(void) {
    int N = 10;
    int baseline = cap_total_caps();
    int ok = 1;

    for (int round = 0; round < N; round++) {
        cap_slot_t main_root = cap_channel_create("g5m", 4, CAP_RIGHT_ALL);
        cap_slot_t aux_root = cap_channel_create("g5a", 4, CAP_RIGHT_ALL);
        if (main_root == CAP_INVALID_SLOT || aux_root == CAP_INVALID_SLOT) {
            ok = 0;
            break;
        }

        static struct tx_sender_args sargs;
        static struct tx_receiver_args rargs;
        sargs.chan_slot = CAP_INVALID_SLOT;
        sargs.transfer_slot = CAP_INVALID_SLOT;
        sargs.use_with_cap = 1;
        sargs.result = -999;
        rargs.chan_slot = CAP_INVALID_SLOT;
        rargs.out_cap_slot = CAP_INVALID_SLOT;
        rargs.result = -999;
        rargs.cap_type = 0;
        rargs.cap_rights = 0;
        rargs.cap_in_use = 0;

        s64 sid = sched_create_task(task_tx_sender, &sargs, "g5-snd");
        s64 rid = sched_create_task(task_tx_receiver, &rargs, "g5-rcv");
        if (sid <= 0 || rid <= 0) { ok = 0; break; }
        struct task_struct *snd = sched_get_task_by_id((u64)sid);
        struct task_struct *rcv = sched_get_task_by_id((u64)rid);

        s64 sm = cap_mint(main_root, snd, CAP_RIGHT_SEND);
        s64 rm = cap_mint(main_root, rcv, CAP_RIGHT_RECV);
        s64 sa = cap_mint(aux_root, snd, CAP_RIGHT_SEND);
        if (sm <= 0 || rm <= 0 || sa <= 0) { ok = 0; break; }

        sargs.chan_slot = (cap_slot_t)sm;
        sargs.transfer_slot = (cap_slot_t)sa;
        rargs.chan_slot = (cap_slot_t)rm;

        sched_yield();
        for (int i = 0; i < 30 && sched_num_tasks() > 1; i++) sched_sleep(1);

        if (sargs.result != CAP_OK || rargs.result != CAP_OK) {
            ok = 0;
        }
        if (rargs.out_cap_slot == CAP_INVALID_SLOT) {
            ok = 0;
        }

        /* 清理两个 channel（会清所有 cap 包括传递的） */
        cap_destroy_channel(main_root);
        cap_destroy_channel(aux_root);
        sched_yield();
        for (int i = 0; i < 10 && sched_num_tasks() > 1; i++) sched_sleep(1);
    }

    int after = cap_total_caps();
    TEST_CHECK_BOOL("G05a 10x transfer round-trip all succeed", ok);
    TEST_CHECK_INT("G05b cap_total_caps returns to baseline (no leak)",
                   baseline, after);
}

static void run_section_g(void) {
    SAVE_COUNTS();
    section_header("Section G: Stress — Cycles + Leak Detection");
    test_g01();
    test_g02();
    test_g03();
    test_g04();
    test_g05();
    RESTORE_SECTION("Section G");
}

/* ================================================================
 * cap_test_run_all — 执行全部测试 + 汇总
 * ================================================================ */
int cap_test_run_all(int quiet) {
    g_quiet = quiet;

    if (!quiet) {
        arch_console_set_color(CON_COLOR_CYAN);
        arch_console_print("\n========================================\n");
        arch_console_print("Lesson 7: Boundary + Stress Tests\n");
        arch_console_print("========================================\n");
        arch_console_set_color(CON_COLOR_DEFAULT);
    }

    int before = cap_total_caps();
    if (!quiet) {
        arch_console_print("  Baseline cap_total_caps before tests: ");
        kprint_dec((u64)before);
        arch_console_print("\n");
    }

    g_pass = 0;
    g_fail = 0;

    run_section_a();
    run_section_b();
    run_section_c();
    run_section_d();
    run_section_e();
    run_section_f();
    run_section_g();

    int after = cap_total_caps();

    if (!quiet) {
        arch_console_set_color(CON_COLOR_CYAN);
        arch_console_print("\n========================================\n");
        arch_console_print("Boundary + Stress Test Summary\n");
        arch_console_print("========================================\n");
        arch_console_set_color(CON_COLOR_DEFAULT);

        arch_console_print("  Total PASS: ");
        arch_console_set_color(CON_COLOR_GREEN);
        kprint_dec((u64)g_pass);
        arch_console_set_color(CON_COLOR_DEFAULT);
        arch_console_print("\n");

        arch_console_print("  Total FAIL: ");
        if (g_fail > 0) arch_console_set_color(CON_COLOR_RED);
        kprint_dec((u64)g_fail);
        arch_console_set_color(CON_COLOR_DEFAULT);
        arch_console_print("\n");

        arch_console_print("  cap_total_caps before: ");
        kprint_dec((u64)before);
        arch_console_print("  after: ");
        kprint_dec((u64)after);
        if (before == after) {
            arch_console_print("  [NO LEAK]");
        } else {
            arch_console_set_color(CON_COLOR_RED);
            arch_console_print("  [LEAK DETECTED]");
            arch_console_set_color(CON_COLOR_DEFAULT);
        }
        arch_console_print("\n");

        if (g_fail == 0) {
            arch_console_set_color(CON_COLOR_GREEN);
            arch_console_print("\n  *** ALL BOUNDARY + STRESS TESTS PASSED ***\n");
            arch_console_set_color(CON_COLOR_DEFAULT);
        } else {
            arch_console_set_color(CON_COLOR_RED);
            arch_console_print("\n  *** SOME TESTS FAILED — SEE ABOVE ***\n");
            arch_console_set_color(CON_COLOR_DEFAULT);
        }
    }

    /* quiet 模式下由调用方（main.c）负责打印汇总 */
    return g_pass;
}
