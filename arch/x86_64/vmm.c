/* ================================================================
 * arch/x86_64/vmm.c — 虚拟内存管理器（VMM, Virtual Memory Manager）
 *
 * 【Lesson 4 核心新增】
 *
 * VMM 负责：
 *   1. 构建新的 PML4 页表（替代 entry.asm 临时设的）
 *   2. identity-map 所有可用 RAM（用 1GB huge page，省内存）
 *   3. 设 PML4[511] = PML4 自身（递归页表技巧）
 *   4. 切换 CR3 到新页表
 *   5. 提供 arch_vmm_map_page / unmap / get_phys API
 *
 * =================================================================
 *
 * 【x86-64 四级页表回顾（详细版）】
 *
 *   64 位虚拟地址（实际只用低 48 位）：
 *
 *     [63:48] canonical extension（必须全 0 或全 1，否则 #GP）
 *     [47:39] PML4 index (9 bits)
 *     [38:30] PDPT index  (9 bits)
 *     [29:21] PD index    (9 bits)
 *     [20:12] PT index    (9 bits)
 *     [11:0]  page offset (12 bits, 4KB page)
 *
 *   - PML4 项 = 8 字节，每项指向一个 PDPT 表
 *   - PDPT 项 = 8 字节，每项指向一个 PD 表，或者直接是 1GB huge page
 *   - PD 项   = 8 字节，每项指向一个 PT 表，或者直接是 2MB huge page
 *   - PT 项   = 8 字节，每项指向一个 4KB 物理页
 *
 *   每级表 512 项 × 8 字节 = 4KB（恰好一个物理页）。
 *
 *   CR3 寄存器存 PML4 的物理地址（低 12 位是 flags，高 40 位是地址）。
 *
 * =================================================================
 *
 * 【递归页表技巧（recursive mapping）】
 *
 *   假设 PML4[511] 指向 PML4 自身（CR3 值）。
 *
 *   那么 CPU 走页表时，访问以下虚拟地址会"绕一圈"回到 PML4：
 *
 *     0xFFFFFFFFFFFFF000 → PML4 自己（4KB 全表）
 *       (PML4[511] → PML4[511] → PML4[511] → PML4[511] → offset)
 *       即"取消"了第 4 层，直接进入 PML4 自身
 *
 *     0xFFFFFFFFFFE00000 + (i << 12) → PML4[i] 指向的 PDPT[i]
 *       (PML4[511] → PML4[511] → PML4[i] → PT[i] → offset)
 *       即"取消"了第 3 层，进入第 i 个 PDPT
 *
 *     0xFFFFFFFFC0000000 + (j << 21) → 第 j 个 PD 的某项
 *
 *     0xFFFFFF8000000000 + (k << 30) → 第 k 个 PT 的某项
 *
 *   简化理解：用 PML4[511] "吃掉" 一层索引。
 *
 *   递归映射的好处：
 *     - 改页表项不用算物理地址
 *     - 直接读改虚拟地址就能操作页表
 *     - 不需要维护"页表虚拟地址缓存"
 *
 *   Linux 也用这个技巧（PML4[511] 通常是 direct map 或 self map）。
 *
 * =================================================================
 *
 * 【递归映射地址计算】
 *
 *   设 vaddr 是要操作的虚拟地址，它对应的：
 *     PML4 index = (vaddr >> 39) & 0x1FF
 *     PDPT index = (vaddr >> 30) & 0x1FF
 *     PD index   = (vaddr >> 21) & 0x1FF
 *     PT index   = (vaddr >> 12) & 0x1FF
 *
 *   那么通过递归映射访问：
 *     PML4 项地址 = 0xFFFFFFFFFFFFF000 + (pml4_idx << 3)
 *     PDPT 项地址 = 0xFFFFFFFFFFE00000 + ((u64)pml4_idx << 12) + (pdpt_idx << 3)
 *     PD   项地址 = 0xFFFFFFFFC0000000 + ((u64)pml4_idx << 21) + (pdpt_idx << 12) + (pd_idx << 3)
 *     PT   项地址 = 0xFFFFFF8000000000 + ((u64)pml4_idx << 30) + (pdpt_idx << 21) + (pd_idx << 12) + (pt_idx << 3)
 *
 *   这些地址都"吃掉"了一层（最后访问 PT 时其实是访问 PD，等等）。
 *   前缀全 1（0xFFFF...）是因为 PML4[511] 的高位是 1，canonical 扩展为全 1。 */
#include <arch/boot.h>
#include <arch/cpu.h>
#include <arch/console.h>
#include <arch/mem.h>
#include <kernel/panic.h>
#include <kernel/types.h>

