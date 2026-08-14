/* ================================================================
 * arch/x86_64/console.c — VGA 文本模式的控制台实现
 *
 * 实现 arch/console.h 定义的接口。
 *
 * VGA 文本模式原理：
 *   PC 显卡在内存中映射了一块区域：0xB8000 ~ 0xBFFFF
 *   这块内存的内容会自动显示在屏幕上
 *   不需要任何驱动，不需要任何初始化
 *   只要 CPU 能访问内存，就能写屏幕
 *
 * 屏幕规格：
 *   80 列 x 25 行 = 2000 个字符位置
 *   每个位置占 2 字节：
 *     低字节 = ASCII 字符码 (0~127)
 *     高字节 = 颜色属性
 *
 * 颜色属性格式（1 字节）：
 *   bit 7:   闪烁 (BLINK)
 *   bit 6-4: 背景色 (0=黑 1=蓝 2=绿 4=红 7=灰...)
 *   bit 3-0: 前景色 (同上)
 *
 * 常用颜色组合：
 *   0x07 = 浅灰字黑底（最常用，好读）
 *   0x0F = 白字黑底（高亮标题）
 *   0x0A = 亮绿字黑底（成功/正常信息）
 *   0x0C = 亮红字黑底（错误/警告）
 *   0x0E = 黄字黑底（注意）
 *   0x0B = 亮青字黑底（信息）
 *
 * 【修复记录】
 *   - 用 arch/io.h 的 outb() 替换裸内联汇编
 *   - 新增 \t（制表符，对齐到 8 列边界）和 \b（退格）支持
 *
 * 【Lesson 2 变更】
 *   - 适配 64 位长模式：
 *     VGA 物理地址 0xB8000 通过 identity mapping 直接可访问
 *     指针宽度从 32 位变 64 位，但 0xB8000 < 4GB，地址值不变
 *     用 0xB8000ULL 显式声明 64 位字面量，避免编译器警告
 *   - cursor_pos 类型从 int 改为 u64（适配 64 位运算）
 *
 * 【Lesson 2 PVH 增强】
 *   - 同时输出到 COM1 串口（0x3F8），方便用 -serial stdio 看内核日志
 *   - 串口在 QEMU 下无需初始化也能写（QEMU 默认已就绪），
 *     但我们仍做标准初始化（8N1, 115200 baud），兼容真硬件
 *   - 每个字符同时写 VGA 和串口，不影响现有 VGA 逻辑
 * ================================================================ */

#include <arch/console.h>
#include <arch/io.h>
#include <kernel/types.h>

/* ---------------------------------------------------------------
 * COM1 串口端口（PC 标准）
 *
 *   0x3F8: 数据寄存器（THR，写一个字节到串口）
 *   0x3F9: 中断使能寄存器（IER）
 *   0x3FA: FIFO 控制寄存器（FCR）
 *   0x3FB: 线控制寄存器（LCR），bit 7 = DLAB（除数访问锁存）
 *   0x3FC: modem 控制寄存器（MCR）
 *   0x3FD: 线状态寄存器（LSR），bit 5 = THRE（THR Empty，可写）
 *
 * 波特率除数（DLAB=1 时写 0x3F8/0x3F9）：
 *   除数 = 1 → 115200 baud（QEMU/真硬件通用）
 * --------------------------------------------------------------- */
#define COM1_DATA  0x3F8
#define COM1_IER   0x3F9
#define COM1_FCR   0x3FA
#define COM1_LCR   0x3FB
#define COM1_MCR   0x3FC
#define COM1_LSR   0x3FD

/* LSR bit 5 = THRE（发送保持寄存器空，可以写下一个字节） */
#define LSR_THRE   0x20

