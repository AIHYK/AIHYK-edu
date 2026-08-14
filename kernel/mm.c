/* ================================================================
 * kernel/mm.c — 内核堆分配器（first-fit 链表实现）
 *
 * 【Lesson 4 核心新增】
 *
 * 实现 include/kernel/mm.h 的接口。
 *
 * 堆内存布局：
 *
 *   ┌──────────────────────────────────────────────────────────┐
 *   │ heap_pages × 4KB 连续虚拟地址                              │
 *   │ 一个大 free block（含 header + data）                    │
 *   └──────────────────────────────────────────────────────────┘
 *   ↑                                                          ↑
 *   heap_virt_start                                         heap_virt_end
 *
 * 分配流程：
 *   1. 遍历 free 链表
 *   2. 找第一个 size >= (req + header) 的 block
 *   3. 如果剩余空间能放下一个最小 block → 拆分
 *      否则整块切出
 *   4. 从链表移除该 block，返回 data 区域指针
 *
 * 释放流程：
 *   1. 计算 block 起始地址（ptr - sizeof(header)）
 *   2. 把 block 加回 free 链表头部
 *   3. 尝试和邻居合并（合并相邻的 free block）
 *
 * block header 布局（16 字节，对齐到 16 边界）：
 *   ┌──────────────┐
 *   │ magic (u32)   │  ← 校验值，检测 double free / 越界写
 *   │ pad  (u32)    │  ← padding 到 8 字节
 *   │ size  (usize) │  ← block 总大小（含 header）
 *   │ next  (ptr)   │  ← 下一个 free block 指针（仅 free 时有效）
 *   └──────────────┘
 *
 * 【magic 校验值设计】
 *   0xDEADBEEF = 已分配（in use）
 *   0xCAFEBABE = 已释放（in free list）
 *
 *   kfree 时检查 magic：
 *     - 不是 DEADBEEF → 不是我们分配的，panic
 *     - 是 CAFEBABE → double free，panic
 *   帮助抓 double free / 野指针 / 越界写。 */
#include <arch/mem.h>
#include <arch/console.h>
#include <kernel/mm.h>
#include <kernel/panic.h>
#include <kernel/types.h>

/* 内核镜像结束符号（linker.ld） */
extern u8 __kernel_end[];

/* ---------------------------------------------------------------
 * 堆配置
 *
 *   HEAP_PAGES    = 256 个 4KB 页 = 1MB 堆
 *                   （够内核用，不浪费物理内存）
 *
 *   heap_virt_start = 把堆映射到的虚拟地址
 *
 *   选择 0xFFFF800000000000（PML4[256]，canonical 高位半区起点）：
 *     - PML4[0]   被 identity-map（0~4GB）占用
 *     - PML4[511] 被递归映射占用（指向自身）
 *     - PML4[256..510] 是高位可用区域
 *
 *   0xFFFF800000000000 是 64 位规范高位地址的起点
 *     （bit 47 = 1，bits 48-63 全 1 → canonical high half）
 *     不与现有映射冲突，且在虚拟地址空间"高半区"，
 *     内核空间习惯放在这里，远离 identity-map 的低 4GB。 */
#define HEAP_PAGES        256u     /* 1MB 堆 */
#define HEAP_VIRT_START   0xFFFF800000000000ULL
#define HEAP_VIRT_END     (HEAP_VIRT_START + (u64)HEAP_PAGES * PAGE_SIZE)

/* magic 校验值 */
#define HEAP_MAGIC_USED   0xDEADBEEFu   /* 已分配 */
#define HEAP_MAGIC_FREE   0xCAFEBABEu   /* 已释放（在 free list 里） */

/* 最小 block 大小（含 header），不足则不拆分 */
#define HEAP_MIN_BLOCK    (sizeof(struct block_header) + 16)

