/* ================================================================
 * kernel/panic.c — 内核 panic 实现
 *
 * panic 流程：
 *   1. 关中断（防止打印时被中断打断）
 *   2. 打印 panic 横幅
 *   3. 打印文件名和行号
 *   4. 打印错误信息
 *   5. 调用 arch_halt() 永久停机
 *
 * 由于是 freestanding 代码（-nostdinc -nostdlib），
 * 不能用 printf，自己实现简单的整数打印。
 *
 * 注意：panic 内部不能再调用 KASSERT/panic，否则无限递归。
 * ================================================================ */

#include <kernel/panic.h>
#include <kernel/util.h>
#include <arch/console.h>
#include <arch/cpu.h>

/* ---------------------------------------------------------------
 * panic — 内核 panic
 *
 * 调用后不返回。
 * --------------------------------------------------------------- */
void panic(const char *file, int line, const char *msg) {
    /* 关中断：panic 期间不能被中断打断 */
    arch_cli();

    /* 换行，确保 panic 信息在新行开始 */
    arch_console_print("\n");

    /* 红色横幅 */
    arch_console_set_color(CON_COLOR_RED);
    arch_console_print("!!! KERNEL PANIC !!!\n");
    arch_console_set_color(CON_COLOR_DEFAULT);

    /* 位置信息 */
    arch_console_print("  at ");
    if (file) {
        arch_console_print(file);
    } else {
        arch_console_print("(unknown file)");
    }
    arch_console_print(":");
    kprint_dec_s((s64)line);   /* 【C8】用统一工具；【C10】INT_MIN 安全 */
    arch_console_print("\n");

    /* 错误信息 */
    arch_console_set_color(CON_COLOR_YELLOW);
    arch_console_print("  reason: ");
    if (msg) {
        arch_console_print(msg);
    } else {
        arch_console_print("(no message)");
    }
    arch_console_print("\n");
    arch_console_set_color(CON_COLOR_DEFAULT);

    /* 永久停机，不返回 */
    arch_halt();

    /* arch_halt 不返回，这里只是消除编译器"non-void function
     * might return" 的警告（虽然本函数是 void，但加 while 兜底更稳） */
    while (1) {}
}
