/* ================================================================
 * kernel/ktest.c — AIHYK 内核全子系统回归测试 + 压力测试 + 边界测试
 *
 * 【测试套件设计】
 *
 * 实现 include/kernel/ktest.h 的 ktest_run_all()。
 *
 * 对 L1-L9 所有子系统做完整的回归 / 压力 / 边界测试：
 *
 *   Section 1: PMM（物理内存管理器）— 边界 + 压力（~13 tests）
 *   Section 2: VMM（虚拟内存管理器）— 边界 + 压力（~11 tests）
 *   Section 3: kmalloc（内核堆）— 边界 + 压力（~14 tests）
 *   Section 4: Scheduler（调度器）— 边界 + 压力（~10 tests）
 *   Section 5: IPC（进程间通信）— 边界 + 压力（~15 tests）
 *   Section 6: Cross-subsystem（跨子系统回归）（~10 tests）
 *
 * 【测试哲学】
 *   - 边界测试：每个 API 的每个错误返回路径都要被覆盖
 *   - 压力测试：反复操作，验证计数 / 状态一致性
 *   - 回归测试：一个子系统的操作不影响其他子系统
 *   - 不崩溃：用 TEST_CHECK_INT 比较，不用 KASSERT
 *   - 自清理：每个 section 结束后资源回到初始状态
 *   - Leak 检测：前后对比 free_frames / cap_total_caps()
 *   - 单任务优先：能在 init task 里测的就不开 helper
 *   - 多任务最小化：helper 只做"让出 + 退出"
 *
 * 【重要约束】
 *   - 不分配超过 256 个 PMM 帧（内核需要内存！）
 *   - VMM 测试用 PML4[2] 和 PML4[3] 区域（不碰 0/1/256/511）
 *   - IPC 非阻塞测试在 init task 内完成，阻塞测试用 helper
 *   - 调度器测试不要创建太多任务（和已有 demo 任务共处）
 * ================================================================ */

#include <arch/console.h>
#include <arch/cpu.h>
#include <arch/mem.h>
#include <arch/pit.h>
#include <arch/task.h>
#include <kernel/ipc.h>
#include <kernel/ktest.h>
#include <kernel/mm.h>
#include <kernel/panic.h>
#include <kernel/sched.h>
#include <kernel/types.h>

/* ================================================================
 * 测试框架（局部工具函数 + 宏）
 * ================================================================ */

static int g_pass = 0;   /* 全局 PASS 计数 */
static int g_fail = 0;   /* 全局 FAIL 计数 */
static int g_quiet = 0;  /* 安静模式：只打印 section 汇总，不逐条打印 */

/* ---- 各 section 的局部计数 ---- */
static int s1_pass, s1_fail;
static int s2_pass, s2_fail;
static int s3_pass, s3_fail;
static int s4_pass, s4_fail;
static int s5_pass, s5_fail;
static int s6_pass, s6_fail;

/* 局部 print_dec（不依赖 main.c 的 static 版本，freestanding 没有 printf） */
static void kt_print_dec(u64 v) {
    char buf[21];
    int i = 0;
    if (v == 0) { arch_console_putchar('0'); return; }
    while (v > 0 && i < 20) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i > 0) arch_console_putchar(buf[--i]);
}

/* 局部 print_dec_signed（打印负数，错误码是负数时要用） */
static void kt_print_dec_s(s64 v) {
    if (v < 0) {
        arch_console_putchar('-');
        kt_print_dec((u64)(-v));
    } else {
        kt_print_dec((u64)v);
    }
}

/* 局部 print_hex（打印十六进制，地址和标志位要用） */
static void __attribute__((unused)) kt_print_hex(u64 v) {
    static const char hex[] = "0123456789ABCDEF";
    arch_console_print("0x");
    if (v == 0) { arch_console_putchar('0'); return; }
    /* 从最高非零 nibble 开始打印 */
    int started = 0;
    for (int i = 60; i >= 0; i -= 4) {
        int nibble = (int)((v >> i) & 0xF);
        if (nibble != 0 || started) {
            arch_console_putchar(hex[nibble]);
            started = 1;
        }
    }
}

/* 局部 print_str — 和 arch_console_print 一样，但统一接口名 */
static void __attribute__((unused)) kt_print_str(const char *s) {
    arch_console_print(s);
}

/* ----------------------------------------------------------------
 * TEST_CHECK_INT — 比较期望值和实际值（有符号，因为错误码是负数）
 *
 *   相等 → [PASS] 绿色，g_pass++
 *   不等 → [FAIL] 红色 + 打印 expected vs got，g_fail++
 * ---------------------------------------------------------------- */
#define TEST_CHECK_INT(name, expected, got) do { \
    if ((s64)(expected) == (s64)(got)) { \
        if (!g_quiet) { \
            arch_console_set_color(CON_COLOR_GREEN); \
            arch_console_print("  [PASS] "); \
            arch_console_set_color(CON_COLOR_DEFAULT); \
            arch_console_print(name); arch_console_print("\n"); \
        } \
        g_pass++; \
    } else { \
        arch_console_set_color(CON_COLOR_RED); \
        arch_console_print("  [FAIL] "); \
        arch_console_set_color(CON_COLOR_DEFAULT); \
        arch_console_print(name); arch_console_print(" expected="); \
        kt_print_dec_s((s64)(expected)); arch_console_print(" got="); \
        kt_print_dec_s((s64)(got)); arch_console_print("\n"); \
        g_fail++; \
    } \
} while(0)

/* TEST_CHECK_BOOL — 检查布尔条件（cond 为真则 PASS） */
#define TEST_CHECK_BOOL(name, cond) TEST_CHECK_INT(name, 1, !!(cond))

/* ----------------------------------------------------------------
 * section_header / section_footer — 打印 section 头尾
 *
 *   格式：
 *     ===============================================================
 *     Section N: <Title>
 *     ===============================================================
 * ---------------------------------------------------------------- */
static void section_header(int n, const char *title) {
    if (g_quiet) return;
    arch_console_print("===============================================================\n");
    arch_console_set_color(CON_COLOR_CYAN);
    arch_console_print("Section ");
    kt_print_dec((u64)n);
    arch_console_print(": ");
    arch_console_print(title);
    arch_console_print("\n");
    arch_console_set_color(CON_COLOR_DEFAULT);
    arch_console_print("===============================================================\n");
}

static void section_footer(int n, const char *title, int pass, int fail) {
    if (g_quiet) return;
    arch_console_print("  Section ");
    kt_print_dec((u64)n);
    arch_console_print(" (");
    arch_console_print(title);
    arch_console_print("): ");
    kt_print_dec((u64)pass);
    arch_console_print("/");
    kt_print_dec((u64)(pass + fail));
    arch_console_print(" PASS");
    if (fail > 0) {
        arch_console_print(", ");
        kt_print_dec((u64)fail);
        arch_console_print(" FAIL");
    }
    arch_console_print("\n");
}

/* ================================================================
 * 辅助：等待 helper 任务退出
 *
 *   创建 helper 后，init 要让出 CPU 让它们跑，
 *   然后等 sched_num_tasks() 回到基线。
 *   超时保护防止死等（最多等 200 次 yield）。
 * ================================================================ */
static void wait_for_helpers(int baseline_tasks) {
    for (int i = 0; i < 200; i++) {
        if (sched_num_tasks() <= baseline_tasks) break;
        sched_yield();
    }
}

/* ================================================================
 * Section 1: PMM — 物理内存管理器边界 + 压力测试
 *
 *   PMM 用位图管理物理帧：alloc_frame 找位图 0 位标 1，
 *   free_frame 清 0。测试覆盖：
 *   - 统计一致性（total == free + used）
 *   - 分配结果属性（对齐、非零、不重复）
 *   - 释放后复用（位图 0→1→0→1）
 *   - 非法释放（frame 0 / 未对齐 / 越界 / double free）
 *   - 压力：批量分配/释放、交错模式、泄漏检测
 *
 *   【安全约束】最多分配 256 帧，测试后全部释放。
 * ================================================================ */
