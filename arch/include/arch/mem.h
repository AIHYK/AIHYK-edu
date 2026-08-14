/* ================================================================
 * arch/mem.h — 物理内存 + 虚拟内存的架构抽象接口
 *
 * 【Lesson 4 核心新增】
 *
 * 这个头文件定义"内存管理"的统一接口：
 *   - 物理内存管理（PMM, Physical Memory Manager）：分配/释放 4KB 物理页
 *   - 虚拟内存管理（VMM, Virtual Memory Manager）：映射/取消映射 4KB 页
 *   - 内存子系统入口 arch_mem_init()：解析 boot_info 后调用，统一初始化
 *
 * 抽象的设计目标（与 arch/cpu.h / arch/console.h 一致）：
 *   - "做什么" 在这里声明
 *   - "怎么做" 在 arch/x86_64/ 里实现
 *   - 换架构（RISC-V / ARM）时只新增 arch/<arch>/ 实现，不改这个头
 *
 * =================================================================
 *
 * 【为什么 PMM 和 VMM 要分开】
 *
 *   PMM 只关心"物理页号"，不关心虚拟地址：
 *     "给我一个 4KB 物理页" → 返回 paddr_t
 *
 *   VMM 只关心"虚拟地址 → 物理页"的映射关系，不关心页是怎么分配的：
 *     "把虚拟地址 V 映射到物理地址 P，可写" → arch_vmm_map_page(V, P, flags)
 *
 *   分开的好处：
 *     - 上层（kernel/mm.c 的 kmalloc）只需要 VMM + PMM 两个抽象
 *     - PMM 可独立测试（不需要页表也能验证分配逻辑）
 *     - VMM 可独立测试（用预分配的物理页测映射）
 *     - 后续可以做 demand paging（缺页才分配），PMM/VMM 接口不变
 *
 * =================================================================
 *
 * 【x86-64 四级页表回顾】
 *
 *   虚拟地址 64 位（实际只用低 48 位）：
 *     [63:48] canonical 47 位扩展（必须全 0 或全 1，否则 #GP）
 *     [47:39] PML4 index     (9 bits, 512 entries)
 *     [38:30] PDPT index      (9 bits, 512 entries)
 *     [29:21] PD index        (9 bits, 512 entries)
 *     [20:12] PT index        (9 bits, 512 entries)
 *     [11:0]  offset          (12 bits, 4KB page)
 *
 *   页表层级：
 *     PML4 → PDPT → PD → PT → 4KB page
 *     每级表 512 项 × 8 字节 = 4KB（恰好一个物理页）
 *
 *   CR3 寄存器存 PML4 的物理地址。
 *   CPU 取指 / 读写内存时自动走这棵 4 级树。
 *
 * 【递归页表技巧（recursive mapping）】
 *
 *   如果我们把 PML4[511] 指向 PML4 自己（CR3 的物理地址），
 *   那么访问 0xFFFFFFFFFFxxx 就能"递归回到 PML4"：
 *
 *     虚拟地址 0xFFFFFFFFFFFFF000 = PML4 自身（4KB 全表）
 *     虚拟地址 0xFFFFFFFFFFE00000 + (i << 12) = 第 i 个 PDPT
 *     虚拟地址 0xFFFFFFFFC0000000 + (i << 21) = 第 i 个 PD
 *     虚拟地址 0xFFFFFF8000000000 + (i << 30) = 第 i 个 PT
 *
 *   这让我们在虚拟地址空间里直接读改页表项，
 *   不用每次都通过 CR3 + 物理地址转换。
 *
 *   arch/x86_64/vmm.c 用这个技巧实现 arch_vmm_map_page / unmap / get_phys。
 * ================================================================ */

#ifndef ARCH_MEM_H
#define ARCH_MEM_H

#include <kernel/types.h>
#include <arch/boot.h>

/* ---------------------------------------------------------------
 * 页大小常量
 *
 * x86-64 的标准页大小是 4KB = 4096 字节。
 * 我们用 4KB 作为最小分配单位（PMM/VMM 都按 4KB 来）。
 *
 * 大页（huge page）：
 *   - 2MB huge page：跳过 PT 这一级，PD 项直接映射 2MB
 *   - 1GB huge page：跳过 PT + PD，PDPT 项直接映射 1GB
 *
 *   我们在初始化时用 1GB huge page identity-map 大块内存
 *   （省下大量 4KB 页表占用的内存），需要细粒度映射时再
 *   改成 4KB 页（VMM 会按需拆分）。
 * --------------------------------------------------------------- */