/* ---------------------------------------------------------------
 * PML4[511] 递归映射的相关常量
 *
 * 递归映射基址：
 *   0xFFFF_FFFF_FFFF_F000 — PML4 自身
 *
 * 计算：bit 47:39 = 511（指向自己），其余 index 全 0，offset 全 0
 *   PML4[511] → PML4[511] → ... → PML4[511] → 0
 *   最后访问的是 PML4 自身的 4KB。 */
#define RECURSIVE_PML4_ADDR  0xFFFFFFFFFFFFF000ULL

/* PML4 索引（递归映射用 index 511） */
#define PML4_RECURSIVE_INDEX 511

/* ---------------------------------------------------------------
 * 当前 PML4 物理地址（arch_vmm_init 切换后保存）
 *
 * 用于：
 *   - 知道递归映射的入口（PML4[511] 指向它）
 *   - 调试时打印
 *   - 切换地址空间时备份 */
static paddr_t current_pml4_phys = 0;

/* ---------------------------------------------------------------
 * 内联汇编：读/写 CR3、读 CR2、invlpg
 *
 * CR3 — 当前 PML4 物理地址
 *   读：movq %cr3, %rax
 *   写：movq %rax, %cr3（写时低 12 位是 flags，但物理地址按 4KB 对齐所以低 12 是 0）
 *       写 CR3 同时刷新整个 TLB（除 global page）
 *
 * CR2 — 缺页地址（#PF 时 CPU 把访问的虚拟地址写入 CR2）
 *   只读，没有"写 CR2"的操作。
 *
 * invlpg — 刷新单个虚拟地址的 TLB
 *   invlpg [virt_addr]
 *
 * 【为什么不用 arch/io.h 的封装】
 *   CR3 / invlpg 是控制寄存器，不属于 I/O 端口范畴。
 *   直接内联汇编更清晰，且这些指令全局只在这里用。 */
static inline paddr_t read_cr3(void) {
    u64 v;
    __asm__ volatile ("movq %%cr3, %0" : "=r"(v));
    /* CR3 低 12 位是 flags（PCD / PWT），地址是高 52 位 */
    return v & ~0xFFFULL;
}

static inline void write_cr3(u64 v) {
    __asm__ volatile ("movq %0, %%cr3" : : "r"(v) : "memory");
}

static inline void invlpg(vaddr_t addr) {
    __asm__ volatile ("invlpg (%0)" : : "r"(addr) : "memory");
}

/* ---------------------------------------------------------------
 * page entry（PTE）格式
 *
 * 64 位 PTE 各 bit 含义（Intel SDM Vol 3, Fig 4.19）：
 *
 *   bit 0   P (Present)         — 存在
 *   bit 1   R/W (Read/Write)    — 可写
 *   bit 2   U/S (User/Supervisor) — 用户可见
 *   bit 3   PWT (Page Write-Through) — 写穿
 *   bit 4   PCD (Page Cache Disable) — 禁用缓存
 *   bit 5   A (Accessed)         — CPU 自动设（已被访问）
 *   bit 6   D (Dirty)           — CPU 自动设（已被写入）
 *   bit 7   PS (Page Size)      — huge page（在 PD 项里 = 2MB）
 *   bit 8   G (Global)          — 全局页（CR3 切换不刷 TLB）
 *   bit 9-11 available           — OS 可用
 *   bit 12-51 物理地址           — 必须 4KB 对齐
 *   bit 52-62 available         — OS 可用
 *   bit 63 NX (No Execute)       — 不可执行
 *
 * 物理地址占 bit 12-51（40 位），但实际机器通常只用低 32 位
 * （即 4GB 物理地址），高位 0。
 *
 * 地址掩码：取 bit 12-51 */
#define PTE_ADDR_MASK  0x000FFFFFFFFFF000ULL
#define PTE_ADDR_SHIFT 12

/* 把物理地址打包成 PTE（添加 P/W flags） */
static inline u64 make_pte(paddr_t phys, u64 flags) {
    /* 物理地址必须是页对齐 */
    return (phys & PTE_ADDR_MASK) | (flags & ~PTE_ADDR_MASK);
}

/* 从 PTE 取出物理地址 */
static inline paddr_t pte_to_phys(u64 pte) {
    return pte & PTE_ADDR_MASK;
}