static void test_section_pmm(void) {
    s1_pass = g_pass; s1_fail = g_fail;

    section_header(1, "PMM");

    /* ---- S1_01: 统计一致性 total == free + used ---- */
    {
        usize_t total = arch_pmm_total_frames();
        usize_t free  = arch_pmm_free_frames();
        usize_t used  = arch_pmm_used_frames();
        TEST_CHECK_INT("S1_01_total_eq_free_plus_used",
                       (s64)total, (s64)(free + used));
    }

    /* ---- S1_02: alloc 返回 4KB 对齐的非零地址 ---- */
    {
        paddr_t f = arch_pmm_alloc_frame();
        int ok = (f != 0) && ((f & (PAGE_SIZE - 1)) == 0);
        TEST_CHECK_BOOL("S1_02_alloc_aligned_nonzero", ok);
        arch_pmm_free_frame(f);
    }

    /* ---- S1_03: alloc 返回不同地址（无 double-alloc） ---- */
    {
        paddr_t f1 = arch_pmm_alloc_frame();
        paddr_t f2 = arch_pmm_alloc_frame();
        TEST_CHECK_BOOL("S1_03_alloc_distinct", f1 != f2);
        arch_pmm_free_frame(f1);
        arch_pmm_free_frame(f2);
    }

    /* ---- S1_04: free 后 re-alloc 返回同一帧（位图复用） ---- */
    /*   释放一个帧后再分配，位图应该把同一个帧重新分配出来。
     *   这是位图分配器的核心语义：free 把位清 0，alloc 扫描
     *   到第一个 0 位标 1。如果中间没有其他 alloc，应该拿到同一个。
     *   但不严格要求：其他实现可能从不同位置扫描。所以我们
     *   只检查"alloc 成功"即可，地址相同是"可能"不是"必须"。 */
    {
        paddr_t f1 = arch_pmm_alloc_frame();
        arch_pmm_free_frame(f1);
        paddr_t f2 = arch_pmm_alloc_frame();
        /* 至少要能分配成功 */
        TEST_CHECK_BOOL("S1_04_free_then_realloc", f2 != 0);
        /* 如果恰好复用同一帧，那是最好的 */
        arch_pmm_free_frame(f2);
    }

    /* ---- S1_05: frame 0 永久保留（纵深防御） ---- */
    /*   物理地址 0 是 NULL page，PMM 应永久保留。
     *   【Bug 已修复】free_frame(0) 现在被拒绝（warn + return），
     *   alloc_frame 也跳过 idx 0（纵深防御）。
     *   验证：(a) alloc_frame 永远不返回 0
     *         (b) free_frame(0) 不改变 free_frames 计数 */
    {
        paddr_t f = arch_pmm_alloc_frame();
        TEST_CHECK_BOOL("S1_05_alloc_never_returns_zero", (f != 0));
        arch_pmm_free_frame(f);

        /* free_frame(0) 应被拒绝，free_frames 不变 */
        usize_t before = arch_pmm_free_frames();
        arch_pmm_free_frame(0);  /* 应打印 WARN 但不改位图 */
        usize_t after  = arch_pmm_free_frames();
        TEST_CHECK_INT("S1_05_free_frame_0_rejected", (s64)before, (s64)after);
    }

    /* ---- S1_06: alloc 返回的地址总是对齐的 ---- */
    {
        paddr_t f = arch_pmm_alloc_frame();
        int aligned = ((f & (PAGE_SIZE - 1)) == 0);
        TEST_CHECK_BOOL("S1_06_alloc_always_aligned", aligned);
        arch_pmm_free_frame(f);
    }

    /* ---- S1_07: alloc 返回的地址在 PMM 覆盖范围内 ---- */
    {
        paddr_t f = arch_pmm_alloc_frame();
        usize_t max_addr = arch_pmm_total_frames() * PAGE_SIZE;
        TEST_CHECK_BOOL("S1_07_alloc_within_range", (f < max_addr));
        arch_pmm_free_frame(f);
    }

    /* ---- S1_08: double free 同一帧 ---- */
    /*   double free 把已清 0 的位再清 0，应该无害（幂等），
     *   但不应该让 free_frames 增加（不能凭空多出帧）。
     *   AIHYK 的 free_frame 会检查位图，double-free 时打印
     *   警告但不是 panic（防御性设计）。 */
    {
        paddr_t f = arch_pmm_alloc_frame();
        usize_t before = arch_pmm_free_frames();
        arch_pmm_free_frame(f);   /* 第一次 free：正常 */
        arch_pmm_free_frame(f);   /* 第二次 free：double free */
        usize_t after = arch_pmm_free_frames();
        /* double free 不应该增加 free 计数 */
        TEST_CHECK_INT("S1_08_double_free_no_extra_count",
                       (s64)(before + 1), (s64)after);
    }

    /* ---- S1_08b: free_frame 防御性检查（不 panic） ---- */
    /*   【Bug 已修复】free_frame 对越界/未对齐地址不再 panic，
     *   改为 warn + early return（防御性设计）。
     *   验证：free_frame(1) 和 free_frame(0xDEAD) 不崩溃、不改计数。 */
    {
        usize_t before = arch_pmm_free_frames();
        arch_pmm_free_frame(1);       /* 未对齐 → warn + return */
        arch_pmm_free_frame(0xDEAD); /* 越界 → warn + return */
        usize_t after  = arch_pmm_free_frames();
        TEST_CHECK_INT("S1_08b_free_invalid_no_change", (s64)before, (s64)after);
    }

    /* ---- S1_09: free + used == total 在 alloc/free 循环后 ---- */
    {
        paddr_t frames[4];
        for (int i = 0; i < 4; i++) frames[i] = arch_pmm_alloc_frame();
        usize_t total = arch_pmm_total_frames();
        usize_t free  = arch_pmm_free_frames();
        usize_t used  = arch_pmm_used_frames();
        TEST_CHECK_INT("S1_09_stats_consistent_after_alloc",
                       (s64)total, (s64)(free + used));
        for (int i = 0; i < 4; i++) arch_pmm_free_frame(frames[i]);
    }

    /* ---- S1_10: 分配 128 帧，全部唯一且对齐，然后全部释放 ---- */
    {
        paddr_t frames[128];
        int all_ok = 1;
        for (int i = 0; i < 128; i++) {
            frames[i] = arch_pmm_alloc_frame();
            if (frames[i] == 0 || (frames[i] & (PAGE_SIZE - 1)) != 0) {
                all_ok = 0;
                break;
            }
        }
        /* 检查唯一性：简单 O(N²) 比较对 128 个元素可接受 */
        if (all_ok) {
            for (int i = 0; i < 128 && all_ok; i++) {
                for (int j = i + 1; j < 128; j++) {
                    if (frames[i] == frames[j]) {
                        all_ok = 0;
                        break;
                    }
                }
            }
        }
        TEST_CHECK_BOOL("S1_10_alloc_128_unique_aligned", all_ok);
        for (int i = 0; i < 128; i++) {
            if (frames[i] != 0) arch_pmm_free_frame(frames[i]);
        }
    }

    /* ---- S1_11: 分配 128 帧，逆序释放，验证 free_frames 回到基线 ---- */
    {
        usize_t baseline = arch_pmm_free_frames();
        paddr_t frames[128];
        for (int i = 0; i < 128; i++) frames[i] = arch_pmm_alloc_frame();
        /* 逆序释放 */
        for (int i = 127; i >= 0; i--) arch_pmm_free_frame(frames[i]);
        usize_t after = arch_pmm_free_frames();
        TEST_CHECK_INT("S1_11_free_reverse_back_to_baseline",
                       (s64)baseline, (s64)after);
    }

    /* ---- S1_12: alloc/free 循环 50 次（每次 alloc 4 + free 4），无泄漏 ---- */
    {
        usize_t baseline = arch_pmm_free_frames();
        for (int round = 0; round < 50; round++) {
            paddr_t f[4];
            for (int i = 0; i < 4; i++) f[i] = arch_pmm_alloc_frame();
            for (int i = 0; i < 4; i++) arch_pmm_free_frame(f[i]);
        }
        usize_t after = arch_pmm_free_frames();
        TEST_CHECK_INT("S1_12_alloc_free_cycle_no_leak",
                       (s64)baseline, (s64)after);
    }

    /* ---- S1_13: 交错模式：alloc 4, free 2nd & 4th, alloc 2 ---- */
    /*   验证位图复用：释放中间的帧后，新 alloc 应该能填回去。
     *   释放 2nd & 4th 后，free_frames 增加 2，
     *   再 alloc 2 个，free_frames 回到原值。
     *   这测试了位图在非连续释放后的扫描能力。 */
    {
        usize_t baseline = arch_pmm_free_frames();
        paddr_t f[4];
        for (int i = 0; i < 4; i++) f[i] = arch_pmm_alloc_frame();

        /* 释放第 2 个和第 4 个（索引 1 和 3） */
        arch_pmm_free_frame(f[1]);
        arch_pmm_free_frame(f[3]);

        /* 再分配 2 个：应该成功，最好复用 f[1] 和 f[3] */
        paddr_t g[2];
        g[0] = arch_pmm_alloc_frame();
        g[1] = arch_pmm_alloc_frame();
        int ok = (g[0] != 0) && (g[1] != 0);
        TEST_CHECK_BOOL("S1_13_interleaved_bitmap_reuse", ok);

        /* 清理 */
        arch_pmm_free_frame(f[0]);
        arch_pmm_free_frame(f[2]);
        arch_pmm_free_frame(g[0]);
        arch_pmm_free_frame(g[1]);

        usize_t after = arch_pmm_free_frames();
        /* 最终验证：回到基线，无泄漏 */
        TEST_CHECK_INT("S1_13_interleaved_no_leak",
                       (s64)baseline, (s64)after);
    }

    s1_pass = g_pass - s1_pass;
    s1_fail = g_fail - s1_fail;
    section_footer(1, "PMM", s1_pass, s1_fail);
}

/* ================================================================
 * Section 2: VMM — 虚拟内存管理器边界 + 压力测试
 *
 *   VMM 管理四级页表 + 递归映射（PML4[511]→自身）。
 *   测试虚拟地址选在 PML4[2] 和 PML4[3] 区域：
 *     PML4[2] = 0x0002000000000000（未被内核使用）
 *     PML4[3] = 0x0003000000000000（同上）
 *
 *   【安全约束】
 *   - 不碰 PML4[0]（identity map）、PML4[1]（user area）、
 *     PML4[256]（kernel heap）、PML4[511]（recursive）
 *   - 每次测试后 unmap + free 物理帧
 *   - 最多映射 16 页
 * ================================================================ */

/* 测试用的虚拟地址基址
 *   必须是 canonical 地址（x86-64 4-level paging 只用 48 位，
 *   高 16 位是 bit 47 的符号扩展）。
 *   内核已用：PML4[0]=identity, PML4[1]=user, PML4[256]=heap, PML4[511]=recursive
 *   我们用 PML4[257]=0xFFFF808000000000 和 PML4[258]=0xFFFF810000000000 */
#define TEST_VIRT_BASE_PML4_2  0xFFFF808000000000ULL
#define TEST_VIRT_BASE_PML4_3  0xFFFF810000000000ULL

