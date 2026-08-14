/* ================================================================
 * kernel/util.c — 内核通用工具函数实现
 *
 * 实现 <kernel/util.h> 的接口。详见头文件说明（C8 修复）。
 * ================================================================ */

#include <kernel/util.h>
#include <arch/console.h>

/* ---------------------------------------------------------------
 * kprint_dec — 打印无符号 64 位十进制数
 *
 * 用"除 10 取余"法：123 → 3, 2, 1 → 反序输出 "123"
 * buf[21]：u64 最大 20 位十进制（2^64-1 = 18446744073709551615，20 位）
 * +1 字节给 '\0'（虽然没用上，防御性）。 */
void kprint_dec(u64 v) {
    char buf[21];
    int i = 0;

    if (v == 0) {
        arch_console_putchar('0');
        return;
    }

    while (v > 0 && i < 20) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }

    while (i > 0) {
        arch_console_putchar(buf[--i]);
    }
}

/* ---------------------------------------------------------------
 * kprint_dec_s — 打印有符号 64 位十进制数
 *
 *   v < 0：先输出 '-'，再转 unsigned 后取负
 *   `-(u64)v` 在 2's complement 下良定义：
 *     v = INT64_MIN = 0x8000000000000000
 *     (u64)v = 0x8000000000000000
 *     -(u64)v = 0x8000000000000000（wrap around）= 9223372036854775808
 *     输出 "-9223372036854775808" ✓
 *
 *   对照原 ktest.c 的 `(u64)(-v)`：
 *     v = INT64_MIN
 *     -v 在 signed 域溢出 → UB（编译器可能假定不会发生 → 优化出错） */
void kprint_dec_s(s64 v) {
    if (v < 0) {
        arch_console_putchar('-');
        kprint_dec(-(u64)v);
    } else {
        kprint_dec((u64)v);
    }
}

/* ---------------------------------------------------------------
 * kprint_hex — 打印 64 位十六进制数（带 "0x" 前缀，无前导零）
 *
 *   用"除 16 取余"法，反序输出。
 *   0 单独处理为 "0x0"（和原 print_hex / cap_print_hex 一致）。 */
void kprint_hex(u64 v) {
    char buf[17];
    int i = 0;
    static const char hex[] = "0123456789ABCDEF";

    if (v == 0) {
        arch_console_print("0x0");
        return;
    }

    while (v > 0 && i < 16) {
        buf[i++] = hex[v & 0xF];
        v >>= 4;
    }

    arch_console_print("0x");
    while (i > 0) {
        arch_console_putchar(buf[--i]);
    }
}