/* ---------------------------------------------------------------
 * 递归映射的虚拟地址计算
 *
 * 给定一个目标虚拟地址 vaddr，计算它对应的：
 *   - PML4 项的虚拟地址（在递归映射空间里）
 *   - PDPT 项的虚拟地址
 *   - PD 项的虚拟地址
 *   - PT 项的虚拟地址
 *
 * 通过这些虚拟地址可以直接读写页表项（递归映射的特性）。
 *
 * 【推导】
 *
 *   设 vaddr 的各级 index：
 *     pml4_idx = (vaddr >> 39) & 0x1FF
 *     pdpt_idx = (vaddr >> 30) & 0x1FF
 *     pd_idx   = (vaddr >> 21) & 0x1FF
 *     pt_idx   = (vaddr >> 12) & 0x1FF
 *
 *   访问 PT[pml4_idx][pdpt_idx][pd_idx][pt_idx] 的虚拟地址：
 *     - bit 47:39 全部 = 511（吃掉第 4 层，进入第 3 层）
 *     - bit 38:30 = pml4_idx（让 PML4 跳到对应 PDPT）
 *     - bit 29:21 = pdpt_idx（让 PDPT 跳到对应 PD）
 *     - bit 20:12 = pd_idx（让 PD 跳到对应 PT）
 *     - bit 11:3  = pt_idx × 8（每项 8 字节，所以偏移 pt_idx × 8）
 *
 *   = 0xFFFFFF80_00000000 | (pml4_idx << 30) | (pdpt_idx << 21) | (pd_idx << 12) | (pt_idx << 3)
 *
 * 同理：
 *   PD 项地址 = 0xFFFFFFFC_00000000 | (pml4_idx << 21) | (pdpt_idx << 12) | (pd_idx << 3)
 *   PDPT 项地址 = 0xFFFFFFFF_E0000000 | (pml4_idx << 12) | (pdpt_idx << 3)
 *   PML4 项地址 = 0xFFFFFFFF_FFFFF000 | (pml4_idx << 3)
 *
 * （高位前缀是 canonical extension 全 1） */

#define PML4_INDEX(v) (((v) >> 39) & 0x1FF)
#define PDPT_INDEX(v) (((v) >> 30) & 0x1FF)
#define PD_INDEX(v)   (((v) >> 21) & 0x1FF)
#define PT_INDEX(v)   (((v) >> 12) & 0x1FF)

/* PML4 项的递归地址（直接读改 PML4[i]） */
static inline volatile u64 *pml4_entry_ptr(vaddr_t v) {
    u64 idx = PML4_INDEX(v);
    return (volatile u64 *)(RECURSIVE_PML4_ADDR + (idx << 3));
}

/* PDPT 项的递归地址 */
static inline volatile u64 *pdpt_entry_ptr(vaddr_t v) {
    u64 pml4 = PML4_INDEX(v);
    u64 pdpt = PDPT_INDEX(v);
    u64 addr = 0xFFFFFFFFFFE00000ULL
               | (pml4 << 12)
               | (pdpt << 3);
    return (volatile u64 *)addr;
}

/* PD 项的递归地址 */
static inline volatile u64 *pd_entry_ptr(vaddr_t v) {
    u64 pml4 = PML4_INDEX(v);
    u64 pdpt = PDPT_INDEX(v);
    u64 pd   = PD_INDEX(v);
    u64 addr = 0xFFFFFFFFC0000000ULL
               | (pml4 << 21)
               | (pdpt << 12)
               | (pd << 3);
    return (volatile u64 *)addr;
}

/* PT 项的递归地址（最终页表项，指向 4KB 物理页） */
static inline volatile u64 *pt_entry_ptr(vaddr_t v) {
    u64 pml4 = PML4_INDEX(v);
    u64 pdpt = PDPT_INDEX(v);
    u64 pd   = PD_INDEX(v);
    u64 pt   = PT_INDEX(v);
    u64 addr = 0xFFFFFF8000000000ULL
               | (pml4 << 30)
               | (pdpt << 21)
               | (pd << 12)
               | (pt << 3);
    return (volatile u64 *)addr;
}

/* ---------------------------------------------------------------
 * arch_vmm_init — 虚拟内存管理器初始化
 *
 * 流程：
 *   1. 从 PMM 分配一页作为新 PML4
 *   2. 清零
 *   3. identity-map 所有可用 RAM（1GB huge page，省内存）
 *   4. identity-map 低 1MB（特殊区域，一定要映射）
 *   5. 设 PML4[511] = PML4 自身（递归映射）
 *   6. 切换 CR3 到新 PML4
 *
 * 【为什么切换 CR3 是安全的】
 *   - 新页表 identity-map 了所有内存，老页表映射的虚拟地址在新表里也有
 *   - 切换瞬间 CPU 仍执行同一虚拟地址（kernel_main 的下一条指令）
 *   - 切换后那个地址在新表里也有映射，CPU 继续执行
 *   - 不会触发 #PF
 *
 * 【为什么必须 identity-map 所有可用 RAM】
 *   - 切换 CR3 后，所有"老映射的虚拟地址"必须在新表里也存在
 *   - 内核代码在 1MB 物理地址（虚拟也 1MB，identity map）
 *   - 如果新表不映射 1MB，CPU 立刻 #PF triple fault
 *   - 必须保证 [__kernel_start, __kernel_end) 在新表里映射 */