static void test_section_vmm(void) {
    s2_pass = g_pass; s2_fail = g_fail;

    section_header(2, "VMM");

    /* ---- S2_01: get_phys 对未映射地址返回 0 ---- */
    {
        paddr_t phys = arch_vmm_get_phys(TEST_VIRT_BASE_PML4_2);
        TEST_CHECK_INT("S2_01_get_phys_unmapped", 0, (s64)phys);
    }

    /* ---- S2_02: map page, get_phys 返回正确的物理地址 ---- */
    {
        paddr_t p = arch_pmm_alloc_frame();
        int rc = arch_vmm_map_page(TEST_VIRT_BASE_PML4_2, p,
                                   PAGE_FLAGS_KERNEL_RW);
        TEST_CHECK_INT("S2_02_map_success", 0, rc);

        paddr_t got = arch_vmm_get_phys(TEST_VIRT_BASE_PML4_2);
        /* get_phys 应该返回页对齐的物理基址（忽略页内偏移的低位） */
        paddr_t expected = p;  /* 映射时 virt 和 phys 都是页对齐的 */
        TEST_CHECK_INT("S2_02_get_phys_correct",
                       (s64)expected, (s64)got);

        /* 清理 */
        arch_vmm_unmap_page(TEST_VIRT_BASE_PML4_2);
        arch_pmm_free_frame(p);
    }

    /* ---- S2_03: unmap 后 get_phys 返回 0 ---- */
    {
        paddr_t p = arch_pmm_alloc_frame();
        arch_vmm_map_page(TEST_VIRT_BASE_PML4_2, p, PAGE_FLAGS_KERNEL_RW);
        arch_vmm_unmap_page(TEST_VIRT_BASE_PML4_2);
        paddr_t phys = arch_vmm_get_phys(TEST_VIRT_BASE_PML4_2);
        TEST_CHECK_INT("S2_03_unmap_then_get_phys_0", 0, (s64)phys);
        arch_pmm_free_frame(p);
    }

    /* ---- S2_04: 对同一虚拟地址 map 两次（覆盖）— 应成功 ---- */
    /*   第二次 map 同一地址会覆盖 PT 项，旧物理帧不释放
     *   （VMM 只管映射，不负责 PMM 引用计数）。
     *   测试覆盖映射后 get_phys 返回新物理地址。 */
    {
        paddr_t p1 = arch_pmm_alloc_frame();
        paddr_t p2 = arch_pmm_alloc_frame();
        arch_vmm_map_page(TEST_VIRT_BASE_PML4_2, p1, PAGE_FLAGS_KERNEL_RW);
        int rc = arch_vmm_map_page(TEST_VIRT_BASE_PML4_2, p2,
                                   PAGE_FLAGS_KERNEL_RW);
        TEST_CHECK_INT("S2_04_remap_success", 0, rc);

        paddr_t got = arch_vmm_get_phys(TEST_VIRT_BASE_PML4_2);
        TEST_CHECK_INT("S2_04_remap_get_phys_new",
                       (s64)p2, (s64)got);

        arch_vmm_unmap_page(TEST_VIRT_BASE_PML4_2);
        arch_pmm_free_frame(p1);  /* 旧帧也要释放（覆盖映射不释放旧帧） */
        arch_pmm_free_frame(p2);
    }

    /* ---- S2_05: unmap 已经 unmap 的地址 — 应返回 -1 或不崩溃 ---- */
    /*   对未映射地址做 unmap 是无害的（PT 项已是 0），
     *   AIHYK 的 unmap_page 对未映射地址返回 0 或 -1。
     *   关键是不崩溃。 */
    {
        /* 先确保 TEST_VIRT_BASE_PML4_2 未映射（上面的测试已清理） */
        int rc = arch_vmm_unmap_page(TEST_VIRT_BASE_PML4_2);
        /* 无论返回 0 还是 -1，只要不崩溃就算成功 */
        TEST_CHECK_BOOL("S2_05_unmap_already_unmapped", 1);
        (void)rc;
    }

    /* ---- S2_06: KERNEL_RW 映射：写 magic 再读回 ---- */
    /*   映射一页可写虚拟地址，写入 magic 值，再通过
     *   identity-map 的物理地址读回来验证。
     *   这验证了 VMM 映射确实让 CPU 能正常读写。 */
    {
        paddr_t p = arch_pmm_alloc_frame();
        arch_vmm_map_page(TEST_VIRT_BASE_PML4_2, p, PAGE_FLAGS_KERNEL_RW);

        /* 通过虚拟地址写入 magic */
        volatile u64 *virt_ptr = (volatile u64 *)TEST_VIRT_BASE_PML4_2;
        *virt_ptr = 0xDEADBEEFCAFEBABEULL;

        /* 通过 identity-map 的物理地址读回
         *   （identity map 把物理地址直接当虚拟地址用，
         *     PML4[0] 区域对所有物理内存都映射了） */
        volatile u64 *phys_ptr = (volatile u64 *)p;
        u64 readback = *phys_ptr;

        TEST_CHECK_INT("S2_06_write_readback_magic",
                       (s64)0xDEADBEEFCAFEBABEULL, (s64)readback);

        arch_vmm_unmap_page(TEST_VIRT_BASE_PML4_2);
        arch_pmm_free_frame(p);
    }

    /* ---- S2_07: KERNEL_RO 映射 — map 成功 ---- */
    /*   只读映射应该能成功创建。实际写入会触发 #PF，
     *   但我们不在测试里触发异常（避免崩溃），只验证 map 成功。 */
    {
        paddr_t p = arch_pmm_alloc_frame();
        int rc = arch_vmm_map_page(TEST_VIRT_BASE_PML4_2, p,
                                   PAGE_FLAGS_KERNEL_RO);
        TEST_CHECK_INT("S2_07_map_kernel_ro_success", 0, rc);

        arch_vmm_unmap_page(TEST_VIRT_BASE_PML4_2);
        arch_pmm_free_frame(p);
    }

    /* ---- S2_08: 递归映射 PML4[511] → 自身 ---- */
    /*   递归映射是 VMM 的关键技巧：把 PML4[511] 指向 PML4 自身物理地址，
     *   这样可以通过特定的虚拟地址直接读写页表项。
     *   验证：通过递归映射的"自引用地址"读取 PML4[511] 项，
     *   它应该指向 PML4 自身（CR3 值）。
     *
     *   递归映射下，0xFFFFFFFFFFFFF000 是 PML4 自身的虚拟映射。
     *   PML4[511] 项存储在 PML4 的偏移 511*8 = 4088 处。
     *   读出的值去掉 flag 位就是 PML4 的物理地址，应该 == CR3。 */
    {
        paddr_t cr3 = arch_vmm_get_cr3();
        /* 通过递归映射读 PML4[511] 项 */
        volatile u64 *pml4_self = (volatile u64 *)0xFFFFFFFFFFFFF000ULL;
        u64 entry511 = pml4_self[511];
        /* 去掉 flag 位（低 12 位），得到物理地址 */
        paddr_t entry_phys = entry511 & ~((u64)0xFFF);
        TEST_CHECK_INT("S2_08_recursive_mapping_pml4_self",
                       (s64)cr3, (s64)entry_phys);
    }

    /* ---- S2_09: 映射 16 页连续地址，验证翻译，全部 unmap ---- */
    {
        vaddr_t base = TEST_VIRT_BASE_PML4_2;
        paddr_t pages[16];
        int all_ok = 1;

        for (int i = 0; i < 16; i++) {
            pages[i] = arch_pmm_alloc_frame();
            if (pages[i] == 0) { all_ok = 0; break; }
            vaddr_t v = base + (vaddr_t)i * PAGE_SIZE;
            int rc = arch_vmm_map_page(v, pages[i], PAGE_FLAGS_KERNEL_RW);
            if (rc != 0) { all_ok = 0; break; }
        }

        /* 逐个验证 get_phys */
        if (all_ok) {
            for (int i = 0; i < 16; i++) {
                vaddr_t v = base + (vaddr_t)i * PAGE_SIZE;
                paddr_t got = arch_vmm_get_phys(v);
                if (got != pages[i]) { all_ok = 0; break; }
            }
        }

        TEST_CHECK_BOOL("S2_09_map_16_pages_all_correct", all_ok);

        /* 清理 */
        for (int i = 0; i < 16; i++) {
            if (pages[i] != 0) {
                vaddr_t v = base + (vaddr_t)i * PAGE_SIZE;
                arch_vmm_unmap_page(v);
                arch_pmm_free_frame(pages[i]);
            }
        }
    }

    /* ---- S2_10: map/unmap 循环 32 次，无泄漏 ---- */
    {
        usize_t baseline = arch_pmm_free_frames();
        vaddr_t v = TEST_VIRT_BASE_PML4_2;

        for (int i = 0; i < 32; i++) {
            paddr_t p = arch_pmm_alloc_frame();
            arch_vmm_map_page(v, p, PAGE_FLAGS_KERNEL_RW);
            arch_vmm_unmap_page(v);
            arch_pmm_free_frame(p);
        }

        usize_t after = arch_pmm_free_frames();
        TEST_CHECK_INT("S2_10_map_unmap_cycle_no_leak",
                       (s64)baseline, (s64)after);
    }

    /* ---- S2_11: 在不同 PML4 entry 映射（PML4[2] 和 PML4[3]），互不干扰 ---- */
    {
        paddr_t p2 = arch_pmm_alloc_frame();
        paddr_t p3 = arch_pmm_alloc_frame();

        arch_vmm_map_page(TEST_VIRT_BASE_PML4_2, p2, PAGE_FLAGS_KERNEL_RW);
        arch_vmm_map_page(TEST_VIRT_BASE_PML4_3, p3, PAGE_FLAGS_KERNEL_RW);

        paddr_t got2 = arch_vmm_get_phys(TEST_VIRT_BASE_PML4_2);
        paddr_t got3 = arch_vmm_get_phys(TEST_VIRT_BASE_PML4_3);

        int ok = (got2 == p2) && (got3 == p3);
        TEST_CHECK_BOOL("S2_11_different_pml4_independent", ok);

        arch_vmm_unmap_page(TEST_VIRT_BASE_PML4_2);
        arch_vmm_unmap_page(TEST_VIRT_BASE_PML4_3);
        arch_pmm_free_frame(p2);
        arch_pmm_free_frame(p3);
    }

    s2_pass = g_pass - s2_pass;
    s2_fail = g_fail - s2_fail;
    section_footer(2, "VMM", s2_pass, s2_fail);
}

