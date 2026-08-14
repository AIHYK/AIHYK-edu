/* ================================================================
 * arch/x86_64/pit.c — 8254 PIT（可编程间隔定时器）实现
 *
 * 【Lesson 3 核心新增】
 *
 * 8254 PIT 是 PC 上的定时器芯片：
 *   - 输入时钟频率固定 1.193182 MHz（≈ 1.193 MHz）
 *   - 有 3 个独立通道，我们用通道 0（连到 IRQ0）
 *   - 通道 0 是 16 位计数器，每来一个时钟脉冲就 -1
 *   - 减到 0 时触发 IRQ0 中断，并重新装载初值
 *
 * 配置端口：
 *   0x40 - 通道 0 数据端口
 *   0x41 - 通道 1 数据端口（DRAM 刷新，PC/AT 后已废弃）
 *   0x42 - 通道 2 数据端口（PC 扬声器）
 *   0x43 - 控制字寄存器
 *
 * 控制字格式（写到 0x43）：
 *   bit 7-6  通道选择（00=ch0, 01=ch1, 10=ch2, 11=read-back）
 *   bit 5-4  访问模式（00=latch, 01=low byte, 10=high byte, 11=both）
 *   bit 3-1  工作模式（000=interrupt on terminal count, ...
 *                       011=square wave generator）
 *   bit 0    计数制（0=binary, 1=BCD）
 *
 *   我们的配置：0x36 = 00 11 011 0
 *     通道 0 | 两个字访问 | mode 3 (方波) | binary
 *
 * 【为什么用 mode 3（方波）而不是 mode 2】
 *   mode 2 输出的是窄脉冲，mode 3 输出的是 50% 占空比方波，
 *   都能触发 IRQ0，但 mode 3 对旧硬件更友好。
 *   Linux 也用 mode 3。
 *
 * 【tick 计数】
 *   每次 IRQ0 中断，全局 tick 计数 +1。
 *   100Hz 下每 tick = 10ms。
 *   后续课程可以用 tick 实现调度器时基。
 *
 * 【为什么 tick 是 volatile u64】
 *   - volatile：防止编译器缓存到寄存器（中断里改，主循环里读，必须同步）
 *   - u64：tick 可能很大（运行几小时就超过 u32 范围）
 *
 *   但 64 位变量的读写在 32 位原子性上有问题（非原子）。
 *   长模式下 64 位读写是原子的，所以安全。
 *   如果用 32 位环境，需要关中断读"高 32 位+低 32 位"两次。
 * ================================================================ */

#include <arch/io.h>
#include <arch/pit.h>
#include <kernel/types.h>

/* PIT 端口 */
#define PIT_CHANNEL0_DATA 0x40
#define PIT_CHANNEL1_DATA 0x41
#define PIT_CHANNEL2_DATA 0x42
#define PIT_COMMAND       0x43

/* PIT 控制字常量 */
#define PIT_CMD_CH0         0x00   /* 通道 0 */
#define PIT_CMD_ACCESS_BOTH 0x30   /* 两个字访问（先低后高） */
#define PIT_CMD_MODE3       0x06   /* mode 3 (square wave) */
#define PIT_CMD_BINARY      0x00   /* binary 计数（非 BCD） */

/* 组合控制字：通道 0 + 两字访问 + mode 3 + binary */
#define PIT_CMD_CONFIG (PIT_CMD_CH0 | PIT_CMD_ACCESS_BOTH | PIT_CMD_MODE3 | PIT_CMD_BINARY)

/* ---------------------------------------------------------------
 * tick_count - 全局 tick 计数
 *
 * 每次 IRQ0 中断，arch_pit_increment_tick 把它 +1。
 * arch_pit_get_tick_count 读取（供主循环、调度器使用）。
 *
 * 【volatile 原因】
 *   中断上下文修改 tick_count，
 *   主循环（或其他上下文）读取 tick_count。
 *   没有 volatile，编译器可能把读缓存到寄存器，
 *   主循环看不到 tick 变化（死循环里读永远是旧值）。
 *   volatile 强制每次都从内存读，保证看到最新值。
 *
 * 【64 位原子性】
 *   x86-64 长模式下，对齐的 64 位读写是原子的（一条 mov 指令）。
 *   所以单核下无需关中断读 tick。
 *   多核下需要更进一步（用 lock cmpxchg 等），目前单核简化。
 * --------------------------------------------------------------- */
