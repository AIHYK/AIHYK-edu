/* ================================================================
 * linker.ld — 链接脚本（Lesson 4 增补版）
 *
 * 告诉链接器：
 *   - 入口点是 _start
 *   - 代码从 1MB (0x100000) 开始放
 *     （1MB 以下被 BIOS/VGA 占了）
 *   - multiboot 头必须最先
 *
 * 段 (section) 是 ELF 文件的基本单位：
 *   .text   = 代码（可执行指令）
 *   .rodata = 只读数据（字符串常量、const 全局变量）
 *   .data   = 已初始化的可变数据（全局变量）
 *   .bss    = 未初始化的数据（启动时被清零，不占文件空间）
 *
 * 【Lesson 4 变更】
 *   新增两个全局符号，给物理内存管理器（PMM）用：
 *
 *     __kernel_start   - 内核镜像在内存中的起始地址（= 1MB）
 *     __kernel_end     - 内核镜像结束地址（.bss 段末尾）
 *
 *   PMM 在初始化时会把 [__kernel_start, __kernel_end) 区间
 *   标记为"已用"，防止后续分配覆盖内核代码/数据。
 *
 *   注意：链接脚本里定义的符号【没有存储】，只是个地址标签。
 *   C 代码用 `extern u8 __kernel_end;` 取地址，
 *   然后 `(u64)&__kernel_end` 得到地址值。
 * ================================================================ */

ENTRY(_start)

SECTIONS
{
    /* 起始地址 = 1MB (0x100000)
     *
     * x86 PC 的低 1MB 内存布局：
     *   0x00000 - 0x003FF  中断向量表 (IVT, 1KB)
     *   0x00400 - 0x004FF  BIOS 数据区 (BDA, 256B)
     *   0x07C00 - 0x07DFF  bootloader (512B)
     *   0xA0000 - 0xBFFFF  VGA 显存 (128KB)
     *   0xC0000 - 0xFFFFF  BIOS ROM (256KB)
     *
     * 1MB 以上 = 安全区，可以随意使用
     * Multiboot 规范默认把内核加载到 1MB
     */
    . = 1M;

    /* __kernel_start = 当前位置（1MB）
     * PMM 会保留 [__kernel_start, __kernel_end) 范围 */
    __kernel_start = .;

    /* .text 段：代码
     * BLOCK(4096)：对齐到 4096B 边界（一个页的大小）
     * *(.multiboot) 必须最先：GRUB 要求头在前 8kb 内
     */
    .text ALIGN(4096) :
    {
        *(.multiboot)
        *(.text)
    }

    /* .note 段：ELF Note（PVH 启动协议用）
     *
     * 【为什么单独放一个 output section】
     *   ELF 链接器默认把 .note.* section 归到 .note output section，
     *   并自动标记为 PT_NOTE 类型 program header。
     *   QEMU 加载内核时扫描 PT_NOTE，找 name="Xen" type=0x18 的 note。
     *
     *   如果不显式声明 .note output section，链接器会把 .note.pvh
     *   当作 orphan section（孤儿段），可能不放 PT_NOTE，导致 QEMU 找不到。
     *
     * ALIGN(4)：ELF Note 必须 4 字节对齐
     */
    .note ALIGN(4) :
    {
        *(.note.pvh)
        *(.note.*)
    }

    /* .rodata 段：只读数据 */
    .rodata ALIGN(4096) :
    {
        *(.rodata)
        *(.rodata.*)
    }

    /* .data 段：已初始化数据 */
    .data ALIGN(4096) :
    {
        *(.data)
        *(.data.*)
    }

    /* .bss 段：未初始化数据
     * 不占文件空间，但占内存空间
     * 我们的栈（16KB + 64KB）在 .bss 里
     *
     * ALIGN(4096) 对齐到页边界，让 __kernel_end 也是页对齐的，
     * 这样 PMM 把 [__kernel_start, __kernel_end) 一次性标记成"已用"，
     * 不会出现跨页的边界情况。 */
    .bss ALIGN(4096) :
    {
        *(COMMON)
        *(.bss)
        *(.bss.*)
    }

    /* __kernel_end = 当前位置（.bss 末尾，已 4KB 对齐）
     *
     * PMM 初始化时会调用 arch_pmm_reserve_region(__kernel_start, __kernel_end)
     * 把这段标记为"已用"，避免后续 alloc_frame 返回内核正在用的页。
     *
     * 同时 kernel/mm.c 会从 __kernel_end 开始往后开辟内核堆
     * （分配若干帧并 map 到 __kernel_end 之后的虚拟地址）。 */
    . = ALIGN(4096);
    __kernel_end = .;

    /* 调试符号（不占运行时内存）
     *
     * 这些符号被 GCC -g 生成，仅用于 GDB 调试。
     * 我们显式丢弃，避免 ELF 文件膨胀。 */
    /DISCARD/ : { *(.comment) *(.note.GNU-stack) }
}