#define PAGE_SIZE   4096u
#define PAGE_SHIFT  12u
#define PAGE_MASK   (~(usize_t)(PAGE_SIZE - 1))

/* 把任意地址向上对齐到页边界 */
#define PAGE_ALIGN_UP(addr) (((addr) + PAGE_SIZE - 1) & ~(usize_t)(PAGE_SIZE - 1))
/* 把任意地址向下对齐到页边界 */
#define PAGE_ALIGN_DOWN(addr) ((addr) & ~(usize_t)(PAGE_SIZE - 1))
/* 给定物理地址，返回所在页的基地址 */
#define PAGE_FRAME(addr)    PAGE_ALIGN_DOWN(addr)
/* 把物理地址转成"页框号"（frame number）*/
#define PAGE_FRAME_NUMBER(addr) ((addr) >> PAGE_SHIFT)
/* 把页框号转回物理地址 */
#define PAGE_FRAME_ADDR(n)   ((paddr_t)(n) << PAGE_SHIFT)

/* ---------------------------------------------------------------
 * VMM 页表项 flags
 *
 * 这些 flag 对应 x86-64 PTE（Page Table Entry）的 bit 含义。
 * VMM 在写 PTE 时按位 OR 这些 flag。
 *
 * 注意：和 PMM 无关，PMM 只管分配物理页，不管页表项内容。
 * --------------------------------------------------------------- */

/* bit 0: P (Present) — 页是否在内存里
 * 0 = 不在内存（访问触发 #PF）
 * 1 = 在内存
 *
 * 必填。如果不带这个 flag，arch_vmm_map_page 会把 PTE 设为"不存在"。 */
#define PAGE_FLAG_PRESENT   (1u << 0)

/* bit 1: R/W (Read/Write) — 读 / 读写
 * 0 = 只读
 * 1 = 可读写 */
#define PAGE_FLAG_WRITE     (1u << 1)

/* bit 2: U/S (User/Supervisor) — 用户态可见
 * 0 = 仅 ring 0（内核）可访问
 * 1 = ring 3（用户态）也可访问
 *
 * 我们目前没有用户态，但保留 flag 方便以后扩展。 */
#define PAGE_FLAG_USER      (1u << 2)

/* bit 3: PWT (Page-Level Write-Through)
 * 0 = 写回（write-back，默认）
 * 1 = 写穿（write-through，每次写都同步到内存）
 * 用于 MMIO 区域（设备内存不能缓存）。 */
#define PAGE_FLAG_WT        (1u << 3)

/* bit 4: PCD (Page-Level Cache Disable)
 * 0 = 可缓存
 * 1 = 不缓存（每次访问都从设备读）
 * 用于 MMIO 区域。 */
#define PAGE_FLAG_CD        (1u << 4)

/* bit 7: PS (Page Size)
 * 0 = 4KB 页（PT 项）
 * 1 = huge page（PD 项 = 2MB / PDPT 项 = 1GB）
 *
 * VMM 通常不直接设这个 flag，由 huge page 初始化逻辑用。 */
#define PAGE_FLAG_HUGE      (1u << 7)

/* bit 8: G (Global) — 全局页
 * 1 = CR3 切换时不刷 TLB（内核代码段常用，加速进程切换） */
#define PAGE_FLAG_GLOBAL    (1u << 8)

/* bit 63: NX (No Execute) — 不可执行
 * 1 = 此页不可取指（CPU 取指会触发 #PF）
 * 用于数据页防止代码注入。
 *
 * 注意：bit 63 在 32 位整型里写不下，必须用 u64。 */
#define PAGE_FLAG_NX        (1ULL << 63)

/* 内核代码段/数据段常用 flag 组合：
 *   PAGE_FLAG_KERNEL_CODE = P | G | NX (可读不可写不可执行... 等等，代码段可执行)
 *   PAGE_FLAG_KERNEL_DATA = P | W | G | NX
 *   PAGE_FLAG_KERNEL_CODE = P | G  (可执行)
 *   PAGE_FLAG_MMIO        = P | W | CD | WT  (设备内存，禁缓存)
 *
 * 我们只列常用组合，调用方按需 OR。 */
