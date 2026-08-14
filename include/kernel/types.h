/* ================================================================
 * kernel/types.h — 通用类型定义 + 内核版本
 *
 * 内核不能 include <stdint.h>（我们用了 -nostdinc），
 * 所以自己定义固定宽度的类型。
 *
 * 为什么不用 int/long？
 *   - int 的大小随编译器变（16/32/64位都有可能）
 *   - 内核需要精确控制大小：
 *     页表项、物理地址、capability ID...
 *   - 固定宽度类型 = 在所有平台上行为一致
 *
 * 【Lesson 2 后】
 *   内核已切换到 64 位长模式：
 *   - paddr_t / vaddr_t 为 u64（与指针宽度一致）
 *   - 编译器用 -m64 生成 64 位代码
 * ================================================================ */

#ifndef KERNEL_TYPES_H
#define KERNEL_TYPES_H

/* ---- 内核版本（单点定义，全内核引用）---- */
#define AIHYK_VERSION_MAJOR  0
#define AIHYK_VERSION_MINOR  2
#define AIHYK_VERSION_PATCH  0
/* 字符串形式："0.2.0" — 供 banner / idle 行等使用 */
#define AIHYK_VERSION_STR  "0.2.0"

/* 固定宽度整数类型
 * 这些定义对 GCC/Clang 在 x86 上有效
 * 如果移植到其他编译器，可能需要调整 */
typedef unsigned char      u8;     /*  8 位无符号，0 ~ 255 */
typedef unsigned short     u16;    /* 16 位无符号，0 ~ 65535 */
typedef unsigned int       u32;    /* 32 位无符号，0 ~ 4294967295 */
typedef unsigned long long u64;    /* 64 位无符号 */

typedef signed char        s8;     /*  8 位有符号，-128 ~ 127 */
typedef signed short       s16;    /* 16 位有符号 */
typedef signed int         s32;    /* 32 位有符号 */
typedef signed long long   s64;    /* 64 位有符号 */

/* 地址类型
 * 给它们起单独的名字，让代码自文档化：
 *   paddr_t phys = ...;     ← 明确是物理地址
 *   vaddr_t virt = ...;     ← 明确是虚拟地址
 *
 * 【长模式阶段】64 位：地址宽度 = 64 位
 * 指针宽度也是 64 位，所以 paddr_t 和 void* 等宽。
 */
typedef u64 paddr_t;       /* 物理地址（64位） */
typedef u64 vaddr_t;       /* 虚拟地址（64位） */
typedef u64 usize_t;       /* 大小/偏移量（64位） */

/* NULL 定义
 * -nostdinc 下没有 <stddef.h>，自己定义
 * 放这里供所有内核代码使用 */
#ifndef NULL
#define NULL ((void *)0)
#endif

/* 布尔类型
 * 内核不用 <stdbool.h>，自己定义
 * C89 模式下没有内置 _Bool，用 int 代替 */
typedef int bool;
#define true  1
#define false 0

#endif /* KERNEL_TYPES_H */
