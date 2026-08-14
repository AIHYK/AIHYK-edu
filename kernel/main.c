/* ================================================================
 * kernel/main.c — 内核主函数（架构无关！）
 *
 * 纯内核入口：初始化所有子系统，可选运行 demo/test，进入 idle。
 *
 * Demo 和 Test 已解耦到独立编译单元：
 *   kernel/demo.c  — 所有 demo 任务（L6 IPC / L7 Cap / L8 User / L9 Crash）
 *   kernel/test.c  — 所有测试（烟雾测试 + cap_test + ktest）
 *
 * 编译配置：
 *   CONFIG_DEMO  — 是否运行 demo: 1=运行, 0=跳过
 *   CONFIG_TEST  — 是否运行测试: 1=运行, 0=跳过
 * ================================================================ */

#include <arch/boot.h>
#include <arch/console.h>
#include <arch/cpu.h>
#include <arch/idt.h>
#include <arch/irq.h>
#include <arch/mem.h>
#include <arch/pic.h>
#include <arch/pit.h>
#include <arch/task.h>
#include <arch/user.h>
#include <kernel/ipc.h>
#include <kernel/cap.h>
#include <kernel/mm.h>
#include <kernel/panic.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>
#include <kernel/types.h>
#include <kernel/util.h>

/* 解耦模块（可选） */
#include <kernel/demo.h>
#include <kernel/test.h>

/* 编译配置：demo/test 开关 */
#ifndef CONFIG_DEMO
#define CONFIG_DEMO  1
#endif

#ifndef CONFIG_TEST
#define CONFIG_TEST  1
#endif

/* 前置声明 */
void arch_keyboard_init(void);

/* 用户程序二进制（user_image.S 用 .incbin 嵌入 .rodata） */
extern const u8 user_hello_bin[];
extern const u8 user_hello_bin_end[];
extern const u8 user_crash_bin[];
extern const u8 user_crash_bin_end[];

static void print_size_kb(u64 bytes) {
    u64 kb = (bytes + 1023) / 1024;
    if (kb < 1024) { kprint_dec(kb); arch_console_print(" KB"); }
    else { u64 mb = (kb + 1023) / 1024; kprint_dec(mb); arch_console_print(" MB"); }
}

/* ================================================================
 * kernel_main — 内核 C 入口
 *
 * 被 entry.asm 的 _start 调用，永不返回。
 * ================================================================ */
void kernel_main(void) {
    /* 1. 解析启动信息 */
    struct boot_info boot_info;
    arch_boot_init(&boot_info);

    /* 2. 初始化早期控制台 */
    arch_console_init();

    /* 版本行 */
    arch_console_set_color(CON_COLOR_WHITE);
    arch_console_print("  /$$$$$$  /$$$$$$ /$$   /$$ /$$     /$$ /$$   /$$\n");
    arch_console_print(" /$$__  $$|_  $$_/| $$  | $$|  $$   /$$/| $$  /$$/\n");
    arch_console_print("| $$  \\ $$  | $$  | $$  | $$ \\  $$ /$$/ | $$ /$$/\n");
    arch_console_print("| $$$$$$$$  | $$  | $$$$$$$$  \\  $$$$/  | $$$$$/\n");
    arch_console_print("| $$__  $$  | $$  | $$__  $$   \\  $$/   | $$  $$\n");
    arch_console_print("| $$  | $$  | $$  | $$  | $$    | $$    | $$\\  $$\n");
    arch_console_print("| $$  | $$ /$$$$$$| $$  | $$    | $$    | $$ \\  $$\n");
    arch_console_print("|__/  |__/|______/|__/  |__/    |__/    |__/  \\__/\n");
    arch_console_print("v" AIHYK_VERSION_STR " | 64-bit hybrid kernel\n");
    arch_console_set_color(CON_COLOR_DEFAULT);

    /* 3. 初始化所有子系统 */
    arch_pic_init();
    arch_irq_init();
    arch_idt_init();
    arch_tss_init();
    arch_pit_init(PIT_DEFAULT_FREQUENCY);
    arch_pic_unmask(0);
    arch_keyboard_init();
    arch_sti();

    arch_mem_init(&boot_info);
    kernel_mm_init();

    sched_init();
    ipc_init();
    cap_init_subsystem();

    /* Boot 组件行 */
    arch_console_print(" boot . idt . pic . pit . pmm . vmm . heap . sched . ipc . cap . user");
    arch_console_set_color(CON_COLOR_GREEN);
    arch_console_print("  OK\n");
    arch_console_set_color(CON_COLOR_DEFAULT);

    /* 4. 可选：运行 demo */
#if CONFIG_DEMO
    demo_run(user_hello_bin, (u64)(user_hello_bin_end - user_hello_bin),
             user_crash_bin, (u64)(user_crash_bin_end - user_crash_bin), 1);
#endif

    /* 5. 可选：运行测试 */
#if CONFIG_TEST
    test_run(1);
#endif

    /* 6. Idle 行 */
    u64 total_usable = 0;
    for (int i = 0; i < boot_info.region_count; i++) {
        if (boot_info.regions[i].type == MEM_USABLE) {
            total_usable += boot_info.regions[i].length;
        }
    }

    arch_console_print("\n");
    arch_console_set_color(CON_COLOR_WHITE);
    arch_console_print("AIHYK v" AIHYK_VERSION_STR " idle. ");
    arch_console_set_color(CON_COLOR_DEFAULT);
    print_size_kb(total_usable);
    arch_console_print(" | ");
    kprint_dec((u64)arch_pmm_free_frames());
    arch_console_print(" free frames\n");

    /* 7. 主循环（halt + 中断唤醒） */
    while (1) {
        __asm__ volatile ("hlt");
    }

    arch_halt();
}
