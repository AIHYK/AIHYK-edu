/* ================================================================
 * arch/x86_64/keyboard.c — PS/2 键盘驱动 + COM1 串口输入
 *
 * 【Lesson 3 核心新增】
 * 【Lesson 6 修复】增加 COM1 串口输入处理，修复"按按键没回显"问题
 *
 * PS/2 键盘原理：
 *   PC 上有一个 8042 键盘控制器（集成在 Super I/O 芯片里），
 *   用户的每次按键/松键都会发一个 scancode 给键盘控制器，
 *   控制器通过 IRQ1 中断通知 CPU。
 *
 *   CPU 收到 IRQ1 → 读端口 0x60 → 得到 scancode → 解码 → 显示字符
 *
 * PS/2 端口：
 *   0x60 - 数据端口（读 scancode / 写命令）
 *   0x64 - 状态/命令端口
 *
 *   状态寄存器（读 0x64）：
 *     bit 0 = OBF (Output Buffer Full): 1=有数据可读
 *     bit 1 = IBF (Input Buffer Full): 1=输入缓冲满（不可写）
 *
 * Scancode Set 1（默认）：
 *   按下键 → 控制器发"make code"（0x01~0x7F，与按键一一对应）
 *   松开键 → 控制器发"break code"（0xF0 + make code）
 *
 *   常见 scancode → ASCII（US 键盘布局）：
 *     0x1E = 'a'   0x1F = 's'   0x20 = 'd'   0x21 = 'f'
 *     0x2C = 'z'   0x2D = 'x'   0x2E = 'c'   0x2F = 'v'
 *     0x0E = '1'   0x1E = '2'   ... 不对，1 是 0x02
 *     0x02 = '1'   0x03 = '2'   ...
 *     0x1C = Enter 0x39 = Space 0x0E = Backspace 0x01 = Esc
 *     0x48 = Up    0x50 = Down  0x4B = Left   0x4D = Right
 *
 *   Shift 状态：
 *     0x2A = Left Shift make
 *     0xAA = Left Shift break
 *     0x36 = Right Shift make
 *     0xB6 = Right Shift break
 *
 *   Caps Lock:
 *     0x3A = CapsLock make（toggle，不会发 break）
 *
 * 【本驱动实现范围】
 *   - 支持 Scancode Set 1（PC 默认）
 *   - 支持字母+数字+常用符号
 *   - 支持 Shift（大小写转换）
 *   - 支持 Caps Lock
 *   - 支持方向键、Backspace、Enter
 *   - 不支持 Alt、Ctrl、功能键（F1~F12）的复杂组合
 *
 *   教学 driver，简单可用即可。
 *
 * ================================================================
 * 【Lesson 6 修复：COM1 串口输入】
 *
 *   问题描述：用户运行 QEMU 时常用 `-serial stdio` 把内核日志重定向
 *   到终端方便看。但用户在终端里敲键时，这些按键【不会】进 PS/2 键盘
 *   （IRQ1），而是被 QEMU 当作 COM1 串口输入发给内核。
 *   原驱动只读 PS/2 键盘（IRQ1），不读 COM1（IRQ4），
 *   导致"按按键没回显"——按键全部进了 COM1 的 FIFO 但没人读。
 *
 *   两条输入路径对比：
 *
 *     ┌─────────────────────────┬──────────────┬──────────────┐
 *     │ QEMU 启动方式            │ 按键去向      │ 处理的 IRQ   │
 *     ├─────────────────────────┼──────────────┼──────────────┤
 *     │ qemu ... -kernel x.elf  │ VGA 窗口按键  │ IRQ1 (PS/2)  │
 *     │ qemu ... -serial stdio │ 终端按键      │ IRQ4 (COM1)  │
 *     │ qemu ... -nographic    │ 终端按键      │ IRQ4 (COM1)  │
 *     └─────────────────────────┴──────────────┴──────────────┘
 *
 *   修复方案：在 arch_keyboard_init 里同时注册 IRQ4 handler，
 *   读 COM1 收到的字节并回显。这样无论用户用哪种方式运行 QEMU，
 *   按键都能被回显。
 *
 *   COM1 UART（16550）寄存器（与 console.c 共享，这里重定义避免循环依赖）：
 *     0x3F8 - RBR/THR  数据寄存器（读=接收字节，写=发送字节）
 *     0x3F9 - IER      中断使能寄存器
 *              bit 0 = ER  (Enable Received Data Interrupt)
 *     0x3FD - LSR      线状态寄存器
 *              bit 0 = DR  (Data Ready, 有字节可读)
 *              bit 1 = OE  (Overrun Error, FIFO 溢出)
 *
 *   开启 COM1 输入中断的步骤：
 *     1. IER |= 0x01  （开接收中断）
 *     2. PIC unmask IRQ4 （让 CPU 收到 COM1 的中断）
 *     3. 注册 IRQ4 callback
 *   注意：console.c 的 serial_init 把 IER 设成 0（禁用中断），
 *   arch_keyboard_init 在 serial_init 之后调用，覆盖 IER=0x01 即可。
 * ================================================================ */

