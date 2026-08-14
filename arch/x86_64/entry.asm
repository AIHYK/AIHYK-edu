; ================================================================
; entry.asm — x86-64 内核启动入口（32 位 → 64 位长模式切换）
;
; 这是整个内核执行的第一段代码。
; 支持两种启动方式（自动检测，同一入口 _start 兼容）：
;
;   A. GRUB + multiboot2：ISO 启动（make run / make iso）
;      GRUB 在 32 位保护模式下加载内核，跳到 _start
;        eax = 0x36d76289 (MULTIBOOT2_BOOTLOADER_MAGIC)
;        ebx = multiboot2 info 结构物理地址
;
;   B. QEMU PVH 直接加载：qemu-system-x86_64 -kernel kernel.elf
;      QEMU 通过 .note.pvh ELF Note 识别内核，用 PVH 协议启动
;        eax = 0x336ec578 (PVH magic)
;        ebx = hvm_start_info 结构物理地址
;
; 两种启动方式的关键差异（必须在 _start 里统一处理）：
;   ┌──────────┬─────────────────────────┬────────────────────────────┐
;   │ 寄存器   │ GRUB (multiboot2)       │ QEMU PVH                   │
;   ├──────────┼─────────────────────────┼────────────────────────────┤
;   │ CR0      │ PE=1, PG=0（分页未开）  │ PE=1, PG=1（分页已开）     │
;   │ CR4      │ PAE=0                   │ PAE=1                       │
;   │ EFER     │ 0                       │ LME=0, LMA=0（未进长模式） │
;   │ GDT      │ GRUB 设的 32 位 GDT     │ QEMU 设的 32 位 GDT        │
;   │ CS       │ GRUB 的 32 位 code 段   │ 0x10（QEMU 设的 32 位段）  │
;   └──────────┴─────────────────────────┴────────────────────────────┘
;
;   关键约束：
;     - x86 规范：EFER.LME 只能在 CR0.PG=0 时修改，否则触发 #GP
;       → PVH 启动时 PG=1，必须先关分页才能设 EFER.LME
;     - 长模式切换需要 64 位代码段（L bit=1）
;       → 两种 bootloader 的 GDT 都没有 64 位段，必须重设 GDT
;     - 重设 GDT 后 CS 仍指向旧 GDT，必须远跳转重新加载 CS
;       → 远跳的目标段必须存在（32 位段，因为还在 32 位模式）
;
; ┌─────────────────────────────────────────────────────────────┐
; │  统一启动流程（兼容 PVH 和 multiboot2）                      │
; ├─────────────────────────────────────────────────────────────┤
; │  1. 保存 eax/ebx（启动 magic + info 地址）                  │
; │  2. 设置 32 位栈                                            │
; │  3. lgdt 加载我们的 GDT（含 32 位 + 64 位段）              │
; │  4. 远跳转到 32 位代码段（重新加载 CS = 我们的 32 位段）   │
; │  5. 加载 DS/ES/FS/GS/SS = 我们的数据段                     │
; │  6. 加载 CR3 = 我们的 PML4 页表                            │
; │  7. 关闭分页 (CR0.PG=0) ← PVH 必需，multiboot2 无影响     │
; │  8. 启用 PAE (CR4.PAE=1)                                  │
; │  9. 设置 EFER.LME=1（PG=0 时才能设）                       │
; │ 10. 启用分页 (CR0.PG=1) → 进入兼容模式                     │
; │ 11. 远跳转到 64 位代码段 → 正式进入 64 位长模式            │
; └─────────────────────────────────────────────────────────────┘
;
; 为什么 32 位和 64 位代码能在同一文件？
;   nasm 用 [bits 32] / [bits 64] 切换指令编码模式。
;   整个文件用 elf64 格式编译（nasm -f elf64），但指令可以是 32 位。
;   链接器（ld -m elf_x86_64）只关心符号和重定位，不关心指令位宽。
;
; 为什么 32 位代码不能调用 64 位 C 函数？
;   - C 函数用 -m64 编译，调用约定是 64 位（参数在 rdi/rsi/...，
;     不是栈上）
;   - 32 位代码的 call 指令用 32 位调用约定（参数在栈上）
;   - 32 位下 call 一个 64 位函数会乱套
;   - 所以切换到 64 位后才能 call kernel_main
; ================================================================

; ---------------------------------------------------------------
; 声明外部符号
; ---------------------------------------------------------------
extern kernel_main          ; kernel/main.c 里的 64 位 C 函数
extern x86_64_boot_eax        ; arch/x86_64/boot.c，保存启动 magic
extern x86_64_boot_ebx        ; arch/x86_64/boot.c，保存启动 info 地址