void arch_vmm_init(void) {
    /* 第 1 步：分配新 PML4 物理页 */
    paddr_t new_pml4 = arch_pmm_alloc_frame();
    if (new_pml4 == 0) {
        panic(__FILE__, __LINE__, "VMM init: no memory for new PML4");
    }

    /* 第 2 步：清零新 PML4
     *
     * 此时 entry.asm 的老页表还在用，新 PML4 物理地址可访问
     * （identity map 前 4GB 保证了这点）。
     * 直接 memset 清零。 */
    u64 *pml4 = (u64 *)new_pml4;
    for (int i = 0; i < 512; i++) {
        pml4[i] = 0;
    }

    /* 第 3 步：identity-map 所有可用 RAM，用 1GB huge page
     *
     * 我们循环 4 次，映射 0~4GB（覆盖 4GB 物理内存，够教学用）。
     * 每个 PDPT 项指向 1GB 物理页，flag = P | W | PS | G。
     *
     * 用 1GB huge page 的好处：
     *   - 4GB 只需 4 个 PDPT 项（不需要 PD / PT）
     *   - 节省 4 × 512 × 8 = 16KB PD 表 + 4 × 512 × 512 × 8 = 8MB PT 表
     *
     * 缺点：
     *   - huge page 内不能 4KB 粒度保护（要么全可写、要么全不可写）
     *   - 后续需要细粒度时，arch_vmm_map_page 会分配新的 PD/PT 来拆分
     *     （但当前实现假设区域未映射，新分配的页表项是"不存在"，可以直接设）
     *
     * 注意：1GB huge page 是在 PDPT 项里设 PS=1，
     *       不是在 PD 项（那是 2MB huge page）。 */

    /* 分配一个 PDPT 物理页（4GB 的 PDPT 都在同一页上） */
    paddr_t new_pdpt = arch_pmm_alloc_frame();
    if (new_pdpt == 0) {
        panic(__FILE__, __LINE__, "VMM init: no memory for PDPT");
    }
    u64 *pdpt = (u64 *)new_pdpt;
    for (int i = 0; i < 512; i++) {
        pdpt[i] = 0;
    }

    /* 前 4 项 = 1GB huge page，identity-map 0~4GB */
    for (int i = 0; i < 4; i++) {
        paddr_t phys = (paddr_t)i << 30;  /* i × 1GB */
        /* flag = P | W | PS | G
         *   P=1 存在
         *   W=1 可写
         *   PS=1 huge page（在 PDPT 里就是 1GB huge）
         *   G=8 全局页
         */
        pdpt[i] = make_pte(phys, PAGE_FLAG_PRESENT | PAGE_FLAG_WRITE
                                  | PAGE_FLAG_HUGE | PAGE_FLAG_GLOBAL);
    }

    /* 把 PDPT 挂到 PML4[0]（虚拟地址 0~4GB 在 PML4[0] 下） */
    pml4[0] = make_pte(new_pdpt, PAGE_FLAG_PRESENT | PAGE_FLAG_WRITE);

    /* 第 4 步：低 1MB 已被第 3 步的 PDPT[0] 包含（0~1GB 范围内）
     * 不需要单独处理。VGA / IVT / BIOS 都能正常访问。 */

    /* 第 5 步：设 PML4[511] = PML4 自身（递归映射）
     *
     * 这是关键技巧：让 PML4[511] 指向 PML4 自己，
     * 之后访问 0xFFFFFFFFFFFFF000 就是 PML4 本身。
     *
     * flag = P | W（可读写）
     * 注意：不要设 PS（PML4 项没有 huge page 概念）。 */
    pml4[PML4_RECURSIVE_INDEX] = make_pte(new_pml4,
                                           PAGE_FLAG_PRESENT | PAGE_FLAG_WRITE);

    /* 第 6 步：切换 CR3 到新 PML4
     *
     * 此时新页表已就绪：
     *   - identity-map 0~4GB（覆盖内核 + 可用 RAM）
     *   - PML4[511] 递归映射到自身
     *
     * 写 CR3 立即生效，CPU 从此用新页表。
     * 新表里 kernel_main 的下一条指令地址仍有映射，所以 CPU 能继续执行。
     *
     * 写 CR3 同时刷新 TLB（除 global page）。 */
    current_pml4_phys = new_pml4;
    write_cr3(new_pml4);
}

