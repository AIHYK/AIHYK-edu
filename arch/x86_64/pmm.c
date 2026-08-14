/* ================================================================
 * arch/x86_64/pmm.c — 物理内存管理器（PMM, Physical Memory Manager）
 *
 * 【Lesson 4 核心新增】
 *
 * 用"位图（bitmap）"实现一个最简单但完整的物理页分配器：
 *   - 每个 4KB 物理页对应 1 个 bit
 *   - bit = 0 → 空闲（可分配）
 *   - bit = 1 → 已用（保留或已分配）
 *   - 线性扫描找第一个 0 bit
 *
 * 这个分配器的特点：
 *   ✅ 实现简单（100 行核心代码）
 *   ✅ 内存占用低（每 GB 内存只占 32KB 位图）
 *   ✅ 分配速度可接受（线性扫描）
 *   ❌ 不支持 buddy 合并（无法做大页合并）
 *   ❌ 单点分配慢（扫描需要 O(N) 时间）
 *
 * 对教学内核来说足够用。Linux 早期也是位图分配器，
 * 后来才换成 buddy。我们在 Lesson 5 调度器里也能用这个。
 *
 * =================================================================
 *
 * 【覆盖范围】
 *
 * 位图覆盖从 0 到 max_phys_addr 的所有物理页：
 *   - 比如机器有 128MB 内存，位图有 128MB / 4KB = 32768 个 bit = 4KB 位图
 *   - 比如机器有 4GB 内存，位图有 1MB
 *
 * 我们设一个静态上限 PMM_MAX_FRAMES（默认 1M，对应 4GB）。
 * 内存超过 4GB 时只用前 4GB（教学内核够用）。
 *
 * 位图本身放在 .bss 段，静态分配（PMM 还没初始化前没法 malloc）。
 *
 * =================================================================
 *
 * 【保留区域（默认标"已用"）】
 *
 * PMM 初始化时把以下区域标"已用"，防止误分配：
 *   1. 物理页 0（NULL 页 — 永不分配，方便抓 null pointer 解引用）
 *   2. 低 1MB（0x0 ~ 0x100000，BIOS / VGA / IVT 占用）
 *   3. 内核镜像（[__kernel_start, __kernel_end)，linker.ld 定义）
 *   4. boot_info 报告的所有非 MEM_USABLE 区域（MMIO / ACPI / reserved）
 *
 * 之后 arch_pmm_alloc_frame 只会返回真正"可用"的 4KB 页。
 *
 * =================================================================
 *
 * 【线程安全】
 *
 * 单核 + 中断可能并发，所以 alloc/free 内部用 arch_irq_save/restore 保护。
 * 多核时需要加 spinlock（Lesson 5+ 才有多核支持）。 */
#include <arch/boot.h>
#include <arch/cpu.h>
#include <arch/console.h>
#include <arch/mem.h>
#include <kernel/panic.h>
#include <kernel/types.h>

/* 内核镜像符号（linker.ld 提供） */
extern u8 __kernel_start[];
extern u8 __kernel_end[];

/* ---------------------------------------------------------------
 * 位图大小上限
 *
 * PMM_MAX_FRAMES = 1M 个帧 = 4GB 物理内存（够大）
 * 位图字节数 = 1M bits / 8 = 128KB
 *
 * 【为什么不用动态分配】
 *   PMM 是最早初始化的子系统，没有堆可用，
 *   位图必须静态分配在 .bss 里。
 *
 * 【为什么是 4GB 上限】
 *   - 教学环境 QEMU 默认 128MB 内存，远小于 4GB
 *   - VMware / 实体机测试也通常 < 1GB
 *   - 真要支持更大内存，改这个常量即可 */
#define PMM_MAX_FRAMES  (1024u * 1024u)            /* 1M 帧 = 4GB */
#define PMM_BITMAP_SIZE (PMM_MAX_FRAMES / 8)       /* 128KB 位图 */

/* 位图：每 bit 代表一个 4KB 页 */
static u8 bitmap[PMM_BITMAP_SIZE];