#include <arch/console.h>
#include <arch/interrupts.h>
#include <arch/io.h>
#include <arch/irq.h>
#include <arch/pic.h>
#include <kernel/types.h>

/* PS/2 端口 */
#define KB_DATA    0x60    /* 数据端口（读 scancode / 写命令）*/
#define KB_STATUS  0x64    /* 状态端口 */
#define KB_COMMAND 0x64    /* 命令端口 */

/* 状态寄存器位 */
#define KB_STATUS_OBF 0x01   /* Output Buffer Full - 有数据可读 */

/* ---------------------------------------------------------------
 * COM1 串口端口（与 console.c 共享，这里重定义避免头文件循环依赖）
 *
 *   为什么不抽到共享头文件：教学内核追求文件少、依赖清晰，
 *   重复两组 #define 比加一个 arch/serial.h 更直观。
 * --------------------------------------------------------------- */
#define COM1_DATA  0x3F8   /* RBR（读）/ THR（写） */
#define COM1_IER   0x3F9   /* 中断使能寄存器 */
#define COM1_LSR   0x3FD   /* 线状态寄存器 */

/* LSR 位定义 */
#define LSR_DR     0x01    /* Data Ready - 接收缓冲区有字节可读 */
#define LSR_OE     0x02    /* Overrun Error - FIFO 溢出（数据丢失）*/

/* IER 位定义 */
#define IER_ER     0x01    /* Enable Received Data Interrupt */

/* 特殊 scancode（Set 1）*/
#define SCAN_RELEASE     0xF0   /* break code 前缀（松键时发） */
#define SCAN_LSHIFT_MAKE 0x2A   /* Left Shift 按下 */
#define SCAN_LSHIFT_BREAK 0xAA  /* Left Shift 松开 */
#define SCAN_RSHIFT_MAKE 0x36   /* Right Shift 按下 */
#define SCAN_RSHIFT_BREAK 0xB6  /* Right Shift 松开 */
#define SCAN_CAPSLOCK    0x3A   /* CapsLock 按（toggle） */
#define SCAN_EXTENDED    0xE0   /* 扩展键前缀（方向键等）*/

/* 控制字符的 scancode */
#define SCAN_ENTER       0x1C
#define SCAN_BACKSPACE   0x0E
#define SCAN_SPACE      0x39
#define SCAN_TAB        0x0F
#define SCAN_ESC        0x01

/* ---------------------------------------------------------------
 * 键盘状态
 *
 *   shift_pressed - Shift 是否按下（任意一个）
 *   caps_lock     - CapsLock 是否开启（toggle）
 *   extended_mode - 上一个字节是否是 0xE0（扩展键前缀）
 *
 * 用 static 全局变量保存状态，因为每次 IRQ1 只来 1 个字节，
 * 多字节序列（如 0xE0 + 0x48 = Up）需要跨中断记忆。
 * --------------------------------------------------------------- */
static bool shift_pressed = false;
static bool caps_lock     = false;
static bool extended_mode = false;

/* ---------------------------------------------------------------
 * Scancode → ASCII 映射表（US 键盘，Set 1）
 *
 *   scan_to_ascii_lower[]: 未按 Shift 时的字符
 *   scan_to_ascii_upper[]: 按 Shift 时的字符
 *
 *   例如 SCAN 'a' = 0x1E：
 *     scan_to_ascii_lower[0x1E] = 'a'
 *     scan_to_ascii_upper[0x1E] = 'A'
 *
 *   不支持的 scancode 用 0 占位（dispatch 时跳过）
 *
 * Set 1 scancode 范围：0x01~0x7F（128 项）
 * --------------------------------------------------------------- */