#define PAGE_FLAGS_KERNEL_RW   (PAGE_FLAG_PRESENT | PAGE_FLAG_WRITE | PAGE_FLAG_GLOBAL)
#define PAGE_FLAGS_KERNEL_RO   (PAGE_FLAG_PRESENT | PAGE_FLAG_GLOBAL)
#define PAGE_FLAGS_KERNEL_CODE (PAGE_FLAG_PRESENT | PAGE_FLAG_GLOBAL)

/* ---------------------------------------------------------------
 * arch_mem_init — 内存子系统初始化入口（统一调用）
 *
 * 参数：
 *   info — arch_boot_init 解析好的 boot_info
 *
 * 这个函数做四件事：
 *   1. arch_pmm_init(info)
 *      - 把 bootloader 报告的内存区域建成内部"可用页"位图
 *      - 标记低 1MB / 内核镜像 / MMIO 等为"已用"
 *   2. arch_vmm_init()
 *      - 构造新的 PML4（覆盖所有可用 RAM，加递归映射）
 *      - 切换 CR3 到新 PML4
 *   3. （可选）arch_heap_init() — 由 kernel/mm.c 负责
 *
 * main.c 应该在控制台初始化【之后】、第一次 kmalloc【之前】调用。 */
void arch_mem_init(struct boot_info *info);

/* ---------------------------------------------------------------
 * arch_pmm_init — 物理内存管理器初始化
 *
 * 参数：
 *   info — boot_info（含 mem_region 数组）
 *
 * 流程：
 *   1. 根据 info->regions 计算"内存上界"（最高可用物理地址）
 *   2. 分配位图（每 4KB 一位），覆盖整个上界
 *   3. 默认全部标"已用"
 *   4. 遍历 regions，把 MEM_USABLE 区域标"可用"
 *   5. 显式保留低 1MB + 内核镜像为"已用"
 *   6. 统计 total / free / used 帧数
 *
 * 失败 → panic（PMM 是后续一切的基础，不能跑就死机最安全） */
void arch_pmm_init(struct boot_info *info);

/* ---------------------------------------------------------------
 * arch_pmm_alloc_frame — 分配一个 4KB 物理页
 *
 * 返回值：
 *   成功 — 物理页基地址（页对齐，4KB 对齐）
 *   失败 — 0（没有可用帧；调用方应处理 OOM）
 *
 * 【为什么用 0 表示失败而不是 (paddr_t)-1】
 *   - 0 是"无效地址"，从 0 地址访问会触发 #PF（被 PMM 显式保留）
 *   - 调用方误用 alloc_frame 返回值时会立即崩溃，便于发现 bug
 *   - (paddr_t)-1 在 64 位下是 0xFFFFFFFFFFFFFFFF，canonical address 不合法
 *     但分配逻辑通常返回 0 表示失败是 C 习惯
 *
 * 【线程安全】
 *   单核 + IF=1（中断开），所以内部用 arch_irq_save/restore 保护位图。
 *   后续多核时需加 spinlock。 */
paddr_t arch_pmm_alloc_frame(void);

/* ---------------------------------------------------------------
 * arch_pmm_free_frame — 释放一个 4KB 物理页
 *
 * 参数：
 *   frame — arch_pmm_alloc_frame 返回的物理地址
 *
 * 行为：
 *   - 把对应位清 0，下次 alloc 可重新分配
 *   - 如果 frame 不是页对齐，panic（释放的不是整页，bug）
 *   - 如果 frame 不在 [0, max_phys] 范围，panic
 *   - 如果 frame 之前没分配过（位图已是 0），警告但不 panic（防御性）
 *
 * 【为什么 kernel 不要 OOM 时也尝试 free】
 *   内核堆按需分配，耗尽时调用方应释放不再用的对象。
 *   但内核本身很少 free（不像用户态），主要用于教学演示。 */
void arch_pmm_free_frame(paddr_t frame);