/* ---------------------------------------------------------------
 * block_header — 堆 block 头部（精确 16 字节）
 *
 * 每个 block（无论 free 还是 used）开头都有这个 header。
 * 设计目标：让 header 严格 16 字节，使 data 区起始地址
 * 自动 16 字节对齐（满足 System V AMD64 ABI 对最大对齐的要求）。
 *
 * 字段选择：
 *   magic (u32)  — 4 字节校验值，检测越界 / double free
 *   size  (u32)  — 4 字节 block 总大小（含 header）
 *                  上限 4GB，对 1MB 堆绰绰有余
 *   next  (u64)  — 8 字节，指向下一个 free block
 *                  （仅 free 时有效；used 时被设为 NULL）
 *
 * 总计 4 + 4 + 8 = 16 字节，正好。 */
struct block_header {
    u32   magic;     /* HEAP_MAGIC_USED / HEAP_MAGIC_FREE */
    u32   size;      /* block 总大小（含 header），单位：字节 */
    struct block_header *next;  /* 下一个 free block（仅 free 时有效） */
} __attribute__((packed));

/* data 区起始地址 = header 末尾（已 16 字节对齐） */
static inline void *header_to_data(struct block_header *h) {
    return (void *)((u8 *)h + sizeof(struct block_header));
}

/* data 指针 → header */
static inline struct block_header *data_to_header(void *ptr) {
    return (struct block_header *)((u8 *)ptr - sizeof(struct block_header));
}

/* ---------------------------------------------------------------
 * 状态变量
 *   free_list  — free 链表头（NULL 表示空）
 *   total_bytes — 堆总字节（HEAP_PAGES × 4KB）
 *   used_bytes  — 已分配字节（含 header + 内部碎片）
 *   free_bytes  — 剩余字节（含 free block 的 header）
 *   alloc_count — 已分配 block 数（调试用） */
static struct block_header *free_list = NULL;
static usize_t total_bytes = 0;
static usize_t used_bytes  = 0;
static usize_t free_bytes  = 0;
static usize_t alloc_count = 0;

/* 是否已初始化（防止 kmalloc 在 init 前调用） */
static int mm_initialized = 0;

/* ---------------------------------------------------------------
 * kernel_mm_init — 初始化内核堆
 *
 * 流程：
 *   1. 检查 VMM 是否已 init（current_pml4_phys 应已就绪）
 *      ↓ 实际上 VMM 是 arch_mem_init() 调用的，main 里顺序保证
 *   2. 循环 HEAP_PAGES 次：
 *      a. arch_pmm_alloc_frame() 拿一个 4KB 物理页
 *      b. arch_vmm_map_page() 映射到对应虚拟地址
 *   3. 把整块堆当成一个大 free block，挂到 free_list
 *   4. 设 mm_initialized = 1
 *
 * 【失败处理】
 *   - PMM OOM → panic（堆是内核基础设施，没堆就死）
 *   - VMM 失败 → panic（同上） */
void kernel_mm_init(void) {
    /* 第 1 步：分配 + 映射 HEAP_PAGES 个页 */
    for (usize_t i = 0; i < HEAP_PAGES; i++) {
        paddr_t phys = arch_pmm_alloc_frame();
        if (phys == 0) {
            panic(__FILE__, __LINE__,
                  "kernel_mm_init: PMM OOM while creating heap");
        }
        vaddr_t virt = HEAP_VIRT_START + i * PAGE_SIZE;
        if (arch_vmm_map_page(virt, phys, PAGE_FLAGS_KERNEL_RW) != 0) {
            panic(__FILE__, __LINE__,
                  "kernel_mm_init: VMM map failed while creating heap");
        }
    }

    /* 第 2 步：把整块设为一个大 free block */
    struct block_header *first = (struct block_header *)HEAP_VIRT_START;
    first->magic = HEAP_MAGIC_FREE;
    first->size = (u32)((usize_t)HEAP_PAGES * PAGE_SIZE);
    first->next = NULL;

    free_list = first;
    total_bytes = (usize_t)HEAP_PAGES * PAGE_SIZE;
    used_bytes = 0;
    free_bytes = total_bytes;
    alloc_count = 0;

    mm_initialized = 1;
}