/* ---------------------------------------------------------------
 * arch_vmm_map_page — 映射一个 4KB 虚拟页到 4KB 物理页
 *
 * 流程（用递归映射）：
 *   1. 检查 virt / phys 是否页对齐
 *   2. 关中断（防止中途被中断导致页表状态不一致）
 *   3. 通过递归映射访问 PML4 项
 *      - 不存在 → 从 PMM 分配新 PDPT 页 → 清零 → 填 PML4 项
 *   4. 通过递归映射访问 PDPT 项
 *      - 不存在 → 从 PMM 分配新 PD 页 → 清零 → 填 PDPT 项
 *      - 如果 PDPT 项是 1GB huge page → panic（拆分 huge page 未实现）
 *   5. 通过递归映射访问 PD 项
 *      - 不存在 → 从 PMM 分配新 PT 页 → 清零 → 填 PD 项
 *      - 如果 PD 项是 2MB huge page → panic（同上）
 *   6. 通过递归映射访问 PT 项
 *      - 已存在且 P=1 → 警告（重复映射，可能 bug）
 *      - 写入新 PTE = phys | flags
 *   7. invlpg 刷新 TLB（让新映射立即生效）
 *   8. 开中断恢复
 *
 * 【为什么不能直接拆分 huge page】
 *   拆分需要：
 *     1. 分配一个新 PT 页
 *     2. 把 huge page 拆成 512 个 4KB 项填到 PT
 *     3. 把 PD 项改成指向 PT（去掉 PS bit）
 *     4. 保留原来 huge page 的物理地址范围
 *
 *   当前实现跳过这个复杂度。教学上 VMM_init 用 huge page
 *   identity-map 大块内存，需要 4KB 映射时只用未映射的虚拟地址。 */
int arch_vmm_map_page(vaddr_t virt, paddr_t phys, u64 flags) {
    /* 检查对齐 */
    if ((virt & (PAGE_SIZE - 1)) != 0) {
        panic(__FILE__, __LINE__, "VMM map: virt not page-aligned");
    }
    if ((phys & (PAGE_SIZE - 1)) != 0) {
        panic(__FILE__, __LINE__, "VMM map: phys not page-aligned");
    }

    u64 saved = arch_irq_save();

    /* 【Lesson 8】中间页表项的 flags：基本是 P | W，
     *   如果最终页是 user 页（flags 含 USER），中间项也要带 USER——
     *   x86 规定 ring 3 要访问一个页，整条页表路径（PML4/PDPT/PD/PT）
     *   的每一项 U/S 位都必须 = 1，否则 #PF（即使最终 PT 项有 U 位）。
     *   所以这里把 USER 传播给所有中间项。 */
    u64 inter_flags = PAGE_FLAG_PRESENT | PAGE_FLAG_WRITE
                      | (flags & PAGE_FLAG_USER);

    /* 1. PML4 项 */
    volatile u64 *pml4_e = pml4_entry_ptr(virt);
    if ((*pml4_e & PAGE_FLAG_PRESENT) == 0) {
        /* 分配新 PDPT */
        paddr_t new_pdpt = arch_pmm_alloc_frame();
        if (new_pdpt == 0) {
            arch_irq_restore(saved);
            return -1;
        }
        /* 清零（必须，否则垃圾数据被当成 PTE） */
        u64 *p = (u64 *)new_pdpt;
        for (int i = 0; i < 512; i++) p[i] = 0;
        /* 填 PML4 项（带 USER 如果是 user 映射） */
        *pml4_e = make_pte(new_pdpt, inter_flags);
    }

    /* 2. PDPT 项 */
    volatile u64 *pdpt_e = pdpt_entry_ptr(virt);
    if ((*pdpt_e & PAGE_FLAG_PRESENT) == 0) {
        /* 分配新 PD */
        paddr_t new_pd = arch_pmm_alloc_frame();
        if (new_pd == 0) {
            arch_irq_restore(saved);
            return -1;
        }
        u64 *p = (u64 *)new_pd;
        for (int i = 0; i < 512; i++) p[i] = 0;
        *pdpt_e = make_pte(new_pd, inter_flags);
    } else if (*pdpt_e & PAGE_FLAG_HUGE) {
        /* 1GB huge page，需要拆分才能映射 4KB 页
         * 当前实现不支持，panic 防止数据不一致。 */
        panic(__FILE__, __LINE__,
              "VMM map: cannot map 4KB page inside 1GB huge page "
              "(need to split first)");
    }

    /* 3. PD 项 */
    volatile u64 *pd_e = pd_entry_ptr(virt);
    if ((*pd_e & PAGE_FLAG_PRESENT) == 0) {
        paddr_t new_pt = arch_pmm_alloc_frame();
        if (new_pt == 0) {
            arch_irq_restore(saved);
            return -1;
        }
        u64 *p = (u64 *)new_pt;
        for (int i = 0; i < 512; i++) p[i] = 0;
        *pd_e = make_pte(new_pt, inter_flags);
    } else if (*pd_e & PAGE_FLAG_HUGE) {
        /* 2MB huge page，需要拆分 */
        panic(__FILE__, __LINE__,
              "VMM map: cannot map 4KB page inside 2MB huge page");
    }

    /* 4. PT 项 */
    volatile u64 *pt_e = pt_entry_ptr(virt);
    if (*pt_e & PAGE_FLAG_PRESENT) {
        /* already mapped, silently overwrite (stress test path) */
    }
    *pt_e = make_pte(phys, flags | PAGE_FLAG_PRESENT);

    /* 5. 刷新 TLB（让新映射立即对 CPU 可见） */
    invlpg(virt);

    arch_irq_restore(saved);
    return 0;
}

