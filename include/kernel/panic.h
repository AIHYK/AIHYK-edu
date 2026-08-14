/* ================================================================
 * kernel/panic.h — 内核 panic 机制
 *
 * panic = 内核遇到不可恢复的错误，立即停机。
 * KASSERT = 运行时断言，失败则 panic。
 *
 * 用法：
 *   panic(__FILE__, __LINE__, "reason");          // 直接调用
 *   PANIC("reason");                              // 自动带文件名行号
 *   KASSERT(ptr != NULL);                         // 断言
 *
 * 设计说明：
 *   - panic() 不返回，调用后永久停机
 *   - panic() 会先关中断，避免打印过程中被中断打断
 *   - panic() 打印位置和原因后调用 arch_halt()
 *   - 这是 freestanding 代码，不能用 printf，自己拼输出
 * ================================================================ */

#ifndef KERNEL_PANIC_H
#define KERNEL_PANIC_H

/* ---------------------------------------------------------------
 * panic — 内核 panic
 *
 * 参数：
 *   file - 源文件名（__FILE__）
 *   line - 行号（__LINE__）
 *   msg  - 错误描述
 *
 * 调用后不返回（内部 arch_halt()）
 * --------------------------------------------------------------- */
void panic(const char *file, int line, const char *msg);

/* ---------------------------------------------------------------
 * PANIC — panic 的便捷宏
 * 自动填入当前文件名和行号
 * --------------------------------------------------------------- */
#define PANIC(msg) panic(__FILE__, __LINE__, (msg))

/* ---------------------------------------------------------------
 * KASSERT — 内核断言
 *
 * 如果 cond 为假，触发 panic。
 * 例：
 *   KASSERT(ptr != NULL);
 *   KASSERT(size > 0 && size < 4096);
 *
 * 失败信息会包含 #cond 的字符串形式（字符串化），
 * 方便定位是哪个断言挂了。
 * --------------------------------------------------------------- */
#define KASSERT(cond) \
    do { \
        if (!(cond)) { \
            panic(__FILE__, __LINE__, "KASSERT failed: " #cond); \
        } \
    } while (0)

#endif /* KERNEL_PANIC_H */