; ---------------------------------------------------------------
; GDT 段选择子（用于远跳转和段寄存器加载）
;
; 段选择子格式：index(13位) | TI(1位) | RPL(2位)
;   index = 在 GDT 中的位置（0, 1, 2, ...）
;   TI    = 0（GDT），1（LDT）
;   RPL   = 请求特权级（0=内核，3=用户）
;
; 我们 GDT 布局：
;   index 0: null        → selector 0x00
;   index 1: 64-bit code → selector 0x08 (CODE_SEG)
;   index 2: data        → selector 0x10 (DATA_SEG，32/64 位共用)
;   index 3: 32-bit code → selector 0x18 (CODE_SEG32)
;
; 【Lesson 8 新增】
;   index 4: 64-bit user code (DPL=3) → selector 0x20 (USER_CODE_SEG)
;   index 5: user data (DPL=3)        → selector 0x28 (USER_DATA_SEG)
;   index 6: TSS（系统段，16 字节）    → selector 0x30 (TSS_SEG)
;     user 访问时 OR RPL=3：user CS=0x23, user DS/SS=0x2B
;
; 【为什么需要 32 位代码段】
;   长模式切换前 CPU 在 32 位模式，远跳转必须跳到 32 位段
;   （跳到 64 位段需要 EFER.LME=1 + EFER.LMA=1，那时还没开）
;   所以 GDT 必须同时有 32 位段（启动用）和 64 位段（切换后用）
; ---------------------------------------------------------------
CODE_SEG   equ 0x08           ; gdt64.code  选择子（64 位代码段）
DATA_SEG   equ 0x10           ; gdt64.data  选择子（数据段，32/64 共用）
CODE_SEG32 equ 0x18           ; gdt64.code32 选择子（32 位代码段，启动阶段用）
USER_CODE_SEG equ 0x20        ; gdt64.ucode  选择子（64 位用户代码段，DPL=3）
USER_DATA_SEG equ 0x28        ; gdt64.udata  选择子（用户数据段，DPL=3）
TSS_SEG       equ 0x30        ; gdt64.tss    选择子（TSS 系统段，16 字节）

; ---------------------------------------------------------------
; Multiboot2 header
;
; 【为什么用 multiboot2 而不是 multiboot1】
;   multiboot1 规范是 32 位的，GRUB 加载 64 位 ELF 时可能失败
;   multiboot2 规范原生支持 64 位 ELF，是 64 位内核的标准选择
;
; GRUB 靠这个识别内核：
;   magic     = 0xE85250D6（multiboot2 magic）
;   arch      = 0（multiboot2 machine mode: i386 32-bit protected mode；
;              GRUB 仍以 32 位进入，我们在 entry.asm 里再切到 64 位）
;   length    = header 总长度（含 end tag）
;   checksum  = -(magic + arch + length)，三者之和必须为 0
;
; 必须在文件前 32KB 内，且 8 字节对齐
; 注意：header 本身是 32 位格式（GRUB 在 32 位模式扫描）
;
; end tag 标记 header 结束：
;   type = 0 (end), flags = 0, size = 8
;
; 【为什么同时保留 multiboot2 header 和 PVH note】
;   - multiboot2 header：让 GRUB（ISO 启动）能识别内核
;   - PVH note：让 QEMU -kernel 能识别内核
;   两者不冲突，QEMU 和 GRUB 各自只看自己需要的部分
; ---------------------------------------------------------------
section .multiboot
align 8
header_start:
    dd 0xE85250D6                                ; magic (multiboot2)
    dd 0                                          ; arch (multiboot2 machine mode = i386 32-bit protected mode)
    dd header_end - header_start                  ; header length
    dd 0x100000000 - (0xE85250D6 + 0 + (header_end - header_start))  ; checksum

    ; end tag（必需，标记 header 结束）
    dw 0                                          ; type = 0 (end)
    dw 0                                          ; flags = 0
    dd 8                                          ; size = 8
header_end:

; ---------------------------------------------------------------
; PVH ELF Note（让 qemu-system-x86_64 -kernel kernel.elf 能直接加载）
;
; 【为什么需要 PVH note】
;   QEMU 的 x86_64 模式下，-kernel 命令不直接支持 multiboot1/2 ELF：
;     qemu-system-x86_64 -kernel kernel.elf
;     → "Error loading uncompressed kernel without PVH ELF Note"
;   QEMU 只接受三种格式：
;     1. Linux bzImage（内核镜像格式，不符合我们需求）
;     2. Multiboot2 + 64 位入口 tag（需要复杂的 header tag）
;     3. PVH (Paravirtualization Hypervisor) 协议（最简洁）
;
; 【PVH 是什么】
;   PVH 是 Xen 项目定义的轻量级启动协议，被 QEMU/KVM 复用。
;   QEMU 看到 ELF 里的 PVH note 后：
;     a. 加载 ELF segments 到指定物理地址（和正常 ELF 加载一样）
;     b. 设置 32 位 GDT（CS=0x10 代码段, DS/SS=0x18 数据段）
;     c. 启用 PAE 分页（identity map 整个可用内存）
;     d. 跳到 note 里指定的 32 位入口点（就是我们的 _start）
;     e. eax = 0x336ec578 (PVH magic)
;     f. ebx = hvm_start_info 结构物理地址（含 cmdline、内存映射等）
;     g. 中断关闭（CLI 状态）
;
; 【ELF Note 格式】（标准 ELF note，见 ELF 规范）
;   ┌──────────────────────────────────────────────┐
;   │ namesz (4 字节) │ name 字节数（含 null）     │
;   │ descsz (4 字节) │ desc 字节数                │
;   │ type   (4 字节) │ note 类型                  │
;   │ name   (namesz 字节，4 字节对齐)            │
;   │ desc   (descsz 字节，4 字节对齐)            │
;   └──────────────────────────────────────────────┘
;
;   PVH note 关键字段：
;     name = "Xen\0"（Xen 项目定义的 note namespace）
;     type = 0x18 (XEN_ELFNOTE_PHYS32_ENTRY，"32 位物理地址入口")
;     desc = 4 字节，32 位入口点的虚拟地址
;
; 【为什么入口是 32 位而不是 64 位】
;   PVH 协议规定入口点在 32 位保护模式（不是长模式）。
;   内核自己负责切换到 64 位长模式（和 multiboot2 启动一样）。
;   这样 PVH 和 multiboot2 可以共用 _start 入口代码。
;
; 【为什么放在 .note.pvh section】
;   QEMU 加载 ELF 时，扫描所有 PT_NOTE 类型的 program header，
;   遍历里面的 note，找 name="Xen" + type=18 的。
;   linker.ld 把 .note.pvh 放到 PT_NOTE segment（见 .note 输出段）。
;
; 【section 属性 "note"】
;   nasm 的 section 属性里，"note" 让 section 类型 = SHT_NOTE
;   （Section Header Type = NOTE），这样链接器才会：
;     1. 把它合并到 .note output section
;     2. 自动生成 PT_NOTE program header
;   如果只用 "progbits"，section 类型是 SHT_PROGBITS，
;   链接器不会生成 PT_NOTE，QEMU 找不到 PVH note。
;
;   属性说明：
;     note     = SHT_NOTE 类型（关键）
;     alloc    = 在内存中分配（加载时存在）
;     noexec   = 不可执行（数据段）
;     nowrite  = 只读
; ---------------------------------------------------------------
section .note.pvh note alloc noexec nowrite
align 4
global pvh_note
pvh_note:
    dd 4                  ; namesz = 4（"Xen\0" 含 null 共 4 字节）
    dd 4                  ; descsz = 4（32 位地址 4 字节）
    dd 18                 ; type   = 18 (XEN_ELFNOTE_PHYS32_ENTRY，注意是十进制！)
                         ;         Xen 规范定义 type=18，不是 0x18(=24)
    db "Xen", 0           ; name   = "Xen\0"（4 字节，含 null）
    dd _start             ; desc   = 32 位入口点地址

; ---------------------------------------------------------------
; 代码段（含 32 位启动代码和 64 位入口）
; ---------------------------------------------------------------
section .text
bits 32
global _start

