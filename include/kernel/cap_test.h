/* ================================================================
 * kernel/cap_test.h — L7 Capability 框架的边界测试 + 压力测试
 *
 * 【Lesson 7 测试套件】
 *
 * 本文件声明 cap_test_run_all()，它在 kernel_main 的 L7 demo
 * 之后被调用，对 cap.c 实现做完整的边界 + 压力测试。
 *
 * 测试分 7 个 section（详见 cap_test.c）：
 *   A. 边界 — 非法参数（NULL / 越界 slot / 未使用 slot）
 *   B. 边界 — 权限强制（无 SEND/RECV/MINT/DESTRUCT → NORIGHT）
 *   C. 边界 — CSpace 耗尽（31 槽全满 + 槽位复用）
 *   D. 生命周期 — 双操作 / 删除后再用 / 销毁后悬空 cap
 *   E. 跨任务 — revoke / destroy_channel 在其他任务的 cspace 生效
 *   F. cap 传递 — send_with_cap / recv_with_cap 边界 + 正常路径
 *   G. 压力 — 反复 create/destroy、mint/revoke、leak 检测
 *
 * 每个 case 打印 [PASS]/[FAIL] + 详细数字，最后打印汇总。
 *
 * 设计原则：
 *   - 测试不使用 KASSERT（避免崩溃），用 TEST_CHECK_INT 比较期望值
 *   - 每个 case 自清理（create 的 channel 最后 destroy）
 *   - 多任务测试用"休眠 helper"模式：init 直接读写 helper 的 cspace，
 *     不需要 helper 真的跑（helper 只 yield+exit）
 * ================================================================ */

#ifndef KERNEL_CAP_TEST_H
#define KERNEL_CAP_TEST_H

/* cap_test_run_all — 执行全部 7 个 section 的测试
 *
 * 在 kernel_main 的 L7 demo "All L7 demo tasks completed!" 之后调用。
 * 调用前要求：
 *   - cap_init_subsystem 已执行
 *   - 中断已打开（IF=1）
 *   - init 的 cspace 已存在（cap_init_subsystem 中初始化）
 *   - L7 demo 已清理完毕（cap_total_caps() == 0 最理想，但测试自带
 *     清理逻辑，不依赖此前状态）
 *
 * 返回值：总 PASS 数（失败数通过控制台输出可见） */
int cap_test_run_all(int quiet);

#endif /* KERNEL_CAP_TEST_H */