static volatile u64 tick_count = 0;

/* ---------------------------------------------------------------
 * arch_pit_init — 初始化 8254 PIT 通道 0
 *
 * 参数：frequency - 期望的中断频率（Hz），1~65535
 *
 * 流程：
 *   1. 计算 divisor = PIT_BASE_FREQUENCY / frequency
 *      基础频率 1.193182 MHz / 期望频率 = 分频值
 *   2. 写控制字到 0x43（通道 0，两字节访问，mode 3，二进制）
 *   3. 写 divisor 低字节到 0x40
 *   4. 写 divisor 高字节到 0x40
 *
 *   之后 PIT 通道 0 每秒触发 `frequency` 次 IRQ0。
 *
 * 【为什么 divisor 上限 65535】
 *   PIT 通道计数器是 16 位的，最大值 65535（写 0 等于 65536）。
 *   如果 frequency 太小（< 19 Hz），divisor 会超过 65535，被截断。
 *   实际频率最小约 18.2 Hz（divisor=65535），最大 1.193 MHz（divisor=1）。
 *
 * 【为什么写低字节在前】
 *   控制字设了"两字访问"模式（bit 5-4 = 11），
 *   PIT 规定先写低字节、再写高字节（顺序固定）。
 * --------------------------------------------------------------- */
void arch_pit_init(u32 frequency) {
    /* 防御性：frequency 不能为 0（会除零） */
    if (frequency == 0) {
        frequency = PIT_DEFAULT_FREQUENCY;
    }

    /* 计算分频值
     *   divisor = 输入频率 / 输出频率
     *   例如：1193182 / 100 = 11932 → 100 Hz */
    u32 divisor = PIT_BASE_FREQUENCY / frequency;

    /* 截断到 16 位（PIT 计数器最大 65535）*/
    if (divisor > 0xFFFF) {
        divisor = 0xFFFF;       /* 最小约 18.2 Hz */
    }
    if (divisor == 0) {
        divisor = 0xFFFF;       /* 0 等价于 65536，但我们直接写 65535 */
    }

    /* 写控制字：通道 0, 两字节访问, mode 3, binary */
    outb(PIT_COMMAND, PIT_CMD_CONFIG);

    /* 写分频值低字节（必须先低后高）*/
    outb(PIT_CHANNEL0_DATA, (u8)(divisor & 0xFF));
    /* 写分频值高字节 */
    outb(PIT_CHANNEL0_DATA, (u8)((divisor >> 8) & 0xFF));
}

/* ---------------------------------------------------------------
 * arch_pit_increment_tick - 增加 tick 计数
 *
 * 这个函数由 IRQ0 中断处理程序调用（在 irq.c 里）。
 *
 * 【为什么不直接在 irq.c 里改 tick_count】
 *   - tick_count 是 pit.c 的 static 变量，外部不能访问
 *   - 提供 increment 接口让 irq.c 调用，封装性好
 *   - 后续可能改 PIT 实现（HPET/Local APIC timer），
 *     increment 接口不变
 * --------------------------------------------------------------- */
void arch_pit_increment_tick(void) {
    tick_count++;
}

/* ---------------------------------------------------------------
 * arch_pit_get_tick_count — 获取自启动以来的 tick 数
 *
 * 100Hz 下每 tick = 10ms。
 *
 * 用例：
 *   - 简单延时（忙等）
 *   - 时间戳
 *   - 后续课程的调度器时基
 *
 * 【为什么单核下无需关中断读】
 *   x86-64 长模式下，对齐的 64 位 mov 是原子的，
 *   不会被中断打断成"半读"。读到的要么是旧值要么是新值，
 *   不会是混合值。
 *
 *   32 位环境下读 64 位变量不是原子的（可能读到高 32 位新+低 32 位旧），
 *   需要关中断读两次。长模式下无此问题。
 * --------------------------------------------------------------- */
u64 arch_pit_get_tick_count(void) {
    return tick_count;
}
