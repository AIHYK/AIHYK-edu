/* ================================================================
 * include/kernel/ktest.h — AIHYK 内核全子系统回归测试 + 压力测试 + 边界测试
 *
 * 【测试套件设计】
 *
 * 对 L1-L9 所有子系统做完整的回归 / 压力 / 边界测试：
 *
 *   Section 1: PMM（物理内存管理器）— 边界 + 压力
 *   Section 2: VMM（虚拟内存管理器）— 边界 + 压力
 *   Section 3: kmalloc（内核堆）— 边界 + 压力
 *   Section 4: Scheduler（调度器）— 边界 + 压力
 *   Section 5: IPC（进程间通信）— 边界 + 压力
 *   Section 6: Cross-subsystem（跨子系统回归）
 *
 * 测试哲学：
 *   - 边界：每个 API 的每个错误返回路径都要被覆盖
 *   - 压力：反复操作，验证计数 / 状态一致性
 *   - 回归：一个子系统的操作不影响其他子系统
 *   - 不崩溃：用 TEST_CHECK_INT 比较，不用 KASSERT
 *   - 自清理：每个 section 结束后资源回到初始状态
 *   - Leak 检测：前后对比 free_frames / cap_total_caps()
 * ================================================================ */

#ifndef KERNEL_KTEST_H
#define KERNEL_KTEST_H

/* ktest_run_all — 执行全部 6 个 section 的测试
 *
 * 在 kernel_main 的 L7 cap_test 之后调用。
 * 调用前要求：
 *   - 所有子系统已初始化（mem / ipc / cap / sched）
 *   - 中断已打开（IF=1）
 *   - L7 cap_test 已完成（不影响本测试）
 *
 * 返回值：总 PASS 数（失败数通过控制台输出可见） */
int ktest_run_all(int quiet);

#endif /* KERNEL_KTEST_H */