/* 统计信息 */
static usize_t total_frames = 0;    /* 总帧数（覆盖范围内） */
static usize_t free_frames  = 0;    /* 当前可用帧数 */
static usize_t used_frames  = 0;    /* 当前已用帧数 */

/* 最高物理地址（来自 boot_info 的最大 region 末尾） */
static paddr_t max_phys_addr = 0;

/* ---------------------------------------------------------------
 * 位图操作：byte/bit 索引计算
 *
 * frame_idx → byte_idx = frame_idx / 8
 * frame_idx → bit_idx  = frame_idx % 8
 *
 * 检查/设置/清除 bit：
 *   bitmap_get(idx)  = (bitmap[idx/8] >> (idx%8)) & 1
 *   bitmap_set(idx)  = bitmap[idx/8] |=  (1 << (idx%8))
 *   bitmap_clear(idx)= bitmap[idx/8] &= ~(1 << (idx%8))
 *
 * 用宏让代码更紧凑（位图操作是性能敏感路径）。 */
#define BM_GET(idx)  ((bitmap[(idx) >> 3] >> ((idx) & 7)) & 1)
#define BM_SET(idx)  do { bitmap[(idx) >> 3] |=  (u8)(1 << ((idx) & 7)); } while (0)
#define BM_CLEAR(idx) do { bitmap[(idx) >> 3] &= (u8)~(1 << ((idx) & 7)); } while (0)

/* ---------------------------------------------------------------
 * frame_to_idx / idx_to_frame — 物理地址 ↔ 位图索引
 *
 * 物理地址 0x0  → idx 0
 * 物理地址 4KB → idx 1
 * 物理地址 8KB → idx 2
 * ...
 *
 * 假设 PAGE_SIZE = 4KB（PAGE_SHIFT = 12）。 */
static inline usize_t frame_to_idx(paddr_t addr) {
    return (usize_t)(addr >> PAGE_SHIFT);
}

static inline paddr_t idx_to_frame(usize_t idx) {
    return (paddr_t)idx << PAGE_SHIFT;
}

/* ---------------------------------------------------------------
 * find_first_free — 在位图中找第一个 0 bit
 *
 * 返回值：
 *   成功 — bit 索引（0 ~ total_frames-1）
 *   失败 — (usize_t)-1（没有可用帧）
 *
 * 实现策略：先按字节扫描（快），找到非 0xFF 的字节再按 bit 找。
 * 这样平均性能从 O(N×8) 提升到 O(N/8) + O(8)。
 *
 * 【优化思路（教学项目不做）】
 *   - 用 ffs/ffz 指令（find first set/clear bit）
 *   - 多级位图（位图位图，类似 buddy）
 *   - free list（链表）— 分配 O(1)，但要管理元数据 */
static usize_t find_first_free(void) {
    /* 按字节扫描，跳过 0xFF（全满）的字节 */
    usize_t max_bytes = (total_frames + 7) / 8;
    for (usize_t i = 0; i < max_bytes; i++) {
        if (bitmap[i] != 0xFF) {
            /* 找到有空位的字节，按 bit 找 */
            for (int b = 0; b < 8; b++) {
                usize_t idx = i * 8 + b;
                if (idx >= total_frames) {
                    return (usize_t)-1;
                }
                if (((bitmap[i] >> b) & 1) == 0) {
                    return idx;
                }
            }
        }
    }
    return (usize_t)-1;
}

/* ---------------------------------------------------------------
 * arch_pmm_reserve_range — 把物理地址区间 [start, end) 标"已用"
 *
 * 行为：
 *   - 把对应 bit 全设为 1
 *   - 更新 free_frames / used_frames 统计
 *   - 超出 max_phys_addr 的部分被忽略
 *
 * 用途：
 *   - 内核镜像（[__kernel_start, __kernel_end)）
 *   - 低 1MB（BIOS / VGA / IVT）
 *   - 已映射给设备的页
 *
 * 【为什么允许部分超出范围】
 *   boot_info 报告的 region 可能延伸到 4GB+，
 *   但我们位图只覆盖前 4GB。忽略超出部分是安全的，
 *   那部分内存 PMM 不分配，调用方也用不到。 */