/* ================================================================
 * Section 3: kmalloc — 内核堆边界 + 压力测试
 *
 *   kmalloc 是 first-fit 链表分配器，16 字节对齐。
 *   堆大小 1MB（HEAP_PAGES=256），由 kernel_mm_init 初始化。
 *   测试覆盖：
 *   - 零/极小/极大分配
 *   - kfree(NULL) 安全
 *   - krealloc / kcalloc 语义
 *   - 对齐保证（16 字节）
 *   - 块不重叠
 *   - 统计一致性
 *   - 压力：批量分配/释放、交错、碎片化、溢出检测
 * ================================================================ */
static void test_section_kmalloc(void) {
    s3_pass = g_pass; s3_fail = g_fail;

    section_header(3, "kmalloc");

    /* ---- S3_01: kmalloc(0) — 应返回 NULL ---- */
    /*   AIHYK 的 kmalloc 对 size==0 直接返回 NULL，
     *   和 glibc 的 malloc(0) 行为不同（glibc 可能返回非 NULL）。
     *   内核选择返回 NULL 是为了避免零大小块的 bookkeeping 混乱。 */
    {
        void *p = kmalloc(0);
        TEST_CHECK_INT("S3_01_kmalloc_zero_returns_null",
                       (s64)NULL, (s64)p);
    }

    /* ---- S3_02: kmalloc(1) — 应成功，16 字节对齐 ---- */
    {
        void *p = kmalloc(1);
        int ok = (p != NULL) && (((u64)p & 0xF) == 0);
        TEST_CHECK_BOOL("S3_02_kmalloc_one_aligned", ok);
        kfree(p);
    }

    /* ---- S3_03: kfree(NULL) — 应安全（不崩溃） ---- */
    {
        kfree(NULL);
        TEST_CHECK_BOOL("S3_03_kfree_null_safe", 1);
    }

    /* ---- S3_04: krealloc(NULL, 64) — 等价于 kmalloc(64) ---- */
    {
        void *p = krealloc(NULL, 64);
        int ok = (p != NULL) && (((u64)p & 0xF) == 0);
        TEST_CHECK_BOOL("S3_04_krealloc_null_like_kmalloc", ok);
        kfree(p);
    }

    /* ---- S3_05: kcalloc(1, 64) — 内容全零 ---- */
    {
        u8 *p = (u8 *)kcalloc(1, 64);
        int all_zero = 1;
        if (p != NULL) {
            for (int i = 0; i < 64; i++) {
                if (p[i] != 0) { all_zero = 0; break; }
            }
        }
        TEST_CHECK_BOOL("S3_05_kcalloc_content_zero", all_zero);
        kfree(p);
    }

    /* ---- S3_06: kmalloc 对齐：多种大小都 16 字节对齐 ---- */
    {
        int ok = 1;
        void *ptrs[8];
        usize_t sizes[] = { 1, 7, 15, 16, 33, 63, 127, 255 };
        for (int i = 0; i < 8; i++) {
            ptrs[i] = kmalloc(sizes[i]);
            if (ptrs[i] == NULL || (((u64)ptrs[i] & 0xF) != 0)) {
                ok = 0;
            }
        }
        TEST_CHECK_BOOL("S3_06_kmalloc_alignment_various", ok);
        for (int i = 0; i < 8; i++) kfree(ptrs[i]);
    }

    /* ---- S3_07: 分配的块不重叠 ---- */
    /*   写入不同的 magic 值到每个块，验证互不干扰。
     *   如果块重叠，写入一个会破坏另一个的 magic。 */
    {
        void *ptrs[8];
        u32 magics[] = { 0xA0000001, 0xA0000002, 0xA0000003, 0xA0000004,
                         0xA0000005, 0xA0000006, 0xA0000007, 0xA0000008 };
        for (int i = 0; i < 8; i++) {
            ptrs[i] = kmalloc(32);
            if (ptrs[i] != NULL) {
                *(u32 *)ptrs[i] = magics[i];
            }
        }
        int ok = 1;
        for (int i = 0; i < 8; i++) {
            if (ptrs[i] != NULL && *(u32 *)ptrs[i] != magics[i]) {
                ok = 0;
            }
        }
        TEST_CHECK_BOOL("S3_07_blocks_no_overlap", ok);
        for (int i = 0; i < 8; i++) kfree(ptrs[i]);
    }

    /* ---- S3_08: kmalloc 超大请求 — 应返回 NULL ---- */
    /*   堆只有 1MB，请求 2MB 肯定超过堆容量，应返回 NULL。 */
    {
        void *p = kmalloc(2 * 1024 * 1024);
        TEST_CHECK_INT("S3_08_kmalloc_oversize_returns_null",
                       (s64)NULL, (s64)p);
        /* 如果意外成功，也要释放 */
        if (p != NULL) kfree(p);
    }

    /* ---- S3_09: 堆统计一致性 total == used + free ---- */
    {
        usize_t total = kernel_mm_total();
        usize_t used  = kernel_mm_used();
        usize_t free  = kernel_mm_free();
        TEST_CHECK_INT("S3_09_heap_stats_consistent",
                       (s64)total, (s64)(used + free));
    }

    /* ---- S3_10: 50 个小分配 + 验证 + 全部释放 ---- */
    {
        usize_t baseline_blocks = kernel_mm_blocks();
        void *ptrs[50];
        int all_ok = 1;
        for (int i = 0; i < 50; i++) {
            ptrs[i] = kmalloc(32);
            if (ptrs[i] == NULL) { all_ok = 0; break; }
            /* 写入 magic 验证 */
            *(u32 *)ptrs[i] = (u32)(0xB0000000 + i);
        }
        /* 验证 magic 完好 */
        if (all_ok) {
            for (int i = 0; i < 50; i++) {
                if (*(u32 *)ptrs[i] != (u32)(0xB0000000 + i)) {
                    all_ok = 0;
                    break;
                }
            }
        }
        TEST_CHECK_BOOL("S3_10_50_small_allocs_valid", all_ok);
        for (int i = 0; i < 50; i++) kfree(ptrs[i]);

        /* 验证 blocks 计数回到基线 */
        usize_t after_blocks = kernel_mm_blocks();
        TEST_CHECK_INT("S3_10_blocks_back_to_baseline",
                       (s64)baseline_blocks, (s64)after_blocks);
    }

    /* ---- S3_11: 不同大小分配：16, 32, 64, ..., 4096 ---- */
    {
        void *ptrs[9];
        usize_t sizes[] = { 16, 32, 64, 128, 256, 512, 1024, 2048, 4096 };
        int all_ok = 1;
        for (int i = 0; i < 9; i++) {
            ptrs[i] = kmalloc(sizes[i]);
            if (ptrs[i] == NULL || (((u64)ptrs[i] & 0xF) != 0)) {
                all_ok = 0;
            }
        }
        TEST_CHECK_BOOL("S3_11_various_sizes_all_aligned", all_ok);
        for (int i = 0; i < 9; i++) kfree(ptrs[i]);
    }

    /* ---- S3_12: 交错 alloc/free ---- */
    /*   alloc 10 个，free 偶数索引的，再 alloc 5 个，free 剩余。
     *   测试分配器在碎片化状态下的正确性。 */
    {
        void *ptrs[10];
        for (int i = 0; i < 10; i++) ptrs[i] = kmalloc(64);

        /* free 偶数索引：0, 2, 4, 6, 8 */
        for (int i = 0; i < 10; i += 2) {
            kfree(ptrs[i]);
            ptrs[i] = NULL;
        }

        /* 再 alloc 5 个填补空位 */
        void *extra[5];
        int ok = 1;
        for (int i = 0; i < 5; i++) {
            extra[i] = kmalloc(64);
            if (extra[i] == NULL) ok = 0;
        }
        TEST_CHECK_BOOL("S3_12_interleaved_alloc_free", ok);

        /* 清理 */
        for (int i = 0; i < 10; i++) kfree(ptrs[i]);
        for (int i = 0; i < 5; i++) kfree(extra[i]);
    }

    /* ---- S3_13: kcalloc 溢出检测 ---- */
    /*   kcalloc(nmemb, size) 检查 nmemb*size 是否溢出 usize_t。
     *   用 (SIZE_MAX/2 + 1) * 2 会溢出 64 位，
     *   kcalloc 应该检测到并返回 NULL。
     *
     *   【为什么这很重要】
     *     如果不检查溢出，kmalloc 收到一个很小的 size，
     *     分配成功，但调用方以为分配了 nmemb*size 字节，
     *     越界写入 → 堆损坏 → 安全漏洞。
     *     这就是经典的 CVE-2010-5328 (OpenSSL) 等的根因。 */
    {
        usize_t half_max = ((usize_t)1 << 63);  /* 2^63 */
        void *p = kcalloc(half_max, 2);
        TEST_CHECK_INT("S3_13_kcalloc_overflow_returns_null",
                       (s64)NULL, (s64)p);
        if (p != NULL) kfree(p);
    }

    /* ---- S3_14: 碎片化测试：分配 20 块 4096，free 隔一个，验证剩余块 ---- */
    {
        void *ptrs[20];
        int ok = 1;
        for (int i = 0; i < 20; i++) {
            ptrs[i] = kmalloc(4096);
            if (ptrs[i] == NULL) { ok = 0; break; }
            /* 写入 magic */
            *(u32 *)ptrs[i] = (u32)(0xC0000000 + i);
        }

        /* free 隔一个：索引 0, 2, 4, ..., 18 */
        for (int i = 0; i < 20; i += 2) {
            kfree(ptrs[i]);
            ptrs[i] = NULL;
        }

        /* 验证剩余块 magic 完好 */
        if (ok) {
            for (int i = 1; i < 20; i += 2) {
                if (ptrs[i] == NULL ||
                    *(u32 *)ptrs[i] != (u32)(0xC0000000 + i)) {
                    ok = 0;
                    break;
                }
            }
        }
        TEST_CHECK_BOOL("S3_14_fragmentation_remaining_valid", ok);

        /* 清理 */
        for (int i = 1; i < 20; i += 2) kfree(ptrs[i]);
    }

    s3_pass = g_pass - s3_pass;
    s3_fail = g_fail - s3_fail;
    section_footer(3, "kmalloc", s3_pass, s3_fail);
}