_start:
    ; ============================================================
    ; 第 1 步：保存启动信息（multiboot2 或 PVH 都保存）
    ;
    ;   multiboot2（GRUB 启动）：
    ;     eax = 0x36d76289 (MULTIBOOT2_BOOTLOADER_MAGIC)
    ;     ebx = multiboot2 info 结构物理地址
    ;   PVH（QEMU -kernel 启动）：
    ;     eax = 0x336ec578 (PVH magic)
    ;     ebx = hvm_start_info 结构物理地址
    ;
    ; boot.c 会根据 eax 的值判断启动方式，分别解析对应的结构。
    ; 必须在调用任何 C 代码前保存（EAX/EBX 是 callee-saved
    ; 但 C 编译器仍可能覆盖）
    ;
    ; 注意：x86_64_boot_eax/ebx 定义为 u64（64 位变量），
    ;       但 mov [sym], eax 只写低 4 字节，
    ;       高 4 字节保持 .bss 的初始值 0，
    ;       这样 64 位代码读到的就是正确的 32 位扩展值。
    ; ============================================================
    mov [x86_64_boot_eax], eax
    mov [x86_64_boot_ebx], ebx

    ; ============================================================
    ; 第 2 步：设置 32 位栈
    ;
    ; 切换到长模式前 C 代码不能跑，但 lgdt/rdmsr 等指令
    ; 不需要栈；设置栈是为了将来可能的 call（虽然这里没 call）
    ; 栈向下增长，ESP 设为 stack_top_32（高地址）
    ;
    ; 【为什么 PVH 启动也要重设栈】
    ;   PVH 启动时 QEMU 不设 ESP（协议规定栈由内核自己设）
    ;   multiboot2 启动时 GRUB 设了一个临时栈，但我们用自己的更安全
    ; ============================================================
    mov esp, stack_top_32

    ; ============================================================
    ; 第 3 步：加载我们的 GDT
    ;
    ; 【为什么必须重设 GDT】
    ;   - GRUB 启动：GDT 是 GRUB 设的，只有 32 位段，没有 64 位段
    ;   - PVH 启动：GDT 是 QEMU 设的，只有 32 位段，没有 64 位段
    ;   两种情况下，原 GDT 都没有 64 位代码段（L bit=1），
    ;   而切换到长模式必须加载 64 位 CS → 必须重设 GDT
    ;
    ; lgdt 只改 GDTR 寄存器（指向新 GDT），不影响 CS/DS 等段寄存器
    ; 段寄存器仍指向旧 GDT 的描述符（内存可能被覆盖）
    ; → 必须远跳转重新加载 CS，才能用新 GDT
    ;
    ; GDTR 结构（lgdt 操作数）：
    ;   - limit (2 bytes): GDT 大小 - 1
    ;   - base  (4 bytes in 32-bit / 8 bytes in 64-bit): GDT 物理地址
    ; ============================================================
    lgdt [gdt64.pointer]

    ; ============================================================
    ; 第 4 步：远跳转到 32 位代码段（重新加载 CS）
    ;
    ; 【为什么是 32 位段而不是 64 位】
    ;   此时 CPU 还在 32 位模式：
    ;     - multiboot2: PE=1, PG=0, EFER.LME=0
    ;     - PVH:        PE=1, PG=1, EFER.LME=0（已关分页前还是 32 位）
    ;   跳到 64 位段（L=1）需要 EFER.LME=1 + EFER.LMA=1（分页开启后自动设）
    ;   所以先跳到 32 位段（D=1），完成长模式切换后再跳到 64 位段
    ;
    ; 远跳后 CPU 从新 GDT 加载 CS = CODE_SEG32 (0x18) 的描述符
    ; CS 缓存刷新，旧 GDT 的描述符不再使用
    ; ============================================================
    jmp CODE_SEG32:reload_segments_32

