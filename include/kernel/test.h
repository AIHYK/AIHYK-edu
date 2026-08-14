#ifndef KERNEL_TEST_H
#define KERNEL_TEST_H

#include <kernel/types.h>

/* test_run — 运行所有测试（烟雾测试 + cap_test + ktest）
 *
 * quiet=0: 详细输出; quiet=1: 只打印汇总行
 * 返回值: 总 PASS 数（cap_pass + ktest_pass） */
int test_run(int quiet);

#endif