/* ---------------------------------------------------------------
 * serial_init — 初始化 COM1 串口（8N1, 115200 baud）
 *
 * 标准初始化序列（PC 16550 UART）：
 *   1. 禁用中断（IER = 0）
 *   2. 设 DLAB=1，写波特率除数（115200 = 除数 1）
 *   3. 设 DLAB=0 + 8N1（LCR = 0x03）
 *   4. 启用 FIFO（FCR = 0xC7）
 *   5. 设 modem 控制信号（MCR = 0x0B：DTR, RTS, OUT2）
 *
 * QEMU 下串口默认就能用，但真硬件需要完整初始化
 * --------------------------------------------------------------- */
static void serial_init(void) {
    outb(COM1_IER, 0x00);       /* 禁用中断 */
    outb(COM1_LCR, 0x80);       /* DLAB=1，准备写波特率 */
    outb(COM1_DATA, 0x01);      /* 除数低字节 = 1（115200 baud） */
    outb(COM1_IER, 0x00);       /* 除数高字节 = 0 */
    outb(COM1_LCR, 0x03);       /* DLAB=0, 8N1（8 数据位，无校验，1 停止位）*/
    outb(COM1_FCR, 0xC7);       /* 启用 FIFO，清空，14 字节阈值 */
    outb(COM1_MCR, 0x0B);       /* DTR, RTS, OUT2（允许中断输出）*/
}

/* ---------------------------------------------------------------
 * serial_putc — 通过 COM1 输出一个字符
 *
 * 轮询 LSR 的 THRE 位，等发送保持寄存器空才写
 * （不写就丢可能导致字符丢失）
 *
 * 换行符特殊处理：
 *   terminal 期望 \r\n（CR+LF），而内核内部只用 \n
 *   所以输出 \n 前先输出 \r，避免终端显示错位
 * --------------------------------------------------------------- */
/* ---------------------------------------------------------------
 * serial_putc — COM1 输出（带超时保护，修复 QEMU 默认模式假死）
 *
 *   【关键修复】原实现无限轮询 THRE，QEMU 默认模式（vc 后端）下
 *   THRE 可能不置位 → serial_putc 永不返回 → ISR 卡死 → EOI 不发 →
 *   PIC 屏蔽所有 IRQ → 内核冻结。加有界等待，超时后直接写。
 * --------------------------------------------------------------- */
/* serial_putc — COM1 输出（检查一次 THRE，非阻塞）
 *
 *   就绪则写，不就绪则跳过。任何 ISR 调用都能立即返回。
 *   配合 specific EOI 修复，不会卡死。
 *   正常情况（-serial stdio / VMware）：QEMU 立即置 THRE，几乎不丢字节。
 *   异常情况（vc 后端不置 THRE）：跳过字节，但 VGA 输出不受影响。 */
static void serial_putc(char c) {
    if (c == '\n') {
        serial_putc('\r');
    }
    if (inb(COM1_LSR) & LSR_THRE) {
        outb(COM1_DATA, (u8)c);
    }
}

/* ---------------------------------------------------------------
 * VGA 文本模式缓冲区的物理地址
 *
 * 0xB8000 是 VGA 文本模式第 0 页的起始地址
 *
 * volatile 含义：
 *   告诉编译器"这个地址的值可能被硬件（显卡）修改"
 *   "不要做以下优化："
 *   1. 缓存到寄存器（每次必须真的读写内存）
 *   2. 删除"无用"的写
 *   3. 重排读写顺序
 *
 * unsigned short *:
 *   每个字符位置是 2 字节（字符+颜色）
 *   用 16 位指针，一次操作一个完整位置
 * --------------------------------------------------------------- */
static volatile unsigned short *const VGA = (unsigned short *)0xB8000ULL;

/* 屏幕尺寸常量 */
#define VGA_COLS  80           /* 每行 80 个字符 */
#define VGA_ROWS  25            /* 共 25 行 */
#define VGA_SIZE  (VGA_COLS * VGA_ROWS)  /* 总共 2000 个位置 */

/* 制表符宽度（对齐到 8 的倍数） */
#define TAB_WIDTH 8