; ---------------------------------------------------------------
; reload_segments_32 — 加载新 GDT 的数据段，然后切换长模式
;
; 这里是远跳转后的落点，CS 已经是新的 32 位代码段
; 接下来要加载 DS/ES/SS 等数据段，然后开始长模式切换
; ---------------------------------------------------------------
reload_segments_32:
    ; ============================================================
    ; 第 5 步：加载数据段寄存器
    ;
    ; DS/ES/FS/GS/SS 全部加载为 DATA_SEG (0x10)
    ; 旧 GDT 的段选择子可能无效，必须全部重载
    ;
    ; 【为什么用 AX 中转】
    ;   32 位下 "mov ds, imm" 不支持立即数到段寄存器
    ;   必须先 mov 到通用寄存器，再 mov 到段寄存器
    ; ============================================================
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; ============================================================
    ; 第 6 步：加载 CR3 = 我们的 PML4 表地址
    ;
    ; CR3 指向 PML4（Page Map Level 4）表的物理地址
    ; 这是 4 级页表的顶层：PML4 → PDPT → PD → PT → 物理页
    ;
    ; pml4_table 是我们在 .data 段定义的页表，已 identity map 前 4GB
    ; identity map = 虚拟地址 == 物理地址（前 4GB）
    ;
    ; 【为什么先加载 CR3 再关分页】
    ;   - PVH 启动时分页已开（PG=1），CR3 指向 QEMU 设的页表
    ;   - 我们要换成自己的 pml4_table
    ;   - 在 PG=1 时写 CR3 会立即切换页表 + 刷新 TLB
    ;   - 因为我们的页表也是 identity map，切换后地址仍有效
    ;   - 顺序：先 CR3（安全切换到我们的页表）→ 再关分页（设 EFER.LME）
    ;
    ;   multiboot2 启动时 PG=0，写 CR3 只是设置寄存器，不立即生效
    ;   等开分页时才用这个 CR3
    ;
    ; mov eax, pml4_table：
    ;   把 pml4_table 的地址加载到 EAX
    ;   注意：在 elf64 下，符号地址是 64 位的，
    ;         但我们的内核在 1MB，地址 < 4GB，
    ;         所以 32 位 EAX 足够装下
    ; ============================================================
    mov eax, pml4_table
    mov cr3, eax

    ; ============================================================
    ; 第 7 步：关闭分页（CR0.PG = 0）
    ;
    ; 【为什么必须关分页】
    ;   x86 规范：EFER.LME 只能在 CR0.PG=0 时修改
    ;     - PG=1 时写 EFER.LME → 触发 #GP（General Protection Fault）
    ;   - multiboot2 启动：PG=0，AND 操作无影响
    ;   - PVH 启动：PG=1，必须清 PG bit
    ;
    ; 【关分页后 CPU 怎么继续执行】
    ;   关分页后，CPU 直接用物理地址取指（不分页）
    ;   我们的内核在 1MB 物理地址，关分页后 EIP 仍指向有效的物理地址
    ;   下一条指令（mov eax, cr4）能正常取到
    ;
    ; CR0 bit 31 = PG（Paging Enable）
    ; 0x7FFFFFFF = bit 31 = 0，其余位保持原值
    ; ============================================================
    mov eax, cr0
    and eax, 0x7FFFFFFF                 ; 清 bit 31 (PG)
    mov cr0, eax

    ; ============================================================
    ; 第 8 步：启用 PAE（Physical Address Extension）
    ;
    ; CR4 寄存器，bit 5 = PAE
    ; PAE 把物理地址从 32 位扩展到 36 位（早期 Intel 设计）
    ; 但更重要的是：长模式【必须】先启用 PAE
    ; （因为长模式用 4 级页表，PAE 是它的前提）
    ;
    ; 读-改-写 CR4：
    ;   mov eax, cr4   ; 读
    ;   or  eax, bit   ; 改
    ;   mov cr4, eax   ; 写
    ;
    ; - multiboot2 启动：PAE=0，需要开
    ; - PVH 启动：PAE=1（关分页不影响 CR4，PAE 仍是 1），OR 无影响
    ; ============================================================
    mov eax, cr4
    or eax, 1 << 5                      ; bit 5 = PAE
    mov cr4, eax

    ; ============================================================
    ; 第 9 步：设置 EFER.LME = 1（启用长模式）
    ;
    ; EFER（Extended Feature Enable Register）是 MSR：
    ;   地址 = 0xC0000080
    ;   bit 8 = LME (Long Mode Enable)
    ;
    ; rdmsr/wrmsr 指令：
    ;   ECX = MSR 地址
    ;   读：EDX:EAX = MSR 值（高 32 位:低 32 位）
    ;   写：EDX:EAX → MSR
    ;
    ; 此时 PG=0（刚关的），可以安全设置 LME
    ;
    ; 设置 LME 后，长模式"待命"：
    ;   - EFER.LME = 1 表示"准备好进长模式"
    ;   - 但还没真正进入，要等启用分页（CR0.PG）才激活
    ; ============================================================
    mov ecx, 0xC0000080                 ; EFER MSR 地址
    rdmsr                               ; 读 EFER 到 EDX:EAX
    or eax, 1 << 8                      ; 设置 LME bit (bit 8)
    wrmsr                               ; 写回 EFER

    ; ============================================================
    ; 第 10 步：启用分页（CR0.PG = 1）→ 进入兼容模式
    ;
    ; CR0 寄存器：
    ;   bit 0  = PE（Protection Enable）
    ;   bit 31 = PG（Paging Enable）
    ;
    ; 启用分页的瞬间：
    ;   - 因为 EFER.LME=1，CPU 进入"长模式兼容模式"（compat mode）
    ;   - 此时代码【仍按 32 位执行】（CS 的 L bit 还没生效）
    ;   - 虚拟地址 = 物理地址（identity mapping 生效）
    ;   - 需要远跳转才能正式进入 64 位模式
    ;
    ; - multiboot2 启动：PE=1, PG=0 → 设 PG=1
    ; - PVH 启动：PE=1, PG=0（刚关的）→ 设 PG=1
    ; 两种情况都正确
    ; ============================================================
    mov eax, cr0
    or eax, (1 << 31) | (1 << 0)        ; bit 31 = PG, bit 0 = PE
    mov cr0, eax

    ; ============================================================
    ; 第 11 步：远跳转到 64 位代码段 → 正式进入长模式
    ;
    ; jmp CODE_SEG:long_mode_start
    ;
    ; 远跳转做的事：
    ;   1. 加载 CS = CODE_SEG (0x08，gdt64.code 选择子)
    ;   2. CPU 查 GDT 第 1 项（64 位 code segment）
    ;   3. 发现 L bit = 1 → 切换到 64 位模式
    ;   4. 跳转到 long_mode_start
    ;
    ; 从这一行开始，CPU 正式运行在 64 位模式下
    ; ============================================================
    jmp CODE_SEG:long_mode_start

