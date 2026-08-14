/* ================================================================
 * arch/user.h — 用户态（ring 3）管理的架构抽象接口
 *
 * 【Lesson 8 核心新增】
 *
 * 这个头文件定义"用户态任务"在架构层的接口：
 *   - GDT 段选择子常量（user code / user data）
 *   - TSS（Task State Segment）初始化与更新
 *   - 用户态进入（ring 0 → ring 3 的 iretq 跳转）
 *   - 用户任务页表映射（user 代码页 / 栈页，U/S=1）
 *
 * 抽象设计（与 arch/cpu.h / arch/mem.h 一致）：
 *   - "做什么" 在这里声明
 *   - "怎么做" 在 arch/x86_64/user.c / user.asm 实现
 *   - 换架构（RISC-V / ARM）时只新增 arch/<arch>/ 实现，不改这个头
 *
 * =================================================================
 *
 * 【为什么需要 TSS】
 *
 *   x86-64 在特权级切换（ring 3 → ring 0）时，CPU 需要知道"ring 0 用哪个栈"。
 *   这个信息存在 TSS.sp0 里。
 *
 *   没用户态时（L1-L7 全 ring 0）：所有中断都在当前栈处理，TSS 没用。
 *   有用户态后（L8）：user 发 int 0x80 或被 IRQ 抢占时，CPU 从 TSS.sp0
 *   取 ring 0 栈指针，把 user 的 SS:RSP:RFLAGS:CS:RIP 压到那个栈上。
 *
 *   所以 L8 必须：
 *     1. 分配一个 TSS（104 字节结构）
 *     2. 在 GDT 里加 TSS 描述符（系统段，16 字节）
 *     3. 加载 TR 寄存器指向 TSS
 *     4. 每次任务切换时更新 TSS.sp0 = next 任务的内栈顶
 *
 * 【为什么 TSS 只用 sp0】
 *   TSS 还能存 rsp1/rsp2（ring 1/2 栈）、IST（中断栈表）、IO 位图等。
 *   AIHYK 只用 ring 0 和 ring 3，所以只用 sp0。其他字段清零。
 *
 * 【共享地址空间 vs 独立地址空间】
 *   AIHYK L8 采用"共享地址空间"设计：
 *     - user 和 kernel 在同一个 CR3（同一份页表）
 *     - kernel 页 U/S=0（user 不可访问 → 特权隔离）
 *     - user 页 U/S=1（user 可访问，kernel 也可访问）
 *   优点：不用每进程一份页表 + 内核映射，简化教学。
 *   缺点：user 能看到 kernel 的虚拟地址（但访问会被 #PF 拒绝）。
 *   真实 OS（Linux）每进程独立页表 + kernel 映射在高位，L9+ 再扩展。
 * ================================================================ */

#ifndef ARCH_USER_H
#define ARCH_USER_H

#include <kernel/types.h>

/* ---------------------------------------------------------------
 * GDT 段选择子（Lesson 8 新增）
 *
 *   段选择子格式：index(13) | TI(1) | RPL(2)
 *
 *   GDT 布局（entry.asm，L8 扩展后）：
 *     index 0: null           → 0x00
 *     index 1: kernel code    → 0x08 (CODE_SEG)
 *     index 2: kernel data    → 0x10 (DATA_SEG)
 *     index 3: 32-bit code    → 0x18 (CODE_SEG32, 启动用)
 *     index 4: user code      → 0x20 (USER_CODE_SEG)   ← L8 新增
 *     index 5: user data      → 0x28 (USER_DATA_SEG)   ← L8 新增
 *     index 6: TSS            → 0x30 (TSS_SEG)          ← L8 新增
 *
 *   从 ring 3 访问时，选择子要 OR RPL=3：
 *     user CS = 0x20 | 3 = 0x23
 *     user DS/SS = 0x28 | 3 = 0x2B
 * --------------------------------------------------------------- */
#define GDT_INDEX_USER_CODE   4
#define GDT_INDEX_USER_DATA   5
#define GDT_INDEX_TSS         6