static const u8 scan_to_ascii_lower[128] = {
    /* 0x00 */  0,    27,   '1',  '2',  '3',  '4',  '5',  '6',
    /* 0x08 */  '7',  '8',  '9',  '0',  '-',  '=',  '\b', '\t',
    /* 0x10 */  'q',  'w',  'e',  'r',  't',  'y',  'u',  'i',
    /* 0x18 */  'o',  'p',  '[',  ']',  '\n', 0,    'a',  's',
    /* 0x20 */  'd',  'f',  'g',  'h',  'j',  'k',  'l',  ';',
    /* 0x28 */  '\'', '`',  0,    '\\', 'z',  'x',  'c',  'v',
    /* 0x30 */  'b',  'n',  'm',  ',',  '.',  '/',  0,    '*',
    /* 0x38 */  0,    ' ',  0,    0,    0,    0,    0,    0,
    /* 0x40 */  0,    0,    0,    0,    0,    0,    0,    '7',
    /* 0x48 */  '8',  '9',  '-',  '4',  '5',  '6',  '+',  '1',
    /* 0x50 */  '2',  '3',  '0',  '.',  0,    0,    0,    0,
    /* 0x58 */  0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x60 */  0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x68 */  0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x70 */  0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x78 */  0,    0,    0,    0,    0,    0,    0,    0,
};

static const u8 scan_to_ascii_upper[128] = {
    /* 0x00 */  0,    27,   '!',  '@',  '#',  '$',  '%',  '^',
    /* 0x08 */  '&',  '*',  '(',  ')',  '_',  '+',  '\b', '\t',
    /* 0x10 */  'Q',  'W',  'E',  'R',  'T',  'Y',  'U',  'I',
    /* 0x18 */  'O',  'P',  '{',  '}',  '\n', 0,    'A',  'S',
    /* 0x20 */  'D',  'F',  'G',  'H',  'J',  'K',  'L',  ':',
    /* 0x28 */  '"',  '~',  0,    '|',  'Z',  'X',  'C',  'V',
    /* 0x30 */  'B',  'N',  'M',  '<',  '>',  '?',  0,    '*',
    /* 0x38 */  0,    ' ',  0,    0,    0,    0,    0,    0,
    /* 0x40 */  0,    0,    0,    0,    0,    0,    0,    '7',
    /* 0x48 */  '8',  '9',  '-',  '4',  '5',  '6',  '+',  '1',
    /* 0x50 */  '2',  '3',  '0',  '.',  0,    0,    0,    0,
    /* 0x58 */  0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x60 */  0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x68 */  0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x70 */  0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x78 */  0,    0,    0,    0,    0,    0,    0,    0,
};

/* ---------------------------------------------------------------
 * kb_read_scancode - 从键盘读 scancode
 *
 * 流程：
 *   1. 等 OBF=1（输出缓冲满，可读）
 *   2. 读 0x60 端口
 *
 * 在中断处理程序里调，OBF 通常已经置位（中断就是因为数据来了）。
 * 但保险起见仍检查一次。
 * --------------------------------------------------------------- */
static u8 kb_read_scancode(void) {
    /* 等数据准备好 */
    while ((inb(KB_STATUS) & KB_STATUS_OBF) == 0) {
        /* spin */
    }
    return inb(KB_DATA);
}

/* ---------------------------------------------------------------
 * kb_should_uppercase - 是否应该大写字母
 *
 *   - Shift 按下：大写
 *   - CapsLock 开启：大写
 *   - 同时按 Shift + CapsLock：小写（互相抵消）
 *
 *  这个互斥逻辑符合人类直觉。
 * --------------------------------------------------------------- */
static bool kb_should_uppercase(void) {
    bool sh = shift_pressed;
    bool cl = caps_lock;
    return sh ^ cl;     /* 异或：两者状态不同时大写 */
}

/* ---------------------------------------------------------------
 * keyboard_handler - 键盘 IRQ1 中断处理程序
 *
 * 每次 IRQ1 调用一次，处理一个 scancode。
 *
 * 流程：
 *   1. 读 scancode
 *   2. 处理特殊键（Shift、CapsLock、扩展前缀、松键前缀）
 *   3. 普通键：查映射表 → 转换大小写 → 显示
 *
 * Scancode 处理顺序：
 *   - 0xE0 (extended): 设 extended_mode，下一个字节是扩展键
 *     （我们简化处理，扩展键暂时不映射）
 *   - 0xF0 (break): 设 break_mode，下一个字节是松键的 scancode
 *     （松键时除 Shift 外，一般不做动作）
 *   - Shift make/break: 修改 shift_pressed 状态
 *   - CapsLock: toggle caps_lock
 *   - 普通键 make: 查表显示
 *
 * 【为什么 break code 单独处理】
 *   Set 1 的 break 是两字节：0xF0 + scancode。
 *   如果直接按 scancode 处理，会把 0xF0 当成"键 0xF0"，
 *   然后把真实 scancode 当成"另一个键"。错乱。
 *   所以设个状态位，遇到 0xF0 后下一个字节当 break 处理。
 * --------------------------------------------------------------- */