/* ---------------------------------------------------------------
 * arch_pmm_reserve_range — 保留一段物理地址区间（标记为已用）
 *
 * 参数：
 *   start — 起始物理地址
 *   end   — 结束物理地址（不含）
 *
 * 用途：
 *   - 内核镜像（[__kernel_start, __kernel_end)）
 *   - 低 1MB（BIOS / VGA / IVT）
 *   - 已映射给某个设备 / 用户态的页
 *
 * 注意：保留只是 PMM 层面"不再分配"，
 *       页表里这段地址仍然存在映射（identity map）。
 *       这是上层"已用"的概念，不是"取消映射"。 */
void arch_pmm_reserve_range(paddr_t start, paddr_t end);

/* ---------------------------------------------------------------
 * 统计函数（返回当前 PMM 状态）
 *
 *   arch_pmm_total_frames() — 内存总帧数（= max_phys / 4KB）
 *   arch_pmm_free_frames()  — 当前可用帧数
 *   arch_pmm_used_frames() — 当前已用帧数（= total - free）
 *
 * 调用方可以算出"可用内存大小"= free_frames × 4KB。
 * --------------------------------------------------------------- */
usize_t arch_pmm_total_frames(void);
usize_t arch_pmm_free_frames(void);
usize_t arch_pmm_used_frames(void);

/* ---------------------------------------------------------------
 * arch_vmm_init — 虚拟内存管理器初始化
 *
 * 流程：
 *   1. 分配一个新的 PML4 页（arch_pmm_alloc_frame）
 *   2. 清零（全 0 = 所有项都不存在）
 *   3. identity-map 所有可用 RAM（用 1GB huge page，省内存）
 *   4. identity-map 低 1MB（BIOS / VGA 区，特殊区域不能漏）
 *   5. 设 PML4[511] = PML4 自身物理地址 | P | W  （递归映射）
 *   6. 切换 CR3 到新 PML4
 *
 * 【为什么必须 identity-map 所有可用 RAM】
 *   - 内核代码当前在物理地址 1MB 处运行（早期 entry.asm 设的）
 *   - 切换 CR3 时如果新页表不映射 1MB 区域，CPU 立刻 #PF triple fault
 *   - 所以 VMM 初始化必须保证切换前后"所有在用的虚拟地址仍有效"
 *
 * 【为什么用 1GB huge page 而不是 4KB 页】
 *   - 4KB 页表完整映射 4GB 需要 4 个 PML4 项 + 4 个 PDPT + 4 个 PD + 4 × 512 PT
 *     = 4 + 4 + 4 + 2048 = 约 2060 个 4KB 页 = 8MB 内存
 *   - 1GB huge page 只需 4 个 PDPT 项 = 32 字节，省 8MB
 *   - 内核代码大部分时候不需要细粒度映射
 *   - 真正需要 4KB 页时，VMM 的 arch_vmm_map_page 会"按需拆分" huge page
 *     （但当前实现只是新增 4KB 映射到未映射区域，不拆分现有 huge page） */
void arch_vmm_init(void);

/* ---------------------------------------------------------------
 * arch_vmm_map_page — 映射一个 4KB 虚拟页到 4KB 物理页
 *
 * 参数：
 *   virt  — 虚拟地址（必须页对齐，否则 panic）
 *   phys  — 物理地址（必须页对齐，否则 panic）
 *   flags — PAGE_FLAG_* 按位 OR（决定 P/W/U/CD/... 位）
 *
 * 返回值：
 *   0  — 成功
 *  -1  — 失败（PMM 无可用帧给中间页表，或虚拟地址不在合法范围）
 *
 * 流程：
 *   1. 计算各级页表 index
 *   2. 通过递归映射取得 PML4 项
 *   3. PDPT 项不存在 → 从 PMM 分配一页 → 清零 → 填到 PML4 项
 *   4. PD 项不存在 → 同上
 *   5. PT 项不存在 → 同上
 *   6. PT[index] = phys | flags
 *   7. invlpg 刷新 TLB（或整个 CR3 刷新）
 *
 * 【为什么映射完后要 invlpg】
 *   CPU 的 TLB 会缓存"虚拟地址 → 物理页"映射，
 *   即使页表改了，TLB 不会自动更新。
 *   如果之前访问过这个虚拟地址（结果是 #PF），TLB 缓存的是"不存在"，
 *   不 invlpg 的话即使改了页表，下一次访问仍可能 #PF。
 *
 * 注意：本函数在 IRQ 关闭下运行（arch_irq_save/restore），
 *       防止中断处理程序在页表更新到一半时被中断。 */