/* ---------------------------------------------------------------
 * 当前颜色属性
 * 默认 0x07 = 浅灰字黑底
 * arch_console_set_color() 会修改这个值
 * --------------------------------------------------------------- */
static unsigned char current_color = 0x07;

/* ---------------------------------------------------------------
 * 颜色映射表
 * 把 CON_COLOR_* 抽象颜色编号映射到 VGA 颜色属性字节
 * --------------------------------------------------------------- */
static const unsigned char color_map[] = {
    [CON_COLOR_DEFAULT] = 0x07,   /* 浅灰 */
    [CON_COLOR_WHITE]   = 0x0F,   /* 白色高亮 */
    [CON_COLOR_GREEN]   = 0x0A,   /* 亮绿 */
    [CON_COLOR_RED]     = 0x0C,   /* 亮红 */
    [CON_COLOR_YELLOW]  = 0x0E,   /* 黄色 */
    [CON_COLOR_CYAN]    = 0x0B,   /* 亮青 */
};

/* ---------------------------------------------------------------
 * 光标位置
 * 值 0~1999，对应屏幕上第 0 个到第 1999 个字符位置
 * 位置 = 行号 * 80 + 列号
 * --------------------------------------------------------------- */
static int cursor_pos = 0;

/* ---------------------------------------------------------------
 * scroll_up — 屏幕向上滚动一行
 *
 * 效果：
 *   第 1 行 -> 第 0 行
 *   第 2 行 -> 第 1 行
 *   ...
 *   第 24 行 -> 清空（变成空白行）
 *
 * 这是最简单的滚动方式：逐位置拷贝
 * 更快的方式：操纵 VGA CRTC 起始地址寄存器
 *   （后续课程可优化，教学项目当前够用）
 * --------------------------------------------------------------- */
static void scroll_up(void) {
    /* 把第 1~24 行拷到第 0~23 行 */
    int i;
    for (i = 0; i < VGA_SIZE - VGA_COLS; i++) {
        VGA[i] = VGA[i + VGA_COLS];
    }

    /* 清空第 24 行（最后一行） */
    for (i = VGA_SIZE - VGA_COLS; i < VGA_SIZE; i++) {
        VGA[i] = (current_color << 8) | ' ';
    }

    /* 光标移到第 24 行开头 */
    cursor_pos = VGA_SIZE - VGA_COLS;
}

/* ---------------------------------------------------------------
 * update_hardware_cursor — 更新 VGA 硬件光标位置
 *
 * VGA 控制器有一个硬件光标（闪烁的下划线）
 * 通过写 CRTC (CRT Controller) 寄存器控制：
 *   端口 0x3D4: CRTC 地址寄存器（选择寄存器）
 *   端口 0x3D5: CRTC 数据寄存器（读写数据）
 *   寄存器 14 (0x0E): 光标高字节
 *   寄存器 15 (0x0F): 光标低字节
 *
 * 【修复】使用 arch/io.h 的 outb()，不再写裸内联汇编
 * --------------------------------------------------------------- */
static void update_hardware_cursor(void) {
    u16 pos = (u16)cursor_pos;

    /* 选择 CRTC 寄存器 14（光标位置高字节），然后写入 */
    outb(0x3D4, 0x0E);
    outb(0x3D5, (u8)(pos >> 8));

    /* 选择 CRTC 寄存器 15（光标位置低字节），然后写入 */
    outb(0x3D4, 0x0F);
    outb(0x3D5, (u8)(pos & 0xFF));
}

/* ---------------------------------------------------------------
 * arch_console_init — 初始化早期控制台
 *
 * x86_64 VGA 实现：清屏 + 光标归零
 * VGA 文本模式不需要硬件初始化（PC 开机默认就是）
 * --------------------------------------------------------------- */
void arch_console_init(void) {
    int i;
    /* 清屏：每个位置填空格 */
    for (i = 0; i < VGA_SIZE; i++) {
        VGA[i] = (0x07 << 8) | ' ';
    }

    cursor_pos = 0;
    update_hardware_cursor();

    /* 初始化串口（同时输出到 COM1，方便无 VGA 环境调试）*/
    serial_init();
}