#define USER_CODE_SEG    (GDT_INDEX_USER_CODE * 8)         /* 0x20 ring 0 用（不用，仅参考） */
#define USER_DATA_SEG    (GDT_INDEX_USER_DATA * 8)         /* 0x28 */
#define TSS_SEG          (GDT_INDEX_TSS * 8)               /* 0x30 */

/* ring 3 实际使用的选择子（带 RPL=3） */
#define USER_CS_RPL3     (USER_CODE_SEG | 3)   /* 0x23 */
#define USER_DS_RPL3     (USER_DATA_SEG | 3)   /* 0x2B */

/* ---------------------------------------------------------------
 * 用户态虚拟地址布局
 *
 *   user 代码页和栈页必须映射在"非 identity-map 区"——
 *   因为 identity-map 的 0~4GB 用 1GB huge page 且 U/S=0（supervisor-only），
 *   arch_vmm_map_page 不能拆分 huge page。
 *
 *   关键：user 页要放在 PML4[1] 之下（虚拟地址 0x8000000000 = 512GB 起），
 *   因为 PML4[0]（0~512GB）被 arch_vmm_init 用 identity-map 填了，
 *   且 PML4[0] 项的 U/S=0（预创建，不带 USER）。
 *   如果 user 页放在 PML4[0] 下，即使低层页表项有 U 位，
 *   ring 3 也走不过 PML4[0]（U=0 → #PF）。
 *   PML4[1] 是空白的，arch_vmm_map_page 会新建并带 U 位（vmm.c 传播）。
 *
 *   ┌──────────────────────────────────────────────┐
 *   │  USER_CODE_BASE  = 0x8000000000 (512 GB)      │  PML4[1] 起始
 *   │    代码页：P | U（可读可执行，不可写）         │  逐页映射，从 image 拷入
 *   ├──────────────────────────────────────────────┤
 *   │  栈页在 code_vma + USER_STACK_OFFSET 处        │  P | W | U
 *   │    栈向下生长                                  │
 *   └──────────────────────────────────────────────┘
 *
 *   每个 user 任务用不同的 VMA（避免互相覆盖）：
 *     task N: code = USER_CODE_BASE + N * USER_SLOT_SIZE
 * --------------------------------------------------------------- */
#define USER_CODE_BASE       0x8000000000ULL         /* 512 GB：PML4[1] 起始，避开 identity-map */
#define USER_SLOT_SIZE       0x1000000ULL             /* 16 MB：每任务地址空间槽位 */
#define USER_STACK_OFFSET    0x800000ULL              /* 8 MB：栈顶相对代码基址的偏移 */

/* 用户栈大小（1 页 4KB，够小 demo 用；可扩展为多页） */
#define USER_STACK_PAGES     1

/* ---------------------------------------------------------------
 * arch_tss_init — 初始化 TSS 并加载 TR
 *
 *   流程：
 *     1. 清零 TSS 结构（104 字节）
 *     2. 设 TSS.sp0 = 当前内核栈顶（init task 的）
 *     3. 设 TSS.iobase = sizeof(TSS)（无 IO 位图）
 *     4. 在 GDT 里填 TSS 描述符（base = &tss, limit = 103, type=available TSS64）
 *     5. ltr TSS_SEG（加载 TR）
 *
 *   必须在 IDT 加载之后、第一次进入 ring 3 之前调用。
 *   main.c 在中断初始化后、任务调度前调用。
 * --------------------------------------------------------------- */
void arch_tss_init(void);

/* ---------------------------------------------------------------
 * arch_tss_set_sp0 — 更新 TSS.sp0（ring 0 栈指针）
 *
 *   参数：sp0 — 新的 ring 0 栈指针（任务的内栈顶）
 *
 *   每次 context switch 切到新任务前调用：
 *     arch_tss_set_sp0(next->kstack_top);
 *     arch_context_switch(prev, next);
 *
 *   这样当 next（在 ring 3 运行）被中断/syscall 时，CPU 从 TSS.sp0
 *   取 ring 0 栈，压中断帧到正确的内核栈上。
 * --------------------------------------------------------------- */
