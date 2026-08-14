/* ================================================================
 * kernel/util.h — 内核通用工具函数（架构无关）
 *
 * 【C8 修复】提取重复代码
 *
 *   原先 print_dec / print_hex 在 9 个文件里各自有一份近似副本：
 *     kernel/panic.c    (print_decimal, int 参数)
 *     kernel/main.c     (print_dec, u64 参数)
 *     kernel/sched.c    (print_dec + print_hex, u64 参数)
 *     kernel/ipc.c      (print_dec, u64 参数)
 *     kernel/cap.c      (cap_print_dec + cap_print_hex)
 *     kernel/demo.c     (print_dec)
 *     kernel/syscall.c  (print_dec_local)
 *     kernel/ktest.c    (kt_print_dec + kt_print_dec_s + kt_print_hex)
 *     kernel/cap_test.c (ct_print_dec + ct_print_dec_s)
 *     arch/x86_64/irq.c (print_dec)
 *     arch/x86_64/exceptions.c (print_dec_u64 + print_hex_u64)
 *
 *   复制粘贴的隐患：
 *     - 修改任一份不会同步其他份
 *     - ktest.c / cap_test.c 的 print_dec_s 有 INT_MIN UB（-(u64)v 溢出）
 *     - 教学项目应展示 DRY，而非"每个 .c 自己造轮子"
 *
 *   统一接口：
 *     kprint_dec(u64)    — 无符号十进制
 *     kprint_dec_s(s64)  — 有符号十进制（负数带 '-'，INT64_MIN 安全）
 *     kprint_hex(u64)    — 十六进制（带 "0x" 前缀，和原实现一致）
 *
 *   所有输出走 arch_console_putchar / arch_console_print，
 *   freestanding 友好（无 printf 依赖）。
 * ================================================================ */

#ifndef KERNEL_UTIL_H
#define KERNEL_UTIL_H

#include <kernel/types.h>

/* ---------------------------------------------------------------
 * kprint_dec — 打印无符号 64 位十进制数
 *
 *   例：kprint_dec(123) → "123"
 *       kprint_dec(0)   → "0" */
void kprint_dec(u64 v);

/* ---------------------------------------------------------------
 * kprint_dec_s — 打印有符号 64 位十进制数
 *
 *   例：kprint_dec_s(123)   → "123"
 *       kprint_dec_s(-45)   → "-45"
 *       kprint_dec_s(0)     → "0"
 *       kprint_dec_s(INT64_MIN) → "-9223372036854775808"（安全，无 UB）
 *
 *   【C8 修复】原 ktest.c / cap_test.c 的 print_dec_s 用 `(u64)(-v)`，
 *   v == INT64_MIN 时 -v 溢出是 UB。改用 `-(u64)v` 在 2's complement
 *   下良定义（结果 = UINT64_MAX - v + 1）。 */
void kprint_dec_s(s64 v);

/* ---------------------------------------------------------------
 * kprint_hex — 打印 64 位十六进制数（带 "0x" 前缀）
 *
 *   例：kprint_hex(0x1234) → "0x1234"
 *       kprint_hex(0)      → "0x0"
 *   不输出前导零（和原 print_hex / cap_print_hex 一致）。 */
void kprint_hex(u64 v);

#endif /* KERNEL_UTIL_H */
