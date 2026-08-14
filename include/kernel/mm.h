/* ================================================================
 * kernel/mm.h — 内核堆分配器（kmalloc / kfree）架构无关接口
 *
 * 【Lesson 4 核心新增】
 *
 * 提供类似 C 标准库 malloc/free 的接口，但用于内核：
 *   kmalloc(size)  — 分配 size 字节，返回虚拟地址指针
 *   kfree(ptr)     — 释放 kmalloc 返回的指针
 *   kcalloc        — 分配并清零
 *   krealloc       — 调整大小（可选实现）
 *
 * 这个文件【架构无关】：
 *   - 不依赖 x86 特有结构
 *   - 通过 arch_pmm_alloc_frame / arch_vmm_map_page 操作具体硬件
 *   - 任何架构（x86-64 / RISC-V / ARM）都能用同一份 kernel/mm.c
 *
 * =================================================================
 *
 * 【为什么需要 kmalloc】
 *
 *   PMM 只能分配整 4KB 物理页，但内核很多数据结构小于 4KB：
 *     - struct task_struct（约 200 字节）
 *     - struct file（约 100 字节）
 *     - 字符串、临时缓冲区
 *
 *   如果每次都分配 4KB，浪费严重（一个 200 字节结构占 4KB，
 *   利用率 5%）。kmalloc 提供任意字节大小的分配。
 *
 * =================================================================
 *
 * 【分配器设计：first-fit 链表】
 *
 *   我们用最简单的"first-fit 单链表"分配器：
 *
 *     ┌────────────┐ ┌────────────┐ ┌────────────┐
 *     │ free block │→│ free block │→│ free block │→ NULL
 *     │ header:    │ │ header:    │ │ header:    │
 *     │  size, next│ │  size, next│ │  size, next│
 *     └────────────┘ └────────────┘ └────────────┘
 *
 *   - 堆内存预先分配一大块（用 PMM + VMM 准备）
 *   - 一开始是一个大 free block
 *   - kmalloc 时：遍历链表，找第一个 size 够的 block
 *     - 如果 size 完全匹配 → 整块切出，从链表移除
 *     - 如果 size 比要求大很多 → 拆成两半，前半给调用方，后半留链表
 *   - kfree 时：把 block 重新加回链表，尝试和邻居合并
 *
 *   这个分配器的特点：
 *     ✅ 实现简单
 *     ✅ 任意大小都能分配
 *     ✅ 分配速度快（first-fit，扫描到第一个够大就停）
 *     ❌ 容易产生外部碎片（小空隙难复用）
 *     ❌ 合并邻居 block 需要双向链表（实现简化）
 *
 *   Linux 早期用类似分配器（linker list + best-fit），
 *   后来换成 slab + buddy，但 first-fit 在教学项目里完全够用。 */
#ifndef KERNEL_MM_H
#define KERNEL_MM_H

#include <kernel/types.h>

/* ---------------------------------------------------------------
 * kmalloc — 分配 size 字节内存
 *
 * 参数：
 *   size — 要分配的字节数（> 0）
 *
 * 返回值：
 *   成功 — 虚拟地址指针（16 字节对齐）
 *   失败 — NULL（堆内存不足）
 *
 * 【为什么 kmalloc 用虚拟地址而不是物理地址】
 *   - 内核代码读写时用虚拟地址（CPU 自动走页表）
 *   - 物理地址对调用方没用（不能直接 deref）
 *   - kmalloc 内部把 PMM 分配的物理页通过 VMM 映射到虚拟地址
 *
 * 【对齐保证】
 *   返回的指针 16 字节对齐（System V AMD64 ABI 要求），
 *   能放任何标准类型（最大对齐是 long double，16 字节）。 */
void *kmalloc(usize_t size);

/* ---------------------------------------------------------------
 * kfree — 释放 kmalloc 分配的内存
 *
 * 参数：
 *   ptr — kmalloc 返回的指针（NULL 时无操作）
 *
 * 行为：
 *   - 把 block 加回 free 链表
 *   - 如果可能，合并相邻 free block
 *   - 不会立即归还物理页给 PMM（简化设计）
 *
 * 【为什么传 size 也行（kfree(ptr, size)）】
 *   有些实现要求传 size，因为 free block header 里没有 size 信息。
 *   我们在 header 里存了 size，所以 kfree(ptr) 就够。 */
void kfree(void *ptr);

/* ---------------------------------------------------------------
 * kcalloc — 分配并清零
 *
 * 等价于 kmalloc(nmemb * size) + memset(0)
 * 检查乘法溢出。 */
void *kcalloc(usize_t nmemb, usize_t size);

/* ---------------------------------------------------------------
 * krealloc — 调整已分配块的大小
 *
 * 行为：
 *   - ptr = NULL → 等价于 kmalloc(size)
 *   - size = 0   → 等价于 kfree(ptr)，返回 NULL
 *   - 新块更小或更大 → 统一"分配新块 + 拷贝旧数据 + 释放旧块"
 *
 * 【C3 修复】原文档声称"新块更小 → 原地截断"，但实现总是
 *   malloc + memcpy + free（见 kernel/mm.c:354-388），从不原地截断。
 *   文档与代码不符是教学致命伤，此处改为如实描述。
 *
 * 【为什么总是分配新块（不做原地截断）】
 *   原地截断需要：检查当前 block 是否够大 → 修改 header.size →
 *   把截下来的尾段作为新 free block 挂回 free_list。
 *   实现不难但需要谨慎处理 free_list 指针，简化教学版不做。
 *   性能影响：realloc 频率低（主要在 IPC 缓冲区扩展），可接受。
 *
 * 【为什么不在 free 链表里就地扩展（grow in place）】
 *   就地扩展需要检查"下一个 block 是否 free 且够大"，实现复杂。
 *   简单实现总是分配新块，效率稍低但正确。 */
void *krealloc(void *ptr, usize_t size);

/* ---------------------------------------------------------------
 * 内部初始化（kernel_main 调用，不是 arch_mem_init）
 *
 *   - 分配堆所需的物理页（HEAP_PAGES 个 4KB 页）
 *   - 通过 VMM 映射到 [heap_virt_start, heap_virt_end) 虚拟地址
 *   - 把整块设为一个大的 free block
 *
 *   如果 arch_mem_init 没先调用，kmalloc 会 panic。 */
void kernel_mm_init(void);

/* ---------------------------------------------------------------
 * 统计函数（调试用）
 *
 *   kernel_mm_total()  — 堆总字节数
 *   kernel_mm_used()   — 已分配字节数
 *   kernel_mm_free()   — 剩余可用字节数（含碎片）
 *   kernel_mm_blocks() — 已分配的 block 数 */
usize_t kernel_mm_total(void);
usize_t kernel_mm_used(void);
usize_t kernel_mm_free(void);
usize_t kernel_mm_blocks(void);

#endif /* KERNEL_MM_H */