int arch_vmm_map_page(vaddr_t virt, paddr_t phys, u64 flags);

/* ---------------------------------------------------------------
 * arch_vmm_unmap_page — 取消映射一个 4KB 虚拟页
 *
 * 参数：
 *   virt — 虚拟地址（必须页对齐）
 *
 * 返回值：
 *   0 — 成功（即使原来没映射也不报错）
 *  -1 — 失败（虚拟地址不合法）
 *
 * 行为：
 *   - 把 PT[index].P 清 0（页不存在）
 *   - invlpg 刷新 TLB
 *
 * 【为什么 unmap 不释放物理页】
 *   物理页属于 PMM 管，VMM 只管映射关系。
 *   调用方决定是否释放物理页（arch_pmm_free_frame）。
 *   分离关注点，避免误释放"仍在用的页"。 */
int arch_vmm_unmap_page(vaddr_t virt);

/* ---------------------------------------------------------------
 * arch_vmm_unmap_user_page — 深度取消映射 + 释放 user 页 + 释放空的中间页表
 *
 * 【Lesson 8 新增】
 *
 * 参数：
 *   virt — 虚拟地址（必须页对齐）
 *
 * 行为：
 *   1. 取 PT 项对应的数据页物理地址（未映射则直接返回）
 *   2. 清 PT 项（arch_vmm_unmap_page）+ 释放数据页物理帧（arch_pmm_free_frame）
 *   3. 向上走：
 *      - PT 全空（512 项全 0）→ 释放 PT 帧 + 清 PD 项
 *      - PD 全空 → 释放 PD 帧 + 清 PDPT 项
 *      - PDPT 全空 且 pml4_idx ∈ [1,510]
 *        → 释放 PDPT 帧 + 清 PML4 项
 *   4. 永不动 PML4[0]（identity map）/ PML4[511]（recursive map）/ PML4 根
 *
 * 与 arch_vmm_unmap_page 区别：
 *   - arch_vmm_unmap_page：只清 PT 项（数据页物理帧不释放，PD/PT 永远占着）
 *   - arch_vmm_unmap_user_page：完整释放（数据页 + 空的中间页表），用于 user 任务退出
 *
 * 适用于 user 任务退出时的清理（task_reaper 调 arch_user_unmap_pages）。
 * 不适用于内核自身使用的页（内核页表共享，中间页表不应被释放）。 */
void arch_vmm_unmap_user_page(vaddr_t virt);

/* ---------------------------------------------------------------
 * arch_vmm_get_phys — 虚拟地址 → 物理地址转换
 *
 * 返回值：
 *   成功 — 物理地址（页对齐的基址 + 页内偏移）
 *   失败 — 0（虚拟地址未映射，或不在合法范围）
 *
 * 用于：
 *   - 调试（"这个指针实际指向哪里"）
 *   - DMA 设置（设备需要物理地址，不能给虚拟地址）
 *   - 共享内存跨进程传递时，需要拿物理页号 */
paddr_t arch_vmm_get_phys(vaddr_t virt);

/* ---------------------------------------------------------------
 * arch_vmm_flush_tlb — 刷新整个 TLB
 *
 * 实现：重写 CR3（CPU 看到相同的 CR3 值仍会刷新 TLB，
 *      除非设了 PGE bit + PAGE_FLAG_GLOBAL）。
 *
 * 什么时候用：
 *   - 切换 CR3（地址空间切换，进程切换）
 *   - 大量映射变化后（避免一条条 invlpg）
 *   - 调试怀疑 TLB 不一致 */
void arch_vmm_flush_tlb(void);

/* ---------------------------------------------------------------
 * arch_vmm_get_cr3 — 读取当前 CR3（PML4 物理地址）
 *
 * 主要用于调试和切换地址空间时备份。 */
paddr_t arch_vmm_get_cr3(void);

/* ---------------------------------------------------------------
 * arch_vmm_load_cr3 — 写入 CR3
 *
 * 参数：pml4_phys — 新的 PML4 物理地址
 *
 * 切换地址空间。返回后 CPU 立即用新页表。 */
void arch_vmm_load_cr3(paddr_t pml4_phys);

#endif /* ARCH_MEM_H */