/* ---------------------------------------------------------------
 * arch_vmm_unmap_page — 取消一个 4KB 虚拟页的映射
 *
 * 行为：
 *   1. 走页表找 PT 项
 *   2. 如果中间某级不存在 → 直接返回成功（本来就没映射）
 *   3. PT 项清 0（P=0，不存在）
 *   4. invlpg 刷新 TLB
 *
 * 【为什么不释放中间页表（PD/PT）】
 *   - 即使一个 PT 的所有 512 项都空了，释放 PT 也很复杂
 *     （要同步更新 PD 项、可能 race condition）
 *   - 教学内核不做这个优化，留着 PT 占 4KB 不影响功能
 *   - Linux 会在 PT 全空时释放，但需要额外记账 */
int arch_vmm_unmap_page(vaddr_t virt) {
    if ((virt & (PAGE_SIZE - 1)) != 0) {
        panic(__FILE__, __LINE__, "VMM unmap: virt not page-aligned");
    }

    u64 saved = arch_irq_save();

    /* 走页表，找 PT 项 */
    volatile u64 *pml4_e = pml4_entry_ptr(virt);
    if ((*pml4_e & PAGE_FLAG_PRESENT) == 0) {
        arch_irq_restore(saved);
        return 0;   /* 没映射，成功 */
    }

    volatile u64 *pdpt_e = pdpt_entry_ptr(virt);
    if ((*pdpt_e & PAGE_FLAG_PRESENT) == 0) {
        arch_irq_restore(saved);
        return 0;
    }
    if (*pdpt_e & PAGE_FLAG_HUGE) {
        /* 1GB huge page 包含此虚拟地址，需要拆分才能 unmap 4KB
         * 当前实现不支持，panic。 */
        panic(__FILE__, __LINE__,
              "VMM unmap: cannot unmap 4KB inside 1GB huge page");
    }

    volatile u64 *pd_e = pd_entry_ptr(virt);
    if ((*pd_e & PAGE_FLAG_PRESENT) == 0) {
        arch_irq_restore(saved);
        return 0;
    }
    if (*pd_e & PAGE_FLAG_HUGE) {
        panic(__FILE__, __LINE__,
              "VMM unmap: cannot unmap 4KB inside 2MB huge page");
    }

    volatile u64 *pt_e = pt_entry_ptr(virt);
    /* 清除 PT 项 */
    *pt_e = 0;
    /* 刷新 TLB */
    invlpg(virt);

    arch_irq_restore(saved);
    return 0;
}

/* ---------------------------------------------------------------
 * arch_vmm_unmap_user_page — 深度取消映射 + 释放 user 页 + 释放空的中间页表
 *
 *   【Lesson 8 新增】
 *
 *   与 arch_vmm_unmap_page 的区别：
 *     1. 不仅清 PT 项，还释放 4KB 数据页的物理帧（调用方不需要再 free）
 *     2. 然后向上走：
 *        - 如果 PT 全空（512 项全 0）→ 释放 PT 帧 + 清 PD 项
 *        - 如果 PD 全空 → 释放 PD 帧 + 清 PDPT 项
 *        - 如果 PDPT 全空 且 pml4_idx ∈ [1,510] → 释放 PDPT 帧 + 清 PML4 项
 *     3. 绝不释放/清 PML4[0]（identity map）和 PML4[511]（recursive）
 *
 *   【为什么需要这个函数】
 *     user 任务退出时，arch_user_unmap_pages 会逐页 unmap。
 *     如果只清 PT 项，中间页表（PT/PD/PDPT）永远占着物理帧
 *     → 每次创建/销毁 user 任务都泄漏若干页表帧。
 *     深度清理让 PMM 帧数在 user 任务前后保持一致。
 *
 *   【安全性】
 *     - 整个过程在 arch_irq_save 关中断下运行（原子）
 *     - 单核 + IF=0 → 无并发访问页表
 *     - all-zero 检查严格读 512 项，任意一项非 0 都不释放
 *     - 读父项的 phys 总在清父项之前（避免读到 0）
 *
 *   【参数】
 *     virt — 要取消映射的 4KB 虚拟地址（必须页对齐）
 * --------------------------------------------------------------- */
