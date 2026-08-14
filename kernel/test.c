/* ================================================================
 * kernel/test.c — 测试运行器（烟雾测试 + cap_test + ktest）
 *
 * 从 main.c 解耦出来的所有测试代码：
 *   - 烟雾测试：test_pmm / test_vmm / test_kmalloc
 *   - 测试运行器：test_run() → 烟雾测试 + cap_test + ktest + 汇总框
 *
 * main.c 只需调用 test_run(quiet) 即可。
 * ================================================================ */

#include <arch/console.h>
#include <arch/mem.h>
#include <kernel/cap_test.h>
#include <kernel/ktest.h>
#include <kernel/mm.h>
#include <kernel/panic.h>
#include <kernel/types.h>
#include <kernel/test.h>

/* ---------------------------------------------------------------
 * print_padded_dec — 右对齐打印十进制数（宽度 width）
 * --------------------------------------------------------------- */
static void print_padded_dec(u64 v, int width) {
    char buf[21];
    int i = 0;
    if (v == 0) { buf[i++] = '0'; }
    while (v > 0 && i < 20) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    /* pad with spaces */
    for (int p = i; p < width; p++) arch_console_putchar(' ');
    while (i > 0) arch_console_putchar(buf[--i]);
}

/* ---------------------------------------------------------------
 * test_pmm — PMM 烟雾测试（安静模式：只 OK/fail）
 * --------------------------------------------------------------- */
static int test_pmm(void) {
    paddr_t f1 = arch_pmm_alloc_frame();
    paddr_t f2 = arch_pmm_alloc_frame();
    paddr_t f3 = arch_pmm_alloc_frame();
    paddr_t f4 = arch_pmm_alloc_frame();

    KASSERT(f1 != 0 && f2 != 0 && f3 != 0 && f4 != 0);
    KASSERT(f1 != f2 && f1 != f3 && f1 != f4);
    KASSERT(f2 != f3 && f2 != f4 && f3 != f4);
    KASSERT((f1 & (PAGE_SIZE - 1)) == 0);
    KASSERT((f2 & (PAGE_SIZE - 1)) == 0);
    KASSERT((f3 & (PAGE_SIZE - 1)) == 0);
    KASSERT((f4 & (PAGE_SIZE - 1)) == 0);

    arch_pmm_free_frame(f2);
    paddr_t f5 = arch_pmm_alloc_frame();
    KASSERT(f5 == f2);

    arch_pmm_free_frame(f1);
    arch_pmm_free_frame(f3);
    arch_pmm_free_frame(f4);
    arch_pmm_free_frame(f5);

    return 1;  /* OK */
}

/* ---------------------------------------------------------------
 * test_vmm — VMM 烟雾测试（安静模式）
 * --------------------------------------------------------------- */
static int test_vmm(void) {
    vaddr_t test_virt = 0xFFFF800000100000ULL;

    paddr_t before = arch_vmm_get_phys(test_virt);
    KASSERT(before == 0);

    paddr_t test_phys = arch_pmm_alloc_frame();
    int rc = arch_vmm_map_page(test_virt, test_phys, PAGE_FLAGS_KERNEL_RW);
    KASSERT(rc == 0);

    paddr_t translated = arch_vmm_get_phys(test_virt);
    KASSERT(translated == test_phys);

    volatile u32 *vp = (volatile u32 *)test_virt;
    *vp = 0xDEADBEEFu;

    volatile u32 *pp = (volatile u32 *)test_phys;
    u32 read_back = *pp;
    KASSERT(read_back == 0xDEADBEEF);

    arch_vmm_unmap_page(test_virt);
    paddr_t after = arch_vmm_get_phys(test_virt);
    KASSERT(after == 0);

    arch_pmm_free_frame(test_phys);

    return 1;  /* OK */
}

/* ---------------------------------------------------------------
 * test_kmalloc — 内核堆烟雾测试（安静模式）
 * --------------------------------------------------------------- */
static int test_kmalloc(void) {
    void *p1 = kmalloc(64);
    void *p2 = kmalloc(128);
    void *p3 = kmalloc(256);

    KASSERT(p1 != NULL && p2 != NULL && p3 != NULL);
    KASSERT(p1 != p2 && p1 != p3 && p2 != p3);
    KASSERT(((u64)p1 & 15) == 0);
    KASSERT(((u64)p2 & 15) == 0);
    KASSERT(((u64)p3 & 15) == 0);

    *((u32 *)p1) = 0x11111111;
    *((u32 *)p2) = 0x22222222;
    *((u32 *)p3) = 0x33333333;
    KASSERT(*((u32 *)p1) == 0x11111111);
    KASSERT(*((u32 *)p2) == 0x22222222);
    KASSERT(*((u32 *)p3) == 0x33333333);

    u8 *p4 = (u8 *)kcalloc(1, 4096);
    KASSERT(p4 != NULL);
    int all_zero = 1;
    for (int i = 0; i < 4096; i++) {
        if (p4[i] != 0) { all_zero = 0; break; }
    }
    KASSERT(all_zero);

    void *p5 = kmalloc(32);
    *((u32 *)p5) = 0xABCDEF01;
    void *p5_new = krealloc(p5, 1024);
    KASSERT(p5_new != NULL);
    KASSERT(*((u32 *)p5_new) == 0xABCDEF01);

    kfree(p1);
    kfree(p2);
    kfree(p3);
    kfree(p4);
    kfree(p5_new);

    return 1;  /* OK */
}

/* ================================================================
 * test_run — 运行所有测试
 *
 * 1. 烟雾测试（test_pmm / test_vmm / test_kmalloc）— 安静，用 KASSERT
 * 2. cap_test_run_all(quiet) — 返回 cap_pass 数
 * 3. ktest_run_all(quiet)  — 返回 ktest_pass 数
 * 4. 打印 Test Suite 框
 * 5. 返回 total_pass = cap_pass + ktest_pass
 * ================================================================ */
int test_run(int quiet) {
    /* 烟雾测试（安静，失败则 KASSERT panic） */
    test_pmm();
    test_vmm();
    test_kmalloc();

    /* cap_test */
    int cap_pass = cap_test_run_all(quiet);

    /* ktest */
    int ktest_pass = ktest_run_all(quiet);

    /* 打印测试结果框 */
    arch_console_print("\n");
    arch_console_print(" +- Test Suite -------------------------+\n");

    /* cap_test 行 */
    arch_console_print(" |  cap_test      ");
    print_padded_dec((u64)cap_pass, 2);
    arch_console_print("/");
    print_padded_dec((u64)cap_pass, 2);
    arch_console_print("  PASS");
    arch_console_print("      |\n");

    /* ktest 行 */
    arch_console_print(" |  ktest        ");
    print_padded_dec((u64)ktest_pass, 3);
    arch_console_print("/");
    print_padded_dec((u64)ktest_pass, 3);
    arch_console_print("  PASS");
    arch_console_print("     |\n");

    arch_console_print(" |------------------------------------|\n");

    /* TOTAL 行（带绿色 + 标记） */
    int total_pass = cap_pass + ktest_pass;
    arch_console_print(" |  TOTAL        ");
    print_padded_dec((u64)total_pass, 3);
    arch_console_print("/");
    print_padded_dec((u64)total_pass, 3);
    arch_console_print("  PASS");
    arch_console_set_color(CON_COLOR_GREEN);
    arch_console_print("  +");
    arch_console_set_color(CON_COLOR_DEFAULT);
    arch_console_print("  |\n");

    arch_console_print(" +-------------------------------------+\n");

    return total_pass;
}