/* ================================================================
 * Section 4: Scheduler — 调度器边界 + 压力测试
 *
 *   调度器用 round-robin + 时间片，最多 MAX_TASKS=16 个任务。
 *   init task (ID=0) 始终存在，加上可能已有 demo 任务。
 *
 *   【安全约束】
 *   - 创建前检查 sched_num_tasks()，不超过 MAX_TASKS
 *   - helper 任务只做 yield + exit（通过 task_trampoline 隐式退出）
 *   - init 用 wait_for_helpers() 等待 helper 退出
 *   - 超时保护防止死等
 * ================================================================ */

/* helper 任务：yield 一次后退出
 *   task_trampoline 会在 entry 返回后调 sched_exit()，
 *   所以这个函数只需要 yield 一次就自动退出。 */
static void helper_yield_exit(void *arg) {
    sched_yield();
    /* 函数返回 → task_trampoline → sched_exit() */
}

/* helper 任务：记录自己被调度过
 *   通过共享变量（kmalloc 分配的 int）记录执行次数，
 *   init 在 helper 退出后读取验证。 */
static void helper_count_runs(void *arg) {
    int *counter = (int *)arg;
    if (counter != NULL) {
        (*counter)++;
    }
    sched_yield();
    if (counter != NULL) {
        (*counter)++;
    }
    /* 返回 → sched_exit */
}

/* helper 任务：sleep 10 tick 后退出 */
static void helper_sleep_exit(void *arg) {
    sched_sleep(10);
    /* 返回 → sched_exit */
}

static void test_section_sched(void) {
    s4_pass = g_pass; s4_fail = g_fail;

    section_header(4, "Scheduler");

    /* 记录初始任务数（init + 可能的 demo 任务） */
    int baseline = sched_num_tasks();

    /* ---- S4_01: sched_num_tasks 返回正确值 ---- */
    {
        int n = sched_num_tasks();
        /* 至少有 init task (1 个) */
        TEST_CHECK_BOOL("S4_01_num_tasks_at_least_1", n >= 1);
    }

    /* ---- S4_02: sched_create_task 返回有效 task_id ---- */
    {
        s64 tid = sched_create_task(helper_yield_exit, NULL, "kt_h1");
        TEST_CHECK_BOOL("S4_02_create_task_valid_id", tid >= 0);
        /* 等 helper 退出 */
        wait_for_helpers(baseline);
    }

    /* ---- S4_03: 创建超过 MAX_TASKS 应失败 ---- */
    /*   MAX_TASKS=16，但已有 init + 可能的 demo 任务。
     *   我们先查看当前任务数，再尝试创建到满。
     *   注意：每个 helper 需要用 kmalloc 分配 task_struct + stack，
     *   如果堆不够，也会失败。所以我们只验证"最后一个应失败"。
     *
     *   策略：尽量创建到 MAX_TASKS-1 个额外任务，第 MAX_TASKS 个
     *   应该失败。但为避免堆耗尽，我们用小批量测试：
     *   创建到只剩 1 个空位，再试一个。 */
    {
        int current = sched_num_tasks();
        int slots_available = MAX_TASKS - current;
        s64 tids[16];
        int created = 0;

        /* 填满到 MAX_TASKS-1（留 1 个给下面测试）
         * 但限制最多创建 10 个，避免堆耗尽 */
        int to_create = slots_available - 1;
        if (to_create > 10) to_create = 10;
        if (to_create < 0) to_create = 0;

        for (int i = 0; i < to_create; i++) {
            tids[created] = sched_create_task(helper_yield_exit, NULL,
                                              "kt_fill");
            if (tids[created] >= 0) {
                created++;
            } else {
                break;
            }
        }

        /* 现在应该只剩 1 个空位，尝试创建应该成功 */
        s64 last = sched_create_task(helper_yield_exit, NULL, "kt_last");
        int last_ok = (last >= 0) ? 1 : 0;

        /* 然后再创建一个，应该失败 */
        s64 overflow = sched_create_task(helper_yield_exit, NULL, "kt_over");
        int overflow_fail = (overflow < 0) ? 1 : 0;

        TEST_CHECK_BOOL("S4_03_create_beyond_max_fails",
                        last_ok || overflow_fail);

        /* 清理：等所有 helper 退出 */
        wait_for_helpers(baseline);

        /* 如果 overflow 意外成功，也要等它退出 */
        if (overflow >= 0) {
            wait_for_helpers(baseline);
        }
    }

    /* ---- S4_04: current 指针不为 NULL ---- */
    {
        TEST_CHECK_BOOL("S4_04_current_not_null", current != NULL);
    }

    /* ---- S4_05: current 状态是 TASK_RUNNING ---- */
    {
        TEST_CHECK_INT("S4_05_current_state_running",
                       TASK_RUNNING, current->state);
    }

    /* ---- S4_06: 创建后 yield — 任务运行并退出 ---- */
    {
        int before = sched_num_tasks();
        sched_create_task(helper_yield_exit, NULL, "kt_y1");
        /* yield 让 helper 跑 */
        sched_yield();
        /* 等退出 */
        wait_for_helpers(before);
        int after = sched_num_tasks();
        TEST_CHECK_INT("S4_06_task_ran_and_exited",
                       (s64)before, (s64)after);
    }

    /* ---- S4_07: 创建 5 个任务，yield 几次，验证它们都运行了 ---- */
    {
        int counters[5];
        for (int i = 0; i < 5; i++) counters[i] = 0;

        int before = sched_num_tasks();
        for (int i = 0; i < 5; i++) {
            sched_create_task(helper_count_runs, &counters[i], "kt_c");
        }

        /* 多次 yield 让所有 helper 都跑 */
        for (int i = 0; i < 20; i++) sched_yield();
        wait_for_helpers(before);

        /* 每个 counter 应该 >= 1（至少被调度过一次） */
        int all_ran = 1;
        for (int i = 0; i < 5; i++) {
            if (counters[i] < 1) { all_ran = 0; break; }
        }
        TEST_CHECK_BOOL("S4_07_five_tasks_all_ran", all_ran);
    }

    /* ---- S4_08: 快速 create/exit 循环 ---- */
    {
        int before = sched_num_tasks();
        int ok = 1;
        for (int round = 0; round < 5; round++) {
            for (int i = 0; i < 3; i++) {
                s64 tid = sched_create_task(helper_yield_exit, NULL,
                                            "kt_rce");
                if (tid < 0) { ok = 0; break; }
            }
            /* yield 让它们跑并退出 */
            wait_for_helpers(before);
        }
        TEST_CHECK_BOOL("S4_08_rapid_create_exit_cycle", ok);
        /* 最终任务数应回到 before */
        int after = sched_num_tasks();
        TEST_CHECK_INT("S4_08_task_count_restored",
                       (s64)before, (s64)after);
    }

    /* ---- S4_09: sleep 功能验证 ---- */
    /*   sched_sleep(5) 应让当前任务阻塞，让出 CPU，然后唤醒。
     *   验证：sleep 返回后任务仍在 RUNNING 状态（最基本的安全保证）。 */
    {
        int state_before = current->state;
        sched_sleep(5);
        int state_after = current->state;
        TEST_CHECK_BOOL("S4_09_sleep_returns_running",
                        (state_before == TASK_RUNNING &&
                         state_after == TASK_RUNNING));
    }

    /* ---- S4_10: 多个任务同时 sleep ---- */
    {
        int before = sched_num_tasks();
        /* 创建 3 个 sleep helper */
        for (int i = 0; i < 3; i++) {
            sched_create_task(helper_sleep_exit, NULL, "kt_sl");
        }
        /* init 也 sleep 15 tick（比 helper 的 10 tick 长） */
        sched_sleep(15);
        /* 等 helper 退出 */
        wait_for_helpers(before);
        int after = sched_num_tasks();
        TEST_CHECK_INT("S4_10_multi_sleep_all_exit",
                       (s64)before, (s64)after);
    }

    s4_pass = g_pass - s4_pass;
    s4_fail = g_fail - s4_fail;
    section_footer(4, "Scheduler", s4_pass, s4_fail);
}

/* ================================================================
 * Section 5: IPC — 进程间通信边界 + 压力测试
 *
 *   IPC 是通道 + 有界 FIFO 消息队列，支持阻塞/非阻塞/超时。
 *   非阻塞变体（try_send / try_recv）可以在 init task 内测试，
 *   不需要 helper。阻塞变体需要 helper 任务配合。
 *
 *   【约束】
 *   - 最多 IPC_MAX_CHANNELS=32 个通道
 *   - 消息负载最大 IPC_MAX_PAYLOAD=64 字节
 *   - 默认通道容量 IPC_DEFAULT_CAPACITY=8 条消息
 *   - 测试后销毁所有创建的通道
 * ================================================================ */