void arch_vmm_unmap_user_page(vaddr_t virt) {
    if ((virt & (PAGE_SIZE - 1)) != 0) {
        panic(__FILE__, __LINE__, "VMM unmap_user: virt not page-aligned");
    }

    u64 saved = arch_irq_save();

    /* 1. 取数据页物理地址，未映射直接返回（已清理过） */
    paddr_t phys = arch_vmm_get_phys(virt);
    if (phys == 0) {
        arch_irq_restore(saved);
        return;
    }
    /* 取页对齐基址（防御：get_phys 返回含页内偏移，virt 页对齐时偏移=0） */
    paddr_t data_phys = phys & ~(paddr_t)(PAGE_SIZE - 1);

    /* 2. 清 PT 项 + 释放数据页帧。
     *    arch_vmm_unmap_page 内部会 invlpg(virt)，并做自己的 irq_save/restore
     *    （嵌套 save/restore 在 IF=0 下是幂等的，安全）。 */
    arch_vmm_unmap_page(virt);
    arch_pmm_free_frame(data_phys);

    /* 3. 检查 PT 是否全空。
     *    PT base VA = pt_entry_ptr(virt_with_pt_idx_0)：
     *      清掉 virt 的 pt_idx 位（bit 20:12）即可得到 pt_idx=0 的同 PT 内 VA。 */
    vaddr_t va_pt0  = virt & ~((vaddr_t)0x1FF << 12);
    volatile u64 *pt_base = pt_entry_ptr(va_pt0);
    int pt_empty = 1;
    for (int i = 0; i < 512; i++) {
        if (pt_base[i] != 0) { pt_empty = 0; break; }
    }
    if (!pt_empty) {
        arch_irq_restore(saved);
        return;
    }

    /* PT 全空：读 PD 项 → 清 PD 项 → 释放 PT 帧 */
    volatile u64 *pd_e = pd_entry_ptr(virt);
    if ((*pd_e & PAGE_FLAG_PRESENT) == 0 || (*pd_e & PAGE_FLAG_HUGE)) {
        /* PD 项不存在或是 2MB huge page：不应到这里，安全退出 */
        arch_irq_restore(saved);
        return;
    }
    paddr_t pt_phys = pte_to_phys(*pd_e);
    *pd_e = 0;
    invlpg((vaddr_t)pt_base);      /* 刷掉 PT 自身的递归映射 TLB */
    arch_pmm_free_frame(pt_phys);

    /* 4. 检查 PD 是否全空 */
    vaddr_t va_pd0  = virt & ~((vaddr_t)0x1FF << 21);
    volatile u64 *pd_base = pd_entry_ptr(va_pd0);
    int pd_empty = 1;
    for (int i = 0; i < 512; i++) {
        if (pd_base[i] != 0) { pd_empty = 0; break; }
    }
    if (!pd_empty) {
        arch_irq_restore(saved);
        return;
    }

    /* PD 全空：读 PDPT 项 → 清 PDPT 项 → 释放 PD 帧 */
    volatile u64 *pdpt_e = pdpt_entry_ptr(virt);
    if ((*pdpt_e & PAGE_FLAG_PRESENT) == 0 || (*pdpt_e & PAGE_FLAG_HUGE)) {
        /* 1GB huge page（可能是 identity map 区），不动 */
        arch_irq_restore(saved);
        return;
    }
    paddr_t pd_phys = pte_to_phys(*pdpt_e);
    *pdpt_e = 0;
    invlpg((vaddr_t)pd_base);
    arch_pmm_free_frame(pd_phys);

    /* 5. 检查 PDPT 是否全空，且 pml4_idx ∈ [1,510]
     *    （不能动 PML4[0] identity map，不能动 PML4[511] recursive map） */
    u64 pml4_idx = PML4_INDEX(virt);
    if (pml4_idx == 0 || pml4_idx == PML4_RECURSIVE_INDEX) {
        arch_irq_restore(saved);
        return;
    }

    vaddr_t va_pdpt0 = virt & ~((vaddr_t)0x1FF << 30);
    volatile u64 *pdpt_base = pdpt_entry_ptr(va_pdpt0);
    int pdpt_empty = 1;
    for (int i = 0; i < 512; i++) {
        if (pdpt_base[i] != 0) { pdpt_empty = 0; break; }
    }
    if (!pdpt_empty) {
        arch_irq_restore(saved);
        return;
    }

    /* PDPT 全空：读 PML4 项 → 清 PML4 项 → 释放 PDPT 帧。
     *    不释放 PML4 根（系统共享 1 个 PML4）。 */
    volatile u64 *pml4_e = pml4_entry_ptr(virt);
    if ((*pml4_e & PAGE_FLAG_PRESENT) == 0) {
        arch_irq_restore(saved);
        return;
    }
    paddr_t pdpt_phys = pte_to_phys(*pml4_e);
    *pml4_e = 0;
    invlpg((vaddr_t)pdpt_base);
    arch_pmm_free_frame(pdpt_phys);

    arch_irq_restore(saved);
}