static bool break_mode = false;     /* 上一个是 0xF0（松键前缀） */

static void keyboard_handler(struct interrupt_frame *frame) {
    (void)frame;

    u8 scancode = kb_read_scancode();

    /* --- 扩展键前缀 0xE0 ---
     * 方向键等扩展键发 0xE0 + scancode，
     * 设 extended_mode 后下一个字节是真实 scancode。
     * 简化处理：直接跳过这两个字节（不映射扩展键） */
    if (scancode == SCAN_EXTENDED) {
        extended_mode = true;
        return;
    }

    /* --- 松键前缀 0xF0 ---
     * 下一个字节是松键的 scancode */
    if (scancode == SCAN_RELEASE) {
        break_mode = true;
        return;
    }

    /* --- Shift 按下/松开 ---
     * 修改 shift_pressed 状态 */
    if (scancode == SCAN_LSHIFT_MAKE || scancode == SCAN_RSHIFT_MAKE) {
        shift_pressed = true;
        extended_mode = false;
        break_mode = false;
        return;
    }
    if (scancode == SCAN_LSHIFT_BREAK || scancode == SCAN_RSHIFT_BREAK) {
        shift_pressed = false;
        extended_mode = false;
        break_mode = false;
        return;
    }

    /* --- CapsLock（toggle，make/break 都切换一次？不！只在 make 时切换）
     *
     * 实际键盘行为：CapsLock 按【一次】触发一次（toggle）。
     * Set 1 scancode：
     *   按 = 0x3A (make)
     *   松 = 0xF0 + 0x3A (break)
     *
     * 我们在 make 时 toggle（即按 0x3A 时），break 时不动。
     * 防止 break 也 toggle 一次（导致按一次等于按两次）。
     * --------------------------------------------------------- */
    if (scancode == SCAN_CAPSLOCK) {
        if (!break_mode) {
            caps_lock = !caps_lock;
        }
        extended_mode = false;
        break_mode = false;
        return;
    }

    /* --- 松键处理（除 Shift 外一般不做事）---
     * 重置 break_mode，等下次 make */
    if (break_mode) {
        break_mode = false;
        extended_mode = false;
        return;
    }

    /* --- 普通键 make ---
     * 查映射表 → 转大小写 → 显示 */

    /* 扩展键（如方向键）暂时不处理，跳过 */
    if (extended_mode) {
        extended_mode = false;
        return;
    }

    if (scancode >= 128) {
        /* 越界，忽略 */
        return;
    }

    /* 查 ASCII 表 */
    u8 ascii;
    if (kb_should_uppercase()) {
        ascii = scan_to_ascii_upper[scancode];
    } else {
        ascii = scan_to_ascii_lower[scancode];
    }

    /* 不支持的键返回 0，跳过 */
    if (ascii == 0) {
        return;
    }

    /* 显示字符（ESC 特殊：清除屏幕？暂不处理）*/
    if (ascii == 27) {
        /* ESC：打印 [ESC] 让用户看到 */
        arch_console_set_color(CON_COLOR_CYAN);
        arch_console_print("[ESC]");
        arch_console_set_color(CON_COLOR_DEFAULT);
    } else {
        /* 普通字符：输出到屏幕（带颜色突出）*/
        arch_console_set_color(CON_COLOR_YELLOW);
        arch_console_putchar((char)ascii);
        arch_console_set_color(CON_COLOR_DEFAULT);
    }
}

/* ---------------------------------------------------------------
 * serial_input_handler - COM1 串口输入中断处理（IRQ4）
 *
 * 【为什么需要这个 handler】
 *   见文件头注释：用户用 `-serial stdio` 运行 QEMU 时，
 *   在终端里敲的键不会进 PS/2 键盘（IRQ1），而是进 COM1（IRQ4）。
 *   不读 COM1 的话按键就"消失"了——表现就是"按按键没回显"。
 *
 * 流程：
 *   1. 读 LSR，检查 DR 位（Data Ready）
 *   2. DR=1 时读 RBR 拿到一个字节
 *   3. 把字节回显到屏幕（arch_console_putchar）
 *   4. 循环直到 DR=0（清空 UART 的 FIFO，一次 IRQ 可能积了多字节）
 *
 * 【为什么循环读直到 DR=0】
 *   16550 UART 有 16 字节 FIFO。如果用户快速敲了多个键，
 *   FIFO 里可能积了多个字节，但 CPU 只收到一个 IRQ4。
 *   只读一个字节就返回的话，剩余字节要等下次按键触发新 IRQ 才能读——
 *   但 UART 在 FIFO 非空时不会再次触发 IRQ（边沿触发，只在 DR 0→1 时触发）。
 *   所以必须一次 IRQ 把 FIFO 读空，否则后续字节永远读不到。
 *
 * 【为什么把 \r 翻译成 \n】
 *   终端按 Enter 键发送 \r（0x0D），但内核换行用 \n（0x0A）。
 *   不翻译的话，arch_console_putchar 把 \r 当回车处理——
 *   光标回到行首但不换行，后续输出会覆盖当前行内容。
 *   翻译成 \n 后，光标移到下一行开头，符合用户预期。
 *
 * 【为什么过滤不可打印字符】
 *   终端可能发送一些控制字符（如 Ctrl-C 的 0x03、Ctrl-D 的 0x04），
 *   这些字符没有可视字形，直接 arch_console_putchar 会显示乱码或空格。
 *   只回显可打印 ASCII（0x20~0x7E）+ 常见编辑控制字符（\n \b \t）。
 * --------------------------------------------------------------- */