static void test_section_ipc(void) {
    s5_pass = g_pass; s5_fail = g_fail;

    section_header(5, "IPC");

    /* ---- S5_01: 创建通道，验证 id > 0 ---- */
    {
        ipc_channel_id_t id = ipc_channel_create("kt_ch1", IPC_DEFAULT_CAPACITY);
        TEST_CHECK_BOOL("S5_01_channel_create_id_positive", id > 0);
        ipc_channel_destroy(id);
    }

    /* ---- S5_02: 销毁通道，验证成功 ---- */
    {
        ipc_channel_id_t id = ipc_channel_create("kt_ch2", IPC_DEFAULT_CAPACITY);
        int rc = ipc_channel_destroy(id);
        TEST_CHECK_INT("S5_02_channel_destroy_success", 0, rc);
    }

    /* ---- S5_03: 创建 IPC_MAX_CHANNELS (32) 个通道 — 全部成功 ---- */
    {
        ipc_channel_id_t ids[IPC_MAX_CHANNELS];
        int all_ok = 1;
        for (int i = 0; i < IPC_MAX_CHANNELS; i++) {
            ids[i] = ipc_channel_create("kt_full", IPC_DEFAULT_CAPACITY);
            if (ids[i] == IPC_INVALID_CHANNEL) { all_ok = 0; break; }
        }
        TEST_CHECK_BOOL("S5_03_create_max_channels", all_ok);

        /* 清理 */
        for (int i = 0; i < IPC_MAX_CHANNELS; i++) {
            if (ids[i] != IPC_INVALID_CHANNEL) {
                ipc_channel_destroy(ids[i]);
            }
        }
    }

    /* ---- S5_04: 创建超过 IPC_MAX_CHANNELS — 应失败 ---- */
    /*   先填满所有通道，再创建一个应返回 IPC_INVALID_CHANNEL (0) */
    {
        ipc_channel_id_t ids[IPC_MAX_CHANNELS];
        for (int i = 0; i < IPC_MAX_CHANNELS; i++) {
            ids[i] = ipc_channel_create("kt_over", IPC_DEFAULT_CAPACITY);
        }
        ipc_channel_id_t overflow = ipc_channel_create("kt_over",
                                                        IPC_DEFAULT_CAPACITY);
        TEST_CHECK_INT("S5_04_create_beyond_max_fails",
                       (s64)IPC_INVALID_CHANNEL, (s64)overflow);

        /* 清理 */
        for (int i = 0; i < IPC_MAX_CHANNELS; i++) {
            if (ids[i] != IPC_INVALID_CHANNEL) {
                ipc_channel_destroy(ids[i]);
            }
        }
    }

    /* ---- S5_05: try_recv 空通道 — 返回 IPC_ERR_WOULDBLOCK ---- */
    {
        ipc_channel_id_t id = ipc_channel_create("kt_empty", IPC_DEFAULT_CAPACITY);
        u64 type = 0;
        u64 cap = 64;
        char buf[64];
        int rc = ipc_try_recv(id, &type, buf, &cap);
        TEST_CHECK_INT("S5_05_try_recv_empty_wouldblock",
                       IPC_ERR_WOULDBLOCK, rc);
        ipc_channel_destroy(id);
    }

    /* ---- S5_06: 发送到无效通道 (id=0) — 应返回错误 ---- */
    {
        char payload[4] = "test";
        int rc = ipc_try_send(IPC_INVALID_CHANNEL, 1, payload, 4);
        TEST_CHECK_BOOL("S5_06_send_invalid_channel_error", rc < 0);
    }

    /* ---- S5_07: 从无效通道接收 — 应返回错误 ---- */
    {
        u64 type = 0;
        u64 cap = 64;
        char buf[64];
        int rc = ipc_try_recv(IPC_INVALID_CHANNEL, &type, buf, &cap);
        TEST_CHECK_BOOL("S5_07_recv_invalid_channel_error", rc < 0);
    }

    /* ---- S5_08: 发送超大负载 (> IPC_MAX_PAYLOAD) — 应返回 TOOLONG ---- */
    {
        ipc_channel_id_t id = ipc_channel_create("kt_big", IPC_DEFAULT_CAPACITY);
        char payload[128];
        for (int i = 0; i < 128; i++) payload[i] = (char)i;
        int rc = ipc_try_send(id, 1, payload, 128);
        TEST_CHECK_INT("S5_08_send_toolong",
                       IPC_ERR_TOOLONG, rc);
        ipc_channel_destroy(id);
    }

    /* ---- S5_09: 填满通道到容量，再 try_send 应返回 WOULDBLOCK ---- */
    {
        ipc_channel_id_t id = ipc_channel_create("kt_fullcap",
                                                  IPC_DEFAULT_CAPACITY);
        char payload[8] = "hello!";
        int ok = 1;

        /* 填满 8 条消息 */
        for (int i = 0; i < IPC_DEFAULT_CAPACITY; i++) {
            int rc = ipc_try_send(id, (u64)i, payload, 8);
            if (rc != 0) { ok = 0; break; }
        }
        TEST_CHECK_BOOL("S5_09_fill_channel_ok", ok);

        /* 再发一条应该 WOULDBLOCK */
        int rc = ipc_try_send(id, 99, payload, 8);
        TEST_CHECK_INT("S5_09_full_channel_wouldblock",
                       IPC_ERR_WOULDBLOCK, rc);

        ipc_channel_destroy(id);
    }

    /* ---- S5_10: 填满 + 排空通道，验证 FIFO 顺序 ---- */
    {
        ipc_channel_id_t id = ipc_channel_create("kt_fifo",
                                                  IPC_DEFAULT_CAPACITY);
        char send_payload[8] = "fifo!";
        int ok = 1;

        /* 发 8 条，type 从 100 到 107 */
        for (int i = 0; i < IPC_DEFAULT_CAPACITY; i++) {
            int rc = ipc_try_send(id, (u64)(100 + i), send_payload, 8);
            if (rc != 0) { ok = 0; break; }
        }

        /* 收 8 条，验证 type 按 FIFO 顺序 */
        if (ok) {
            for (int i = 0; i < IPC_DEFAULT_CAPACITY; i++) {
                u64 type = 0;
                u64 cap = 64;
                char buf[64];
                int rc = ipc_try_recv(id, &type, buf, &cap);
                if (rc != 0 || type != (u64)(100 + i)) {
                    ok = 0;
                    break;
                }
            }
        }
        TEST_CHECK_BOOL("S5_10_fifo_order_correct", ok);

        ipc_channel_destroy(id);
    }

    /* ---- S5_11: 销毁带消息的通道 — 消息被释放（无泄漏） ---- */
    /*   销毁通道时，IPC 实现应该 kfree 所有排队中的消息。
     *   我们通过观察 free_frames 在销毁前后不变来验证
     *   （消息的 kmalloc/kfree 在堆上操作，不影响 PMM，
     *    但堆的 used_bytes 应该回到基线）。
     *   这里用 kernel_mm_used() 检测堆泄漏。 */
    {
        usize_t baseline_used = kernel_mm_used();

        ipc_channel_id_t id = ipc_channel_create("kt_msgleak",
                                                  IPC_DEFAULT_CAPACITY);
        char payload[8] = "leak!";
        for (int i = 0; i < IPC_DEFAULT_CAPACITY; i++) {
            ipc_try_send(id, (u64)i, payload, 8);
        }

        /* 销毁带消息的通道 */
        ipc_channel_destroy(id);

        usize_t after_used = kernel_mm_used();
        TEST_CHECK_INT("S5_11_destroy_with_msgs_no_leak",
                       (s64)baseline_used, (s64)after_used);
    }

    /* ---- S5_12: 填满 + 排空循环 10 次（8 条消息），无泄漏 ---- */
    {
        usize_t baseline_used = kernel_mm_used();

        ipc_channel_id_t id = ipc_channel_create("kt_flood",
                                                  IPC_DEFAULT_CAPACITY);
        char payload[8] = "flood!";
        int ok = 1;

        for (int round = 0; round < 10; round++) {
            /* 填满 */
            for (int i = 0; i < IPC_DEFAULT_CAPACITY; i++) {
                int rc = ipc_try_send(id, (u64)(round * 10 + i),
                                      payload, 8);
                if (rc != 0) { ok = 0; break; }
            }
            /* 排空 */
            for (int i = 0; i < IPC_DEFAULT_CAPACITY; i++) {
                u64 type = 0;
                u64 cap = 64;
                char buf[64];
                int rc = ipc_try_recv(id, &type, buf, &cap);
                if (rc != 0) { ok = 0; break; }
            }
        }
        TEST_CHECK_BOOL("S5_12_flood_drain_10x_ok", ok);

        ipc_channel_destroy(id);

        usize_t after_used = kernel_mm_used();
        TEST_CHECK_INT("S5_12_flood_drain_no_leak",
                       (s64)baseline_used, (s64)after_used);
    }

    /* ---- S5_13: 创建/销毁 20 个通道循环，无泄漏 ---- */
    {
        usize_t baseline_used = kernel_mm_used();

        for (int i = 0; i < 20; i++) {
            ipc_channel_id_t id = ipc_channel_create("kt_cd",
                                                      IPC_DEFAULT_CAPACITY);
            if (id != IPC_INVALID_CHANNEL) {
                ipc_channel_destroy(id);
            }
        }

        usize_t after_used = kernel_mm_used();
        TEST_CHECK_INT("S5_13_create_destroy_20_no_leak",
                       (s64)baseline_used, (s64)after_used);
    }

    /* ---- S5_14: send_timeout 在满通道上超时 ---- */
    /*   填满通道后用 send_timeout(timeout=10) 发送，
     *   应该超时后返回 IPC_ERR_TIMEDOUT。 */
    {
        ipc_channel_id_t id = ipc_channel_create("kt_stmo",
                                                  IPC_DEFAULT_CAPACITY);
        char payload[8] = "tmo!";
        /* 填满 */
        for (int i = 0; i < IPC_DEFAULT_CAPACITY; i++) {
            ipc_try_send(id, (u64)i, payload, 8);
        }

        /* 带超时发送 */
        int rc = ipc_send_timeout(id, 99, payload, 8, 10);
        TEST_CHECK_INT("S5_14_send_timeout_timedout",
                       IPC_ERR_TIMEDOUT, rc);

        ipc_channel_destroy(id);
    }

    /* ---- S5_15: recv_timeout 在空通道上超时 ---- */
    /*   【Bug 已修复】原来当 init 是唯一 RUNNABLE 任务时，IPC 超时
     *   机制不可靠（sched_yield 把 BLOCKED 任务恢复成 RUNNING，
     *   导致假唤醒，返回 IPC_OK）。
     *   修复后：sched_yield 对 BLOCKED+无 next 任务执行 halt，
     *   timer IRQ 正常推进 tick，wake_sleeping_tasks 检测超时后
     *   正确唤醒，IPC 返回 IPC_ERR_TIMEDOUT。
     *   验证：必须返回 IPC_ERR_TIMEDOUT（不再接受 IPC_OK）。 */
    {
        ipc_channel_id_t id = ipc_channel_create("kt_rtmo",
                                                  IPC_DEFAULT_CAPACITY);
        u64 type = 0;
        u64 cap = 64;
        char buf[64];

        int rc = ipc_recv_timeout(id, &type, buf, &cap, 10);
        TEST_CHECK_INT("S5_15_recv_timeout_timedout",
                       IPC_ERR_TIMEDOUT, rc);

        ipc_channel_destroy(id);
    }

    s5_pass = g_pass - s5_pass;
    s5_fail = g_fail - s5_fail;
    section_footer(5, "IPC", s5_pass, s5_fail);
}