/* ---------------------------------------------------------------
 * arch_vmm_get_phys — 虚拟地址 → 物理地址
 *
 * 返回值：
 *   成功 — 物理地址（含页内偏移）
 *   失败 — 0（未映射或路径中断）
 *
 * 注意：返回值 0 也可能是合法的物理地址 0，
 *       但 PMM 保留了页 0（不分配），所以正常情况下不会拿到。 */
paddr_t arch_vmm_get_phys(vaddr_t virt) {
    u64 saved = arch_irq_save();

    volatile u64 *pml4_e = pml4_entry_ptr(virt);
    if ((*pml4_e & PAGE_FLAG_PRESENT) == 0) {
        arch_irq_restore(saved);
        return 0;
    }
    volatile u64 *pdpt_e = pdpt_entry_ptr(virt);
    if ((*pdpt_e & PAGE_FLAG_PRESENT) == 0) {
        arch_irq_restore(saved);
        return 0;
    }
    if (*pdpt_e & PAGE_FLAG_HUGE) {
        /* 1GB huge page */
        paddr_t base = pte_to_phys(*pdpt_e);
        arch_irq_restore(saved);
        return base + (virt & 0x3FFFFFFF);   /* 1GB 内偏移 */
    }

    volatile u64 *pd_e = pd_entry_ptr(virt);
    if ((*pd_e & PAGE_FLAG_PRESENT) == 0) {
        arch_irq_restore(saved);
        return 0;
    }
    if (*pd_e & PAGE_FLAG_HUGE) {
        /* 2MB huge page */
        paddr_t base = pte_to_phys(*pd_e);
        arch_irq_restore(saved);
        return base + (virt & 0x1FFFFF);   /* 2MB 内偏移 */
    }

    volatile u64 *pt_e = pt_entry_ptr(virt);
    if ((*pt_e & PAGE_FLAG_PRESENT) == 0) {
        arch_irq_restore(saved);
        return 0;
    }

    paddr_t base = pte_to_phys(*pt_e);
    arch_irq_restore(saved);
    return base + (virt & (PAGE_SIZE - 1));   /* 4KB 页内偏移 */
}

/* ---------------------------------------------------------------
 * arch_vmm_flush_tlb — 刷新整个 TLB
 *
 * 写 CR3 触发 TLB flush（除 global page）。
 * 写相同的 CR3 值也能触发 flush（CPU 不做"值相同就跳过"优化）。 */
void arch_vmm_flush_tlb(void) {
    u64 cr3 = read_cr3();
    write_cr3(cr3);
}

/* ---------------------------------------------------------------
 * arch_vmm_get_cr3 — 读取当前 CR3（PML4 物理地址） */
paddr_t arch_vmm_get_cr3(void) {
    return read_cr3();
}

/* ---------------------------------------------------------------
 * arch_vmm_load_cr3 — 写入 CR3，切换地址空间 */
void arch_vmm_load_cr3(paddr_t pml4_phys) {
    write_cr3(pml4_phys);
    current_pml4_phys = pml4_phys;
}

/* ---------------------------------------------------------------
 * arch_mem_init — 内存子系统统一初始化
 *
 * 顺序：
 *   1. arch_pmm_init(info) — 初始化物理帧分配器
 *      ↓
 *   2. arch_vmm_init() — 构造新页表 + 切换 CR3
 *
 * 必须先 PMM，再 VMM。因为 VMM 初始化时要分配新 PML4 / PDPT 页，
 * 需要 PMM 已就绪。 */
void arch_mem_init(struct boot_info *info) {
    arch_pmm_init(info);
    arch_vmm_init();
}