; ---------------------------------------------------------------
; 64 位入口
; ---------------------------------------------------------------
bits 64
long_mode_start:
    ; ============================================================
    ; 加载数据段寄存器
    ;
    ; 64 位下 DS/ES/FS/GS 的 base/limit 被忽略（除了 FS/GS 的 base）
    ; 但 SS 必须是有效段（不能是 null selector，否则栈访问会 #GP）
    ;
    ; mov ax, DATA_SEG + mov ds, ax：
    ;   加载 DS = 0x10（gdt64.data 选择子）
    ;   注意：直接 mov ds, 0x10 在 64 位下是非法的，
    ;         必须通过通用寄存器中转
    ; ============================================================
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; ============================================================
    ; 设置 64 位栈
    ;
    ; 64 位下栈向下增长，RSP 是 64 位寄存器
    ; 清空 RBP（帧指针）方便调试器识别栈帧边界
    ; ============================================================
    mov rsp, stack_top_64
    mov rbp, 0

    ; ============================================================
    ; 调用 64 位 kernel_main
    ;
    ; 此时 CPU 在 64 位模式，调用约定是 System V AMD64：
    ;   参数通过 RDI/RSI/RDX/RCX/R8/R9 传递
    ;   返回值在 RAX
    ;   栈必须 16 字节对齐（call 之前）
    ;
    ; kernel_main 不返回，但万一返回就停机
    ; ============================================================
    call kernel_main

    ; ============================================================
    ; 安全停机（不应该到这里）
    ; ============================================================
.hang:
    cli
    hlt
    jmp .hang

; ================================================================
; GDT（Global Descriptor Table）
;
; GDT 包含 4 个段描述符：
;   index 0: null          （CPU 要求第一项必须 null）
;   index 1: 64-bit code   （长模式代码段，L bit=1）
;   index 2: data          （数据段，32/64 位共用）
;   index 3: 32-bit code    （启动阶段代码段，D bit=1）
;
; 【为什么 32 位代码段在 64 位段后面】
;   顺序不影响功能，但把启动阶段用的 32 位段放后面，
;   让常用的 64 位段 selector 值更小（0x08 比 0x18 更紧凑）
; ================================================================
section .rodata
global gdt64

gdt64:
    ; -----------------------------------------------------------
    ; null descriptor（index 0，selector 0x00）
    ; CPU 要求 GDT 第一项必须是 null（全 0）
    ; -----------------------------------------------------------
    dq 0

.code:
    ; -----------------------------------------------------------
    ; 64 位代码段（index 1，selector 0x08）
    ;
    ; 描述符格式（8 字节）：
    ;   byte 0-1: limit[0:15]        = 0xFFFF
    ;   byte 2-4: base[0:23]        = 0x000000
    ;   byte 5:   access byte        = 0x9A
    ;   byte 6:   flags + limit[16:19] = 0xAF
    ;   byte 7:   base[24:31]       = 0x00
    ;
    ; access byte 0x9A = 1001 1010：
    ;   bit 7 P (Present)      = 1
    ;   bit 6-5 DPL             = 00 (ring 0)
    ;   bit 4 S (Descriptor type) = 1 (code/data, not system)
    ;   bit 3 Type bit 3        = 1 (code: executable)
    ;   bit 2 Type bit 2        = 0 (non-conforming)
    ;   bit 1 Type bit 1        = 1 (readable)
    ;   bit 0 Type bit 0        = 0 (not accessed)
    ;
    ; flags 0xA = 1010：
    ;   bit 3 reserved          = 0
    ;   bit 2 L (Long mode)     = 1 ★（关键！64 位代码段）
    ;   bit 1 D (Default operand size) = 0（L=1 时必须 0）
    ;   bit 0 G (Granularity)   = 1（4KB 单位）
    ;
    ; limit high 0xF：配合 G=1，limit = 0xFFFFF * 4KB = 4GB
    ;   （64 位下 limit 被忽略，但保留标准值）
    ; -----------------------------------------------------------
    dq 0x00AF9A000000FFFF