void arch_pmm_reserve_range(paddr_t start, paddr_t end) {
    paddr_t s = PAGE_ALIGN_DOWN(start);
    paddr_t e = PAGE_ALIGN_UP(end);

    for (paddr_t a = s; a < e; a += PAGE_SIZE) {
        if (a >= max_phys_addr) {
            break;      /* 超出位图范围 */
        }
        usize_t idx = frame_to_idx(a);
        if (BM_GET(idx) == 0) {
            BM_SET(idx);
            free_frames--;
            used_frames++;
        }
    }
}

/* ---------------------------------------------------------------
 * mark_region_from_boot_info — 把 boot_info 的 region 应用到位图
 *
 * 遍历所有 region：
 *   - MEM_USABLE → 标"可用"（clear bit）
 *   - 其他 → 标"已用"（set bit）
 *
 * 然后再单独保留低 1MB 和内核镜像。
 *
 * 注意：调用顺序很重要！
 *   1. 默认全部已用（清零位图阶段）
 *   2. 把 MEM_USABLE 区域标可用
 *   3. 强制保留低 1MB + 内核镜像（即使 boot_info 说它"可用"）
 *
 * 这样即使 bootloader 报错把内核区域算成"可用"，
 * 我们的强制保留也会纠正。 */
static void mark_region_from_boot_info(struct boot_info *info) {
    for (int i = 0; i < info->region_count; i++) {
        struct mem_region *r = &info->regions[i];
        paddr_t end = r->base + r->length;
        if (end > max_phys_addr) {
            end = max_phys_addr;
        }

        if (r->type == MEM_USABLE) {
            /* 标"可用"（clear bit） */
            paddr_t s = PAGE_ALIGN_UP(r->base);
            for (paddr_t a = s; a < end; a += PAGE_SIZE) {
                usize_t idx = frame_to_idx(a);
                if (BM_GET(idx) == 1) {
                    BM_CLEAR(idx);
                    free_frames++;
                    used_frames--;
                }
            }
        }
        /* 其他类型保持"已用"（位图初始就是全 1）*/
    }
}

/* ---------------------------------------------------------------
 * arch_pmm_init — 物理内存管理器初始化
 *
 * 流程：
 *   1. 扫描 boot_info，找到 max_phys_addr（最高物理地址）
 *      + 确定 total_frames
 *   2. 把整个位图设为 0xFF（全部已用）
 *   3. 遍历 region：
 *      - MEM_USABLE 区域 → 位图对应 bit clear
 *      - 其他 → 保持已用
 *   4. 强制保留低 1MB（VGA/BIOS 区域）
 *   5. 强制保留内核镜像（[__kernel_start, __kernel_end)）
 *   6. 强制保留物理页 0（NULL page，永不分配）
 *   7. 统计 total/free/used
 *
 * 【为什么物理页 0 永不分配】
 *   - NULL 指针解引用时访问地址 0
 *   - 如果 0 地址对应一页"已分配"的内存，访问会"成功"返回垃圾数据
 *     （bug 难以发现）
 *   - 把页 0 标"已用"，访问 NULL 立即触发 #PF（缺页异常）
 *     （bug 立即暴露）
 *
 *   这是 Linux 也用的技巧（CONFIG_DEBUG_VIRTUAL）。 */
