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
#include <arch/console.h>
#include <arch/cpu.h>

/* ---------------------------------------------------------------
 * print_decimal — 打印一个非负十进制整数
 *
 * panic 只需打印行号（正数），所以只实现非负版本。
 * 用最简单的"除 10 取余"法：
 *   123 → 3, 2, 1 → 反序输出 "123"
 * --------------------------------------------------------------- */
static void print_decimal(int n) {
    char buf[16];
    int i = 0;

    if (n == 0) {
        arch_console_putchar('0');
        return;
    }

    if (n < 0) {
        arch_console_putchar('-');
        n = -n;     /* 转正；INT_MIN 会溢出，但 panic 行号不会是 INT_MIN */
    }

    while (n > 0) {
        buf[i++] = (char)('0' + (n % 10));
        n /= 10;
    }

    /* 反序输出 */
    while (i > 0) {
        arch_console_putchar(buf[--i]);
    }
}

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
    print_decimal(line);
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