.data:
    ; -----------------------------------------------------------
    ; 数据段（index 2，selector 0x10）
    ;
    ; 【32/64 位共用】数据段描述符 32 位和 64 位格式一样：
    ;   - 64 位下 DS/ES/SS 的 base/limit 被忽略
    ;   - 32 位下需要正确的 base=0, limit=4GB
    ;   所以一个描述符 0x00CF92000000FFFF 同时满足 32 位和 64 位
    ;
    ; access byte 0x92 = 1001 0010：
    ;   P=1, DPL=00, S=1, Type=0 (data), writable=1
    ;
    ; flags 0xC = 1100：
    ;   bit 2 L = 0（数据段没有 L）
    ;   bit 1 D = 1（32 位 operand size，64 位下被忽略）
    ;   bit 0 G = 1
    ; -----------------------------------------------------------
    dq 0x00CF92000000FFFF

.code32:
    ; -----------------------------------------------------------
    ; 32 位代码段（index 3，selector 0x18）
    ;
    ; 【为什么需要 32 位代码段】
    ;   启动阶段 CPU 在 32 位保护模式，远跳转必须跳到 32 位段
    ;   （跳到 64 位段需要 EFER.LME=1 + EFER.LMA=1，那时还没开）
    ;   所以 GDT 必须有 32 位代码段供启动阶段使用
    ;
    ; 描述符格式（和 64 位代码段对比）：
    ;   access byte 0x9A（同 64 位段：P=1, DPL=0, S=1, code, readable）
    ;   flags 0xC = 1100：
    ;     bit 2 L = 0（★32 位，不是 64 位）
    ;     bit 1 D = 1（★32 位默认操作数大小）
    ;     bit 0 G = 1（4KB 单位）
    ;
    ; limit 0xFFFFF + G=1 → 4GB（覆盖整个 32 位地址空间）
    ; base = 0（平坦模型，段基址 = 0）
    ; -----------------------------------------------------------
    dq 0x00CF9A000000FFFF

; ================================================================
; 【Lesson 8 新增】用户态段 + TSS 描述符
; ================================================================
.ucode:
    ; -----------------------------------------------------------
    ; 64 位用户代码段（index 4，selector 0x20）
    ;
    ; 和内核代码段（gdt64.code）完全一样，【只有 DPL 不同】：
    ;   access byte 0xFA = 1111 1010：
    ;     bit 7 P=1
    ;     bit 6-5 DPL=11 (ring 3 ★用户可访问)
    ;     bit 4 S=1 (code/data)
    ;     bit 3-0 = 1010 (code, readable)
    ;
    ; 描述符值：0x00AFFA000000FFFF
    ;   （内核是 0x00AF9A...，把 access 的 0x9A 换成 0xFA 即 DPL=3）
    ;
    ; user 用 0x20 | RPL3 = 0x23 作为 CS。
    ; -----------------------------------------------------------
    dq 0x00AFFA000000FFFF

.udata:
    ; -----------------------------------------------------------
    ; 用户数据段（index 5，selector 0x28）
    ;
    ; 和内核数据段一样，DPL=3：
    ;   access byte 0xF2 = 1111 0010：
    ;     P=1, DPL=11, S=1, type=data, writable=1
    ;   描述符值：0x00CFF2000000FFFF
    ;     （内核是 0x00CF92...，把 0x92 换成 0xF2 即 DPL=3）
    ;
    ; user 用 0x28 | RPL3 = 0x2B 作为 DS/SS。
    ; -----------------------------------------------------------
    dq 0x00CFF2000000FFFF

.tss:
    ; -----------------------------------------------------------
    ; TSS 描述符（index 6，selector 0x30，16 字节系统段）
    ;
    ; 【为什么是 16 字节】
    ;   64 位模式下，TSS / LDT 等"系统段"描述符是 16 字节（两个普通项宽）。
    ;   代码段 / 数据段描述符仍是 8 字节。
    ;
    ; 【这里先填 0，运行时由 arch_tss_init 填实际值】
    ;   base 是 link-time 才确定的符号地址（tss 在 .bss），
    ;   nasm 可以写死，但我们选择运行时填，让 C 代码集中管理（更清晰）。
    ;   arch_tss_init 会计算 16 字节描述符写到这里。
    ; -----------------------------------------------------------
    dq 0x0000000000000000
    dq 0x0000000000000000