/* ---------------------------------------------------------------
 * kmalloc — 分配 size 字节
 *
 * 流程：
 *   1. 校验 mm_initialized + size 合法性
 *   2. 计算"需要 block 多大"（含 header，向上对齐 16）
 *   3. 遍历 free 链表找 first-fit
 *   4. 找到 block：
 *      - 如果剩余空间能放下 HEAP_MIN_BLOCK → 拆分
 *      - 否则整块切出
 *   5. 把 block 从 free 链表移除
 *   6. 标记 magic = USED
 *   7. 返回 data 区指针
 *
 * 【对齐 16 字节】
 *   - header 是 16 字节
 *   - data 起始地址 = header 末尾，自动 16 对齐
 *   - 后续 block 也是 16 对齐
 *
 *   如果 block size 不是 16 的倍数，最后一个 block 的 data 区可能
 *   有 1~15 字节浪费（padding），但保证下一个 block 16 对齐。 */
void *kmalloc(usize_t size) {
    if (!mm_initialized) {
        panic(__FILE__, __LINE__, "kmalloc called before kernel_mm_init");
    }
    if (size == 0) {
        return NULL;
    }

    /* 需要 block 总大小 = header + data，向上对齐 16 */
    usize_t need = sizeof(struct block_header) + size;
    need = (need + 15) & ~((usize_t)15);

    /* first-fit 遍历
     *
     * 链表节点结构：prev → cur → cur->next
     * 找到合适的 cur 后，要把它从链表里"摘出来"。 */
    struct block_header *prev = NULL;
    struct block_header *cur  = free_list;

    while (cur != NULL) {
        if (cur->size >= need) {
            usize_t remaining = cur->size - need;
            /* 保存原 next，用于"摘出"时跳过 cur */
            struct block_header *old_next = cur->next;

            if (remaining >= HEAP_MIN_BLOCK) {
                /* 拆分：在 cur 后面紧贴着创建一个新 free block，
                 * 占用剩下的空间。新 block 替换 cur 在链表中的位置。 */
                struct block_header *new_block =
                    (struct block_header *)((u8 *)cur + need);
                new_block->magic = HEAP_MAGIC_FREE;
                new_block->size  = (u32)remaining;
                new_block->next  = old_next;

                cur->size = (u32)need;
                /* cur->next 不再有效（cur 即将被摘出，转为 USED） */

                /* 让 prev（或链表头）跳过 cur，指向 new_block */
                if (prev != NULL) {
                    prev->next = new_block;
                } else {
                    free_list = new_block;
                }
            } else {
                /* 不拆分：整块切出，剩余的几字节算浪费（padding） */
                if (prev != NULL) {
                    prev->next = old_next;
                } else {
                    free_list = old_next;
                }
            }

            /* 标记 cur 为 USED（已用，不在 free 链表里） */
            cur->magic = HEAP_MAGIC_USED;
            cur->next  = NULL;

            used_bytes += cur->size;
            free_bytes -= cur->size;
            alloc_count++;

            return header_to_data(cur);
        }

        prev = cur;
        cur  = cur->next;
    }

    /* 没找到合适的 block */
    return NULL;
}

/* ---------------------------------------------------------------
 * kfree — 释放 kmalloc 分配的内存
 *
 * 流程：
 *   1. 校验 ptr 合法（NULL 直接返回）
 *   2. 通过 ptr - sizeof(header) 找到 header
 *   3. 检查 magic：
 *      - USED → 正常，继续
 *      - FREE → double free，panic
 *      - 其他 → 不是我们分配的，panic
 *   4. 把 block 加回 free 链表头部
 *   5. 标记 magic = FREE
 *   6. 更新统计
 *   7. 尝试合并相邻 free block（简化版：和链表里相邻 block 合并）
 *
 * 【简化合并】
 *   完整实现应该检查物理上相邻的 block（前后邻居）来合并，
 *   但需要双向链表或者从链表查找邻居，复杂。
 *   简化：把刚释放的 block 加到链表头，不主动合并。
 *   碎片化在长期运行时会变严重，但教学内核够用。
 *
 *   改进方向（后续可做）：
 *     - 在 header 里加 prev 指针（双向链表）
 *     - 释放时遍历链表，按地址排序插入
 *     - 检查相邻物理块并合并 */