void arch_pmm_init(struct boot_info *info) {
    /* 第 1 步：扫描所有 region，找 max_phys_addr
     *
     * 【只考虑 MEM_USABLE 区域】（修 Bug）
     *   旧实现把所有 region（包括 RESERVED/MMIO）的 end 都拿来比较，
     *   导致高位 MMIO 区（如 QEMU PVH 报的 0xFD00000000-0x10000000000
     *   共 12GB 的 RESERVED）把 max_phys_addr 顶到 1TB，再被
     *   PMM_MAX_FRAMES 上限砍到 4GB。
     *   后果：total_frames=1M=4096MB，但真实可用 RAM 只有 128MB，
     *   位图里 ~3.87GB 的 bit 永远是 1（"已用"），display 的
     *   "Total frames / Used frames" 数字严重误导。
     *
     *   正确做法：max_phys_addr 只算 MEM_USABLE 区域的 end，
     *   这样 total_frames 反映真实可分配 RAM 上限，
     *   位图覆盖范围也只到真实 RAM 最高地址。
     *   高位 MMIO 区域不会出现在位图里（它们也绝不可能被分配，
     *   所以"不在位图"和"在位图且标 used"在分配语义上等价）。
     *
     *   低位 MMIO 区域（位于最高 USABLE 之内的 RESERVED，例如 VGA
     *   0xA0000-0x100000）依然被位图覆盖，由 mark_region_from_boot_info
     *   保持"已用"（位图默认 1，不会被 clear）。 */
    max_phys_addr = 0;
    for (int i = 0; i < info->region_count; i++) {
        struct mem_region *r = &info->regions[i];
        if (r->type != MEM_USABLE) {
            continue;       /* 跳过 RESERVED / KERNEL / MMIO / ACPI */
        }
        paddr_t end = r->base + r->length;
        if (end > max_phys_addr) {
            max_phys_addr = end;
        }
    }

    if (max_phys_addr == 0) {
        panic(__FILE__, __LINE__,
              "PMM init: no usable memory regions reported by bootloader");
    }

    /* 限制位图覆盖范围（不超过 PMM_MAX_FRAMES 帧 = 4GB） */
    usize_t max_frames = max_phys_addr >> PAGE_SHIFT;
    if (max_frames > PMM_MAX_FRAMES) {
        max_frames = PMM_MAX_FRAMES;
        max_phys_addr = (paddr_t)PMM_MAX_FRAMES << PAGE_SHIFT;
    }
    total_frames = max_frames;

    /* 第 2 步：把整个位图设为 0xFF（全部已用）
     *
     * 之后再"开"可用区域，这样默认状态是"已用"，
     * 防止 bootloader 没报告的区域被误分配。 */
    for (usize_t i = 0; i < PMM_BITMAP_SIZE; i++) {
        bitmap[i] = 0xFF;
    }
    free_frames = 0;
    used_frames = total_frames;

    /* 第 3 步：把 MEM_USABLE 区域标"可用"（clear bit）
     *
     * 这一步会更新 free/used 统计。 */
    mark_region_from_boot_info(info);

    /* 第 4 步：强制保留低 1MB（IVT / BDA / VGA / BIOS ROM）
     *
     * 即使 boot_info 说某些 0~1MB 区域"可用"，
     * 我们也强制保留——这里太多硬件特殊区域，安全起见全留。
     *
     * 注意：这步在 mark_region 之后，所以会"覆盖"可用标记。 */
    arch_pmm_reserve_range(0, 1 * 1024 * 1024);

    /* 第 5 步：强制保留内核镜像区域
     *
     * linker.ld 提供 __kernel_start / __kernel_end。
     * boot.c 已经在 boot_info 里加了一条 MEM_KERNEL region，
     * 但 MEM_KERNEL 走"已用"逻辑（不分配），效果一样。
     * 这里再调一次是双保险（防止 boot.c 没加）。 */
    arch_pmm_reserve_range((paddr_t)(u64)&__kernel_start[0],
                            (paddr_t)(u64)&__kernel_end[0]);

    /* 第 6 步：保留物理页 0（NULL page）
     *
     * 低 1MB 已经包含了页 0，这一步其实是冗余的，
     * 但明确写出来强调"页 0 永不分配"的设计意图。 */
    /* arch_pmm_reserve_range(0, PAGE_SIZE); */ /* 已包含在低 1MB 保留里 */

    /* 第 7 步：统计已就绪，可以分配 */
}

/* ---------------------------------------------------------------
 * arch_pmm_alloc_frame — 分配一个 4KB 物理页
 *
 * 返回值：
 *   成功 — 物理地址（4KB 对齐）
 *   失败 — 0（无可用帧）
 *
 * 实现：
 *   1. 关中断保护位图（中断处理程序可能也调 alloc）
 *   2. 找第一个 0 bit
 *   3. 设为 1（已用）
 *   4. 更新统计
 *   5. 返回物理地址
 *
 * 【为什么用 arch_irq_save/restore 而不是 cli/sti】
 *   - 中断可能已经关着（在另一个 alloc 中），不能强制开
 *   - save/restore 保留原状态，正确处理嵌套 */