.pointer:
    ; -----------------------------------------------------------
    ; GDTR 加载结构（lgdt 操作数）
    ;
    ; 32 位 lgdt 读 6 字节：2 字节 limit + 4 字节 base
    ; 64 位 lgdt 读 10 字节：2 字节 limit + 8 字节 base
    ;
    ; 我们用 dq（8 字节）存 base：
    ;   - 32 位 lgdt 只读低 4 字节（base 低 32 位）
    ;   - 64 位 lgdt 读全部 8 字节
    ;   - 我们的 GDT 地址 < 4GB，低 32 位足够
    ; -----------------------------------------------------------
    dw $ - gdt64 - 1                     ; limit = GDT 大小 - 1（含新增的 user 段 + TSS）
    dq gdt64                             ; base = GDT 物理地址

; ================================================================
; 页表（4 级页表，identity map 前 4GB）
;
; 4 级页表结构：
;   PML4 (Level 4) → PDPT (Level 3) → PD (Level 2) → PT (Level 1) → 4KB 页
;
; 用 1GB huge page 简化：跳过 PD 和 PT，PDPT 项直接映射 1GB
;   PDPT[i] = (i * 1GB) 物理地址 | flags
;
; identity map：虚拟地址 == 物理地址
;   前 4GB 虚拟地址直接映射到前 4GB 物理地址
;   内核在 1MB，VGA 在 0xB8000，都在前 4GB 内
;
; 页表项 flags（低 12 位）：
;   bit 0 P (Present)      = 1
;   bit 1 W (Writable)     = 1
;   bit 2 U (User)         = 0（仅内核可访问）
;   bit 7 PS (Page Size)   = 1（huge page，1GB）
;   0x83 = P|PS = 1000 0011
; ================================================================
section .data
align 4096                             ; 页表必须 4KB 对齐
global pml4_table

; PML4 表（Page Map Level 4）
; 512 项 × 8 字节 = 4096 字节
; 每项指向一个 PDPT
pml4_table:
    dq pdpt_table + 7                  ; PML4[0] → PDPT
                                       ; +7 = 0b0111 = P|W|U（present, writable, user）
    times 511 dq 0                     ; 其余 511 项未使用

align 4096
; PDPT 表（Page Directory Pointer Table）
; 512 项 × 8 字节 = 4096 字节
; 每项可以指向 PD，或直接是 1GB huge page
pdpt_table:
    ; 前 4 项用 1GB huge page，identity map 0~4GB
    ; 1GB huge page entry 格式：
    ;   bits 0-11:  flags
    ;   bits 30-51: physical base（必须 1GB 对齐）
    ; 0x83 = present + huge（bit 7 = PS = huge page）
    dq 0x83                           ; PDPT[0] → 0 ~ 1GB
    dq (1 << 30) | 0x83               ; PDPT[1] → 1GB ~ 2GB
    dq (2 << 30) | 0x83               ; PDPT[2] → 2GB ~ 3GB
    dq (3 << 30) | 0x83               ; PDPT[3] → 3GB ~ 4GB
    times 508 dq 0                     ; 其余 508 项未使用

; ================================================================
; BSS 段：栈 + TSS
; ================================================================
section .bss
align 16                               ; 栈必须 16 字节对齐（x86 ABI）
stack_bottom_32:
    resb 16384                         ; 32 位栈 16KB（切换前用）
stack_top_32:                         ; 栈顶 = stack_bottom_32 + 16384

align 16
stack_bottom_64:
    resb 65536                         ; 64 位栈 64KB（切换后用）
stack_top_64:                         ; 栈顶 = stack_bottom_64 + 65536
global stack_top_64                   ; 【Lesson 8】导出给 sched.c 设 init task kstack_top

; ---------------------------------------------------------------
; 【Lesson 8】TSS（Task State Segment）结构
;
;   104 字节，运行时由 arch_tss_init 填充：
;     - rsp0 = 当前任务的内核栈顶（每次 context switch 更新）
;     - iobase = 104（无 IO 位图）
;   其他字段全 0。
;
;   GDT[6] 描述符指向这里（arch_tss_init 写入 GDT entry）。
; ---------------------------------------------------------------
align 16
global tss
tss:
    resb 104

; 声明：栈不需要执行权限（消除链接器 warning）
section .note.GNU-stack noalloc noexec nowrite progbits