void kfree(void *ptr) {
    if (ptr == NULL) {
        return;
    }
    if (!mm_initialized) {
        panic(__FILE__, __LINE__, "kfree called before kernel_mm_init");
    }

    struct block_header *h = data_to_header(ptr);

    /* 检查 magic */
    if (h->magic == HEAP_MAGIC_FREE) {
        panic(__FILE__, __LINE__, "kfree: double free detected");
    }
    if (h->magic != HEAP_MAGIC_USED) {
        panic(__FILE__, __LINE__,
              "kfree: invalid magic (not allocated by kmalloc?)");
    }

    /* 标记为 free */
    h->magic = HEAP_MAGIC_FREE;
    h->next  = free_list;
    free_list = h;

    used_bytes -= h->size;
    free_bytes += h->size;
    alloc_count--;
}

/* ---------------------------------------------------------------
 * kcalloc — 分配 nmemb × size 字节并清零
 *
 * 检查乘法溢出：nmemb × size 不能超过 usize_t 范围。
 * 简单方法：如果 size != 0 且 result / size != nmemb，则溢出。 */
void *kcalloc(usize_t nmemb, usize_t size) {
    if (nmemb == 0 || size == 0) {
        return NULL;
    }
    usize_t total = nmemb * size;
    if (total / size != nmemb) {
        return NULL;     /* 溢出 */
    }

    void *p = kmalloc(total);
    if (p == NULL) {
        return NULL;
    }

    /* memset(0) — 手写，避免依赖 string.h */
    u8 *b = (u8 *)p;
    for (usize_t i = 0; i < total; i++) {
        b[i] = 0;
    }
    return p;
}

/* ---------------------------------------------------------------
 * krealloc — 调整已分配块大小
 *
 * 三种情况：
 *   1. ptr == NULL → kmalloc(size)
 *   2. size == 0   → kfree(ptr)，返回 NULL
 *   3. 正常情况 → 分配新块 + memcpy + 释放旧块 */
void *krealloc(void *ptr, usize_t size) {
    if (ptr == NULL) {
        return kmalloc(size);
    }
    if (size == 0) {
        kfree(ptr);
        return NULL;
    }

    struct block_header *h = data_to_header(ptr);
    if (h->magic != HEAP_MAGIC_USED) {
        panic(__FILE__, __LINE__, "krealloc: invalid magic on input ptr");
    }

    /* 旧 block 的 data 大小 */
    usize_t old_data = h->size - sizeof(struct block_header);

    /* 新块 */
    void *new_ptr = kmalloc(size);
    if (new_ptr == NULL) {
        return NULL;     /* 分配失败，原块不变 */
    }

    /* 拷贝数据（取较小值，避免越界） */
    usize_t to_copy = (old_data < size) ? old_data : size;
    u8 *src = (u8 *)ptr;
    u8 *dst = (u8 *)new_ptr;
    for (usize_t i = 0; i < to_copy; i++) {
        dst[i] = src[i];
    }

    /* 释放旧块 */
    kfree(ptr);
    return new_ptr;
}

/* ---------------------------------------------------------------
 * 统计函数 */
usize_t kernel_mm_total(void)  { return total_bytes; }
usize_t kernel_mm_used(void)   { return used_bytes; }
usize_t kernel_mm_free(void)   { return free_bytes; }
usize_t kernel_mm_blocks(void) { return alloc_count; }
