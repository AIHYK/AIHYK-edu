/* ================================================================
 * user_image.S — 把用户程序二进制嵌入内核镜像
 *
 * 【Lesson 8 核心新增】
 *
 *   用户程序（user/hello.asm）用 nasm -f bin 组装成 flat binary，
 *   然后通过 .incbin 指令把这段二进制嵌入内核的 .rodata 段。
 *
 *   内核运行时用符号 user_hello_bin / user_hello_bin_end 取出这段
 *   二进制，调 sched_create_user_task(user_hello_bin, len, ...) 加载。
 *
 * 【Lesson 9 新增】
 *
 *   增加第二个用户程序 user/crash.asm（故意崩溃）。
 *   crash.bin 页对齐（.balign 4096）嵌入，避免和 hello.bin 共享同一页。
 *
 * 【为什么用 .incbin 而不是生成 .h 数组】
 *   - .incbin 直接在链接时把文件字节嵌入 ELF，不需要 xxd/od 转换
 *   - 符号 user_hello_bin 是普通 C 可访问的地址，类型 const u8[]
 *   - 修改 hello.asm 后只需重新 nasm + 链接，无需重新生成 .h
 *
 * 【为什么是 .S（大写）不是 .s】
 *   - .S 让 gcc 走 C 预处理（处理 # 等指令），.s 不预处理
 *   - 这里没用到预处理，但 .S 习惯更安全（可加 #include）
 *
 * 【Makefile 依赖】
 *   kernel.elf 依赖 user/hello.bin 和 user/crash.bin（.incbin 找不到文件会链接失败）
 *   Makefile 用 order-only prerequisite 保证 bin 文件先生成。
 * ================================================================ */

    .section .rodata

    .global user_hello_bin
    .global user_hello_bin_end

user_hello_bin:
    .incbin "user/hello.bin"

user_hello_bin_end:
    /* 长度 = user_hello_bin_end - user_hello_bin（C 代码算） */

    /* 【Lesson 9】crash.bin 页对齐嵌入
     *   .balign 4096 确保 crash.bin 的起始在页边界上，
     *   避免和 hello.bin 共享同一页（虽然当前每个 user task
     *   有独立 VMA 槽位，对齐更多是整洁性要求）。 */
    .balign 4096

    .global user_crash_bin
    .global user_crash_bin_end

user_crash_bin:
    .incbin "user/crash.bin"

user_crash_bin_end:
    /* 长度 = user_crash_bin_end - user_crash_bin（C 代码算） */

    /* 显式声明栈段不可执行 —— 消除 ld 的
     *   "missing .note.GNU-stack section implies executable stack" 警告。
     *   所有 .asm（nasm）文件都用 `section .note.GNU-stack noalloc noexec nowrite progbits`
     *   声明了；这里是 GAS 语法等价物。空属性 "" = progbits 无 flags，
     *   等价于 noalloc noexec nowrite progbits。 */
    .section .note.GNU-stack,"",@progbits