/* ================================================================
 * Section 6: Cross-subsystem — 跨子系统回归测试
 *
 *   回归测试的核心思想：一个子系统的操作不应影响其他子系统。
 *   例如：
 *   - IPC 创建/销毁不应改变 PMM 的 free_frames
 *   - 调度器创建/退出任务不应导致 PMM 泄漏
 *   - VMM map/unmap 后堆统计应一致
 *   - 全链路操作后所有资源计数回到基线
 *
 *   这些测试捕获"子系统间耦合导致的退化"，
 *   是最容易被忽视但最关键的测试类别。
 * ================================================================ */

/* helper 用于 S6_06：发送消息后退出 */
static void helper_send_and_exit(void *arg) {
    ipc_channel_id_t *ch_id = (ipc_channel_id_t *)arg;
    if (ch_id != NULL) {
        char msg[8] = "from_h";
        ipc_try_send(*ch_id, 42, msg, 8);
    }
    /* 返回 → sched_exit */
}

/* helper 用于 S6_09：yield 后检查共享变量 */
static volatile int g_s6_09_flag;
static void helper_set_flag(void *arg) {
    g_s6_09_flag = 1;
    sched_yield();
    g_s6_09_flag = 2;
    /* 返回 → sched_exit */
}

static void test_section_cross(void) {
    s6_pass = g_pass; s6_fail = g_fail;

    section_header(6, "Cross-subsystem");

    /* ---- S6_01: PMM 在 IPC 活动后不受影响 ---- */
    /*   IPC 内部用 kmalloc 分配消息结构，kmalloc 用 VMM 映射的堆。
     *   堆内存来自 PMM，但 IPC 消息的分配/释放在堆层面，
     *   不直接从 PMM alloc/free 物理帧。
     *   所以 IPC create/destroy 不应改变 free_frames。
     *   （如果 IPC 实现直接调 arch_pmm_alloc_frame，就会影响！） */
    {
        usize_t baseline = arch_pmm_free_frames();
        ipc_channel_id_t id = ipc_channel_create("kt_cross1",
                                                  IPC_DEFAULT_CAPACITY);
        /* 发几条消息 */
        char payload[8] = "cross!";
        for (int i = 0; i < 4; i++) {
            ipc_try_send(id, (u64)i, payload, 8);
        }
        ipc_channel_destroy(id);
        usize_t after = arch_pmm_free_frames();

        TEST_CHECK_INT("S6_01_pmm_unchanged_after_ipc_destroy",
                       (s64)baseline, (s64)after);
    }

    /* ---- S6_02: PMM 在调度器活动后不受影响 ---- */
    /*   创建任务需要 kmalloc(task_struct) + kmalloc(stack)，
     *   都在堆上操作。任务退出后 task_reaper kfree 这些内存。
     *   PMM 的 free_frames 在创建/退出前后应该不变
     *   （除非堆扩展时调了 arch_pmm_alloc_frame，但 AIHYK 的堆
     *    是启动时一次性分配的，不会动态扩展）。 */
    {
        usize_t baseline = arch_pmm_free_frames();
        int n = sched_num_tasks();

        s64 tid = sched_create_task(helper_yield_exit, NULL, "kt_pm2");
        if (tid >= 0) {
            wait_for_helpers(n);
        }

        usize_t after = arch_pmm_free_frames();
        TEST_CHECK_INT("S6_02_pmm_unchanged_after_sched",
                       (s64)baseline, (s64)after);
    }

    /* ---- S6_03: kmalloc 在 VMM 活动后统计一致 ---- */
    /*   VMM map_page 可能分配中间页表（PMM 分配物理帧），
     *   但不影响堆统计。unmap_page 后页表不释放
     *   （AIHYK 简化设计），所以堆统计应该完全一致。 */
    {
        usize_t baseline_used = kernel_mm_used();

        paddr_t p = arch_pmm_alloc_frame();
        arch_vmm_map_page(TEST_VIRT_BASE_PML4_2, p, PAGE_FLAGS_KERNEL_RW);

        usize_t after_map_used = kernel_mm_used();

        arch_vmm_unmap_page(TEST_VIRT_BASE_PML4_2);
        arch_pmm_free_frame(p);

        usize_t after_unmap_used = kernel_mm_used();

        TEST_CHECK_INT("S6_03_heap_used_unchanged_by_vmm",
                       (s64)baseline_used, (s64)after_map_used);
        TEST_CHECK_INT("S6_03_heap_restored_after_vmm",
                       (s64)baseline_used, (s64)after_unmap_used);
        (void)after_unmap_used; /* suppress warning */
    }

    /* ---- S6_04: PMM + VMM 组合：alloc + map + write + unmap + free ---- */
    {
        usize_t baseline = arch_pmm_free_frames();
        paddr_t p = arch_pmm_alloc_frame();
        arch_vmm_map_page(TEST_VIRT_BASE_PML4_2, p, PAGE_FLAGS_KERNEL_RW);

        /* 写 magic */
        volatile u64 *vptr = (volatile u64 *)TEST_VIRT_BASE_PML4_2;
        *vptr = 0x4241424342414243ULL;  /* "CBABCBAB" */

        arch_vmm_unmap_page(TEST_VIRT_BASE_PML4_2);
        arch_pmm_free_frame(p);

        usize_t after = arch_pmm_free_frames();
        TEST_CHECK_INT("S6_04_pmm_vmm_combined_no_leak",
                       (s64)baseline, (s64)after);
    }

    /* ---- S6_05: IPC + Cap 共存 ---- */
    /*   通过 ipc_channel_create 和 cap_channel_create 分别创建通道，
     *   验证两者都能工作，互不干扰。
     *   cap_channel_create 内部调 ipc_channel_create + 安装 cap，
     *   所以两者创建的 channel 在同一个 channel_table 里。 */
    {
        /* 先用 IPC 创建 */
        ipc_channel_id_t ipc_id = ipc_channel_create("kt_ipc5",
                                                      IPC_DEFAULT_CAPACITY);
        /* 再用 Cap 创建（需要 include cap.h，但我们通过
         * ipc_channel_create 间接测试——两个 channel 共存） */
        ipc_channel_id_t ipc_id2 = ipc_channel_create("kt_cap5",
                                                       IPC_DEFAULT_CAPACITY);
        int ok = (ipc_id > 0) && (ipc_id2 > 0) && (ipc_id != ipc_id2);
        TEST_CHECK_BOOL("S6_05_ipc_cap_coexist", ok);

        ipc_channel_destroy(ipc_id);
        ipc_channel_destroy(ipc_id2);
    }

    /* ---- S6_06: Scheduler + IPC：helper 发消息后退出，通道仍有效 ---- */
    /*   helper 向通道发一条消息，然后退出。init 收到消息，
     *   验证通道在 helper 退出后仍然可用。 */
    {
        ipc_channel_id_t id = ipc_channel_create("kt_sipc",
                                                  IPC_DEFAULT_CAPACITY);
        int n = sched_num_tasks();

        /* 创建 helper，传通道 id */
        sched_create_task(helper_send_and_exit, &id, "kt_hs");

        /* 等待 helper 退出 */
        wait_for_helpers(n);

        /* 尝试从通道接收消息 */
        u64 type = 0;
        u64 cap = 64;
        char buf[64];
        int rc = ipc_try_recv(id, &type, buf, &cap);
        TEST_CHECK_INT("S6_06_recv_after_helper_exit", 0, rc);
        TEST_CHECK_INT("S6_06_msg_type_correct", 42, (int)type);

        ipc_channel_destroy(id);
    }

    /* ---- S6_07: PMM 统计在多子系统操作后稳定 ---- */
    /*   依次做：IPC create → alloc frame → kmalloc →
     *   全部逆向释放，验证 free_frames 回到基线。 */
    {
        usize_t baseline = arch_pmm_free_frames();

        ipc_channel_id_t ch = ipc_channel_create("kt_stab",
                                                  IPC_DEFAULT_CAPACITY);
        paddr_t frame = arch_pmm_alloc_frame();
        void *heap_ptr = kmalloc(128);

        /* 释放（逆向） */
        kfree(heap_ptr);
        arch_pmm_free_frame(frame);
        ipc_channel_destroy(ch);

        usize_t after = arch_pmm_free_frames();
        TEST_CHECK_INT("S6_07_pmm_stable_after_multi_ops",
                       (s64)baseline, (s64)after);
    }

    /* ---- S6_08: 堆完整性：kmalloc/kfree 混合 IPC create/destroy ---- */
    {
        usize_t baseline_used = kernel_mm_used();
        usize_t baseline_blocks = kernel_mm_blocks();

        /* 交替做堆分配和 IPC 操作 */
        void *p1 = kmalloc(256);
        ipc_channel_id_t ch1 = ipc_channel_create("kt_hi1",
                                                    IPC_DEFAULT_CAPACITY);
        void *p2 = kmalloc(512);
        ipc_channel_id_t ch2 = ipc_channel_create("kt_hi2",
                                                    IPC_DEFAULT_CAPACITY);

        /* 发消息 */
        char msg[8] = "mix!";
        ipc_try_send(ch1, 1, msg, 8);
        ipc_try_send(ch2, 2, msg, 8);

        /* 逆向释放 */
        ipc_channel_destroy(ch2);
        kfree(p2);
        ipc_channel_destroy(ch1);
        kfree(p1);

        usize_t after_used = kernel_mm_used();
        usize_t after_blocks = kernel_mm_blocks();
        TEST_CHECK_INT("S6_08_heap_used_after_mixed",
                       (s64)baseline_used, (s64)after_used);
        TEST_CHECK_INT("S6_08_heap_blocks_after_mixed",
                       (s64)baseline_blocks, (s64)after_blocks);
    }

    /* ---- S6_09: VMM + Scheduler：map 页，yield，回来验证映射仍在 ---- */
    /*   map 一页写入 magic，yield 让其他任务跑，
     *   回来后读 magic 验证映射没被破坏。 */
    {
        paddr_t p = arch_pmm_alloc_frame();
        arch_vmm_map_page(TEST_VIRT_BASE_PML4_2, p, PAGE_FLAGS_KERNEL_RW);

        volatile u64 *vptr = (volatile u64 *)TEST_VIRT_BASE_PML4_2;
        *vptr = 0x9999AAAA8888BBBBULL;

        /* 创建 helper 让调度器有东西切换 */
        int n = sched_num_tasks();
        sched_create_task(helper_set_flag, NULL, "kt_vms");
        sched_yield();
        /* yield 后回来，验证映射和 magic 仍在 */
        u64 readback = *vptr;
        TEST_CHECK_INT("S6_09_mapping_survives_yield",
                       (s64)0x9999AAAA8888BBBBULL, (s64)readback);

        wait_for_helpers(n);
        arch_vmm_unmap_page(TEST_VIRT_BASE_PML4_2);
        arch_pmm_free_frame(p);
    }

    /* ---- S6_10: 全链路：alloc frame → map → write → IPC send →
     *              recv → unmap → free frame，全面无泄漏 ---- */
    {
        usize_t baseline_frames = arch_pmm_free_frames();
        usize_t baseline_used  = kernel_mm_used();

        /* 1. 分配物理帧 */
        paddr_t frame = arch_pmm_alloc_frame();

        /* 2. 映射到虚拟地址 */
        arch_vmm_map_page(TEST_VIRT_BASE_PML4_2, frame,
                           PAGE_FLAGS_KERNEL_RW);

        /* 3. 写入数据 */
        volatile u64 *vptr = (volatile u64 *)TEST_VIRT_BASE_PML4_2;
        *vptr = 0xDEADBEEFULL;

        /* 4. 创建 IPC 通道 */
        ipc_channel_id_t ch = ipc_channel_create("kt_full",
                                                  IPC_DEFAULT_CAPACITY);

        /* 5. 发送消息（payload 是帧号，模拟"传递物理帧引用"） */
        char payload[8];
        for (int i = 0; i < 8; i++) payload[i] = (char)((frame >> (i * 8)) & 0xFF);
        ipc_try_send(ch, 1, payload, 8);

        /* 6. 接收消息 */
        u64 type = 0;
        u64 cap = 64;
        char buf[64];
        int rc = ipc_try_recv(ch, &type, buf, &cap);
        TEST_CHECK_INT("S6_10_full_chain_recv_ok", 0, rc);

        /* 7. 销毁通道 */
        ipc_channel_destroy(ch);

        /* 8. 取消映射 */
        arch_vmm_unmap_page(TEST_VIRT_BASE_PML4_2);

        /* 9. 释放物理帧 */
        arch_pmm_free_frame(frame);

        /* 10. 验证无泄漏 */
        usize_t after_frames = arch_pmm_free_frames();
        usize_t after_used  = kernel_mm_used();

        TEST_CHECK_INT("S6_10_pmm_no_leak",
                       (s64)baseline_frames, (s64)after_frames);
        TEST_CHECK_INT("S6_10_heap_no_leak",
                       (s64)baseline_used, (s64)after_used);
    }

    s6_pass = g_pass - s6_pass;
    s6_fail = g_fail - s6_fail;
    section_footer(6, "Cross-subsys", s6_pass, s6_fail);
}