void arch_tss_set_sp0(u64 sp0);

/* ---------------------------------------------------------------
 * arch_enter_user_mode — 从 ring 0 跳到 ring 3（用户态）
 *
 *   参数：
 *     user_rip — 用户代码入口虚拟地址
 *     user_rsp — 用户栈顶
 *     user_arg — 传给用户的参数（放到 rdi，作为 user 的第一个参数）
 *
 *   行为：构造 IRETQ 栈帧并执行 iretq，CPU 切到 ring 3：
 *     - CS = USER_CS_RPL3 (0x23)
 *     - SS = USER_DS_RPL3 (0x2B)
 *     - RFLAGS = 0x202（IF=1，开中断）
 *     - RIP = user_rip
 *     - RSP = user_rsp
 *     - RDI = user_arg
 *
 *   永不返回（iretq 跳到 ring 3 后，本函数的栈帧被废弃）。
 *   当 user 调 sys_exit 时，syscall_handler 调 sched_exit 切走。
 *
 *   【为什么必须用 iretq 而不是远调用】
 *     x86 特权级切换（高→低）只能用 iretq / sysret：
 *       - retf（远返回）也能改 CS，但不能同时设 RFLAGS/RSP
 *       - iretq 一次设置 RIP/CS/RFLAGS/RSP/SS，是标准做法
 *
 *   【IRETQ 栈帧布局（从低地址往上）】
 *     [rsp+0]  RIP    ← iretq 先弹
 *     [rsp+8]  CS
 *     [rsp+16] RFLAGS
 *     [rsp+24] RSP    ← 特权级变化时才弹（CS.RPL != CPL 时）
 *     [rsp+32] SS
 * --------------------------------------------------------------- */
void arch_enter_user_mode(u64 user_rip, u64 user_rsp, u64 user_arg);

/* ---------------------------------------------------------------
 * arch_user_map_code — 映射用户代码页并装入 image
 *
 *   参数：
 *     vma       — 代码起始虚拟地址（必须页对齐）
 *     image     — 用户程序二进制（flat bin）
 *     image_len — 二进制长度（字节）
 *
 *   行为：
 *     1. 按 image_len 计算需要多少页（向上取整）
 *     2. 逐页：PMM 分配物理页 → VMM 映射 vma+i*4K → phys，flags = P | U
 *     3. 把 image 字节拷贝到 vma（现在可写了——首次写不触发 #PF 因为刚映射）
 *
 *   返回值：页数（成功）/ -1 失败
 *
 *   【为什么代码页 P | U 不带 W】
 *     - W=0 让 user 不能修改自己的代码（防止运行时 patch）
 *     - 但内核首次拷入 image 时要写——内核是 ring 0，不受 U/W 限制
 *       （ring 0 忽略 U 位和只读位，可任意写）
 * --------------------------------------------------------------- */
s64 arch_user_map_code(u64 vma, const void *image, u64 image_len);

/* ---------------------------------------------------------------
 * arch_user_map_stack — 映射用户栈页
 *
 *   参数：
 *     stack_top — 栈顶虚拟地址（必须页对齐）
 *
 *   行为：
 *     1. 从 stack_top 往下数 USER_STACK_PAGES 页
 *     2. 逐页 PMM alloc → VMM map，flags = P | W | U
 *
 *   返回值：0 成功 / -1 失败
 * --------------------------------------------------------------- */
s64 arch_user_map_stack(u64 stack_top);

/* ---------------------------------------------------------------
 * arch_user_unmap_pages — 取消映射并释放 user 页
 *
 *   参数：
 *     vma       — 起始虚拟地址
 *     npages    — 页数
 *
 *   行为：逐页 get_phys → unmap → pmm_free
 *   用于 task_reaper 清理 user 任务的代码页 / 栈页。
 * --------------------------------------------------------------- */
void arch_user_unmap_pages(u64 vma, u64 npages);

#endif /* ARCH_USER_H */
