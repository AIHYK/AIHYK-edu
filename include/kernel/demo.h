/* ================================================================
 * kernel/demo.h — Demo 模块接口
 *
 * 将所有 demo 代码（L6 IPC + L7 Cap + L8 user + L9 crash）
 * 从 main.c 解耦到独立的 demo.c 编译单元。
 *
 * main.c 只需调用 demo_run()，传入用户程序二进制即可。
 * ================================================================ */

#ifndef KERNEL_DEMO_H
#define KERNEL_DEMO_H

#include <kernel/types.h>

/* demo_run — 运行所有 demo（L6 IPC + L7 Cap + L8 user + L9 crash）
 *
 * 传入用户程序二进制（由 main.c 的 user_image.S 提供）。
 * quiet=0: 详细输出; quiet=1: 只打印结果行 */
void demo_run(const void *hello_bin, u64 hello_len,
              const void *crash_bin, u64 crash_len,
              int quiet);

#endif