/* ---------------------------------------------------------------
 * arch_console_putchar — 输出一个字符
 *
 * 特殊字符处理：
 *   '\n' (0x0A) — 换行：光标移到下一行开头
 *   '\r' (0x0D) — 回车：光标移到当前行开头
 *   '\t' (0x09) — 制表符：对齐到下一个 8 列边界
 *   '\b' (0x08) — 退格：光标左移一格（不删除字符）
 *
 * 屏幕溢出：光标超出底行时，自动 scroll_up()
 *
 * 【修复】新增 \t 和 \b 处理
 * --------------------------------------------------------------- */
void arch_console_putchar(char c) {
    /* 同时输出到串口（不影响 VGA 逻辑，serial_putc 内部处理 \r\n）*/
    serial_putc(c);

    /* 处理换行 */
    if (c == '\n') {
        /* 移到下一行开头
         * 例: 光标在 83 (第1行第3列)
         *   83 / 80 = 1, (1+1)*80 = 160
         *   光标移到 160 (第2行第0列)
         */
        cursor_pos = (cursor_pos / VGA_COLS + 1) * VGA_COLS;

        if (cursor_pos >= VGA_SIZE) {
            scroll_up();
        }
        update_hardware_cursor();
        return;
    }

    /* 处理回车 */
    if (c == '\r') {
        cursor_pos = (cursor_pos / VGA_COLS) * VGA_COLS;
        update_hardware_cursor();
        return;
    }

    /* 处理制表符
     * 对齐到下一个 TAB_WIDTH 的倍数列
     * 例: 当前在第 3 列 -> 对齐到第 8 列（前进 5 格）
     *     当前在第 8 列 -> 对齐到第 16 列（前进 8 格） */
    if (c == '\t') {
        int col = cursor_pos % VGA_COLS;
        int next_tab = (col / TAB_WIDTH + 1) * TAB_WIDTH;
        int advance = next_tab - col;

        /* 逐格前进，每个位置写一个空格（保持背景色） */
        int i;
        for (i = 0; i < advance; i++) {
            if (cursor_pos >= VGA_SIZE) {
                scroll_up();
            }
            VGA[cursor_pos] = (current_color << 8) | ' ';
            cursor_pos++;
        }
        update_hardware_cursor();
        return;
    }

    /* 处理退格
     * 光标左移一格，并用空格覆盖原字符（视觉上"删除"）
     * 如果已在行首，不退到上一行（简化处理） */
    if (c == '\b') {
        if (cursor_pos > 0 && (cursor_pos % VGA_COLS) != 0) {
            cursor_pos--;
            VGA[cursor_pos] = (current_color << 8) | ' ';
            update_hardware_cursor();
        }
        return;
    }

    /* 屏幕溢出检查 */
    if (cursor_pos >= VGA_SIZE) {
        scroll_up();
    }

    /* 写入字符
     * (current_color << 8) | c:
     *   高字节 = 颜色属性
     *   低字节 = ASCII 字符
     */
    VGA[cursor_pos] = (current_color << 8) | (unsigned char)c;
    cursor_pos++;

    update_hardware_cursor();
}

/* ---------------------------------------------------------------
 * arch_console_print — 输出字符串
 * 逐字符调用 arch_console_putchar，遇 '\0' 停止
 * --------------------------------------------------------------- */
void arch_console_print(const char *s) {
    while (*s) {
        arch_console_putchar(*s);
        s++;
    }
}

/* ---------------------------------------------------------------
 * arch_console_set_color — 设置输出颜色
 * --------------------------------------------------------------- */
void arch_console_set_color(int color) {
    if (color >= 0 &&
        color < (int)(sizeof(color_map) / sizeof(color_map[0]))) {
        current_color = color_map[color];
    } else {
        current_color = 0x07;
    }
}