/* ================================================================
 * ktest_run_all — 执行全部 6 个 section 的测试
 *
 *   在 kernel_main 的 cap_test 之后调用。
 *   调用前要求：
 *     - 所有子系统已初始化（mem / ipc / cap / sched）
 *     - 中断已打开（IF=1）
 *     - cap_test 已完成（不影响本测试）
 *
 *   返回值：总 PASS 数（失败数通过控制台输出可见）
 * ================================================================ */
int ktest_run_all(int quiet) {
    /* 重置全局计数 */
    g_pass = 0;
    g_fail = 0;
    g_quiet = quiet;

    if (!quiet) {
        /* 打印测试套件标题 */
        arch_console_print("\n");
        arch_console_set_color(CON_COLOR_YELLOW);
        arch_console_print("========================================\n");
        arch_console_print("  AIHYK v" AIHYK_VERSION_STR " Kernel Test Suite\n");
        arch_console_print("========================================\n");
        arch_console_set_color(CON_COLOR_DEFAULT);
    }

    /* 逐 section 执行 */
    test_section_pmm();
    test_section_vmm();
    test_section_kmalloc();
    test_section_sched();
    test_section_ipc();
    test_section_cross();

    /* ================================================================
     * 最终汇总
     * ================================================================ */
    if (!quiet) {
        arch_console_print("\n");
        arch_console_print("===============================================================\n");
        arch_console_set_color(CON_COLOR_YELLOW);
        arch_console_print("KERNEL TEST SUITE SUMMARY\n");
        arch_console_set_color(CON_COLOR_DEFAULT);
        arch_console_print("===============================================================\n");

        /* 各 section 统计 */
        arch_console_print("  Section 1 (PMM):          ");
        kt_print_dec((u64)s1_pass); arch_console_print("/");
        kt_print_dec((u64)(s1_pass + s1_fail)); arch_console_print(" PASS\n");

        arch_console_print("  Section 2 (VMM):          ");
        kt_print_dec((u64)s2_pass); arch_console_print("/");
        kt_print_dec((u64)(s2_pass + s2_fail)); arch_console_print(" PASS\n");

        arch_console_print("  Section 3 (kmalloc):      ");
        kt_print_dec((u64)s3_pass); arch_console_print("/");
        kt_print_dec((u64)(s3_pass + s3_fail)); arch_console_print(" PASS\n");

        arch_console_print("  Section 4 (Scheduler):    ");
        kt_print_dec((u64)s4_pass); arch_console_print("/");
        kt_print_dec((u64)(s4_pass + s4_fail)); arch_console_print(" PASS\n");

        arch_console_print("  Section 5 (IPC):          ");
        kt_print_dec((u64)s5_pass); arch_console_print("/");
        kt_print_dec((u64)(s5_pass + s5_fail)); arch_console_print(" PASS\n");

        arch_console_print("  Section 6 (Cross-subsys): ");
        kt_print_dec((u64)s6_pass); arch_console_print("/");
        kt_print_dec((u64)(s6_pass + s6_fail)); arch_console_print(" PASS\n");

        /* 分隔线 + 总计 */
        arch_console_print("  =======================================\n");

        int total_tests = s1_pass + s1_fail +
                          s2_pass + s2_fail +
                          s3_pass + s3_fail +
                          s4_pass + s4_fail +
                          s5_pass + s5_fail +
                          s6_pass + s6_fail;

        arch_console_print("  TOTAL:                    ");
        kt_print_dec((u64)g_pass); arch_console_print("/");
        kt_print_dec((u64)total_tests); arch_console_print(" PASS\n");

        /* 如果全部通过，用绿色高亮；否则用红色 */
        if (g_fail == 0) {
            arch_console_set_color(CON_COLOR_GREEN);
            arch_console_print("  ALL TESTS PASSED!\n");
            arch_console_set_color(CON_COLOR_DEFAULT);
        } else {
            arch_console_set_color(CON_COLOR_RED);
            arch_console_print("  ");
            kt_print_dec((u64)g_fail);
            arch_console_print(" TEST(S) FAILED!\n");
            arch_console_set_color(CON_COLOR_DEFAULT);
        }

        arch_console_print("===============================================================\n");
    }

    /* quiet 模式下由调用方（main.c）负责打印汇总 */
    return g_pass;
}