static void serial_input_handler(struct interrupt_frame *frame) {
    (void)frame;

    /* 循环读出 COM1 FIFO 里的所有字节
     *（DR 位 = 1 表示还有字节可读，详见上方注释）*/
    int max_reads = 16;
    while ((inb(COM1_LSR) & LSR_DR) != 0) {
        u8 c = inb(COM1_DATA);

        /* \r → \n（终端 Enter 键发 \r，内核换行用 \n）*/
        if (c == '\r') {
            c = '\n';
        }

        /* 只回显可打印字符 + 常见编辑控制字符 */
        if ((c >= 0x20 && c <= 0x7E) || c == '\n' || c == '\b' || c == '\t') {
            /* 用黄色突出用户输入，和 PS/2 键盘的回显颜色一致 */
            arch_console_set_color(CON_COLOR_YELLOW);
            arch_console_putchar((char)c);
            arch_console_set_color(CON_COLOR_DEFAULT);
        }
        /* 其他控制字符（0x00~0x1F 除 \n \b \t，以及 0x7F DEL）忽略 */
        if (--max_reads <= 0) {
            break;
        }
    }
}

/* ---------------------------------------------------------------
 * arch_keyboard_init - 初始化 PS/2 键盘 + COM1 串口输入
 *
 * 流程：
 *   PS/2 键盘（IRQ1）：
 *     1. 注册 IRQ1 callback（keyboard_handler）
 *     2. 取消屏蔽 IRQ1
 *
 *   COM1 串口输入（IRQ4）【Lesson 6 修复新增】：
 *     3. 开启 UART 接收中断（IER bit 0 = 1）
 *     4. 注册 IRQ4 callback（serial_input_handler）
 *     5. 取消屏蔽 IRQ4
 *
 * 之后两条输入路径都能用：
 *   - QEMU VGA 窗口按键 → IRQ1 → keyboard_handler → 显示
 *   - -serial stdio 终端按键 → IRQ4 → serial_input_handler → 显示
 *
 * 【为什么不清空键盘缓冲区】
 *   PS/2 键盘控制器有自己的缓冲区，但只要中断及时处理就不会溢出。
 *   初始化时 IRQ1 还是屏蔽的，缓冲区里有按键也无所谓。
 *
 * 【为什么取消屏蔽 IRQ1/IRQ4 放这里】
 *   必须在 callback 注册后才取消屏蔽，
 *   否则按键来了没人处理 → 触发 default handler（一直警告）。
 *   所以 init 顺序：先 register，再 unmask。
 *
 * 【为什么 IER 在这里写而不是在 console.c 的 serial_init 里】
 *   console.c 的 serial_init 把 IER 设成 0（禁用所有中断），
 *   那是输出初始化阶段，不想被输入中断打扰。
 *   arch_keyboard_init 在 serial_init 之后调用，这里开 IER bit 0
 *   覆盖掉之前的 0，让接收中断在所有初始化完成后才启用。
 *   顺序：console_init（IER=0）→ ... → keyboard_init（IER=0x01）→ sti。
 * --------------------------------------------------------------- */
void arch_keyboard_init(void) {
    /* === PS/2 键盘（IRQ1）===
     * 处理 QEMU VGA 窗口里的按键 */
    arch_irq_register(1, keyboard_handler);
    arch_pic_unmask(1);   /* 允许 IRQ1 */

    /* === COM1 串口输入（IRQ4）===
     * 处理 -serial stdio / -nographic 终端里的按键 */
    outb(COM1_IER, IER_ER);
    arch_irq_register(4, serial_input_handler);
    arch_pic_unmask(4);
}