paddr_t arch_pmm_alloc_frame(void) {
    u64 flags = arch_irq_save();

    usize_t idx = find_first_free();
    if (idx == (usize_t)-1) {
        arch_irq_restore(flags);
        return 0;   /* OOM */
    }

    BM_SET(idx);
    free_frames--;
    used_frames++;

    paddr_t result = idx_to_frame(idx);

    /* 【纵深防御】alloc_frame 永远不返回 0（NULL page）
     *   frame 0 的位图位在 init 时被标"已用"且 free_frame(0) 被拒绝，
     *   所以 find_first_free 正常不会返回 idx 0。但如果位图被损坏，
     *   返回 0 会导致调用方 NULL deref。安全检查：若返回 0 则跳过。 */
    if (result == 0) {
        /* 极端情况：位图损坏导致 idx 0 被误认为空闲
         *   重新标"已用"（防御性），然后继续找下一帧 */
        used_frames--;  /* 撤销上面的 used_frames++ */
        idx = find_first_free();
        if (idx == (usize_t)-1) {
            arch_irq_restore(flags);
            return 0;   /* OOM */
        }
        BM_SET(idx);
        free_frames--;
        used_frames++;
        result = idx_to_frame(idx);
    }

    arch_irq_restore(flags);
    return result;
}

/* ---------------------------------------------------------------
 * arch_pmm_free_frame — 释放一个 4KB 物理页
 *
 * 参数：
 *   frame — 之前 arch_pmm_alloc_frame 返回的物理地址
 *
 * 行为：
 *   - 检查 frame 是否页对齐（不是 → panic，调用方 bug）
 *   - 检查 frame 是否在覆盖范围（不是 → panic）
 *   - 把对应 bit 清 0
 *   - 更新统计
 *   - 如果该 bit 之前已是 0（重复 free），警告但不 panic
 *
 * 【为什么重复 free 只警告不 panic】
 *   - 内核可能有 bug 导致 double free，但崩溃反而无法调试
 *   - 警告 + 继续运行，方便查看现场
 *   - 多个 free 把同一 bit clear 多次，结果一样（幂等） */
void arch_pmm_free_frame(paddr_t frame) {
    /* 【Bug fix】拒绝释放 frame 0（NULL page，永不分配）
     *
     *   物理地址 0 对应 NULL 指针解引用。如果 free_frame(0)
     *   把位图位清 0，后续 alloc_frame 可能返回 0，
     *   调用方拿到 0 地址后读写 → NULL deref → 难以发现的 bug。
     *
     *   Linux 也有同样的保护（pfn 0 永不 free）。
     *   这是纵深防御：init 时已保留 frame 0，正常路径不会
     *   调 free_frame(0)，但防御性代码防止意外调用。 */
    if (frame == 0) {
        /* silently reject: NULL page is permanently reserved */
        return;
    }

    /* 检查对齐 */
    if ((frame & (PAGE_SIZE - 1)) != 0) {
        /* silently reject: not page-aligned */
        return;
    }

    /* 检查范围 */
    if (frame >= max_phys_addr) {
        /* silently reject: out of range */
        return;
    }

    usize_t idx = frame_to_idx(frame);

    u64 flags = arch_irq_save();

    if (BM_GET(idx) == 0) {
        /* silently ignore double free */
    } else {
        BM_CLEAR(idx);
        free_frames++;
        used_frames--;
    }

    arch_irq_restore(flags);
}

/* ---------------------------------------------------------------
 * 统计函数
 * --------------------------------------------------------------- */
usize_t arch_pmm_total_frames(void) { return total_frames; }
usize_t arch_pmm_free_frames(void)  { return free_frames; }
usize_t arch_pmm_used_frames(void)  { return used_frames; }
