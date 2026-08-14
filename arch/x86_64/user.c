/* ================================================================
 * arch/x86_64/user.c — 用户态管理实现（TSS + 用户页映射）
 *
 * 【Lesson 8 核心新增】
 *
 * 实现 <arch/user.h> 的接口（除了 arch_enter_user_mode / arch_tss_load
 * 在 user.asm 里）。
 *
 * 三大块：
 *   1. TSS 初始化（arch_tss_init）— 分配 TSS 结构、填 GDT 描述符、ltr
 *   2. TSS.sp0 更新（arch_tss_set_sp0）— 每次切任务前调
 *   3. 用户页映射（arch_user_map_code / map_stack / unmap_pages）
 *
 * 【GDT / TSS 在内存里的关系】
 *
 *   entry.asm 的 .rodata 里：
 *     gdt64:
 *       [0] null          (8B)
 *       [1] kernel code   (8B)  selector 0x08
 *       [2] kernel data   (8B)  selector 0x10
 *       [3] 32-bit code   (8B)  selector 0x18
 *       [4] user code     (8B)  selector 0x20  ← L8 新增
 *       [5] user data     (8B)  selector 0x28  ← L8 新增
 *       [6] TSS           (16B) selector 0x30  ← L8 新增（系统段，16 字节）
 *     gdt64.pointer: { limit, base }
 *
 *   entry.asm 的 .bss 里：
 *     tss:  resb 104   ← TSS 结构体，本文件运行时填充
 *
 *   本文件 arch_tss_init 做：
 *     1. 清零 tss
 *     2. 设 tss.sp0 = 临时值（后续 sched_yield 会更新）
 *     3. 设 tss.iobase = 104（无 IO 位图）
 *     4. 计算 TSS 描述符的 16 字节，写到 gdt64[6]（即 gdt64.tss 位置）
 *     5. ltr 0x30
 * ================================================================ */

#include <arch/cpu.h>
#include <arch/mem.h>
#include <arch/user.h>
#include <kernel/panic.h>
#include <kernel/types.h>

/* ---------------------------------------------------------------
 * TSS 结构（x86-64，104 字节）
 *
 *   只用 rsp0（ring 0 栈指针）。其他全 0。
 *   不设 iobase（TSS limit = 103，CPU 不读 IO 位图字段；
 *   ring 3 IO 访问按 CPL/IOPL 判定，user 不做 IO 所以无所谓）。
 *
 *   布局（Intel SDM Vol 3, 7.7，64-bit TSS）：
 *     0x00: reserved(4)
 *     0x04: RSP0(8)   ← 我们用这个
 *     0x0C: RSP1(8)
 *     0x14: RSP2(8)
 *     0x1C: reserved(4)
 *     0x20: IST1~IST7(56)
 *     0x58: reserved(8)
 *     0x60: reserved(8)   ← 总 104 字节（0x68）
 * --------------------------------------------------------------- */
struct tss64 {
    u32 reserved0;     /*  0: 4 */
    u64 rsp0;          /*  4: 8 — ring 0 栈指针（特权切换时用） */
    u64 rsp1;          /* 12: 8 — ring 1（不用） */
    u64 rsp2;          /* 20: 8 — ring 2（不用） */
    u32 reserved1;     /* 28: 4 */
    u64 ist[7];         /* 32: 56 — IST1~IST7（中断栈表，不用） */
    u64 reserved2;     /* 88: 8 */
    u64 reserved3;     /* 96: 8 */
} __attribute__((packed));

/* 编译期断言：TSS 必须是 104 字节（TSS 描述符 limit = 103） */
typedef char tss_size_check[sizeof(struct tss64) == 104 ? 1 : -1];

/* ---------------------------------------------------------------
 * GDT 描述符（16 字节系统段，用于 TSS）
 *
 *   TSS 在 64 位模式下是"系统段描述符"（16 字节，跨两个 GDT 项）：
 *
 *     byte 0-1: limit[0:15]        = sizeof(tss)-1 = 103
 *     byte 2-4: base[0:23]        = &tss 的低 24 位
 *     byte 5:   access byte        = 0x89
 *               bit7 P=1 (present)
 *               bit6-5 DPL=00 (ring 0)
 *               bit4 S=0 (system segment, 不是 code/data)
 *               bit3-0 type=1001 (available 64-bit TSS)
 *     byte 6:   flags + limit[16:19] = 0x0
 *               bit7 G=0 (byte 粒度，limit 是字节)
 *               bit6-5 0
 *               bit4 0
 *               bit3-0 limit[16:19] = 0（103 < 65536，高 4 位 0）
 *     byte 7:   base[24:31]
 *     byte 8-15: base[32:63]（高 8 字节）
 *
 *   注意：代码段 / 数据段描述符是 8 字节，TSS 是 16 字节。
 *   所以 GDT[6] 占 16 字节，正好等于"两个普通项"。
 * --------------------------------------------------------------- */
struct tss_descriptor {
    u16 limit_low;
    u16 base_low;
    u8  base_mid;       /* base[16:23] */
    u8  access;         /* 0x89 */
    u8  flags_limit;    /* G|0|0|0 | limit[16:19] */
    u8  base_high;      /* base[24:31] */
    u32 base_high32;    /* base[32:63] */
    u32 reserved;       /* 0 */
} __attribute__((packed));

/* ---------------------------------------------------------------
 * 外部符号：entry.asm 的 GDT 表和 TSS 结构
 * --------------------------------------------------------------- */
extern u8 gdt64[];          /* GDT 起始地址（entry.asm .rodata） */
extern u8 tss[];            /* TSS 结构（entry.asm .bss，104 字节） */

/* TSS 描述符在 GDT 里的偏移：6 个 8 字节项之后 = 48 字节
 *   null(8) + kcode(8) + kdata(8) + kcode32(8) + ucode(8) + udata(8) = 48 */
#define GDT_TSS_OFFSET  (GDT_INDEX_TSS * 8)

/* arch_tss_load 在 user.asm */
extern void arch_tss_load(u16 selector);

/* ---------------------------------------------------------------
 * arch_tss_init — 初始化 TSS + 加载 TR
 *
 *   流程见文件头。
 *   必须在 arch_idt_init 之后（IDT 要先加载），
 *   在第一次 sched_yield / 进 ring 3 之前。
 * --------------------------------------------------------------- */
void arch_tss_init(void) {
    /* 1. 清零 TSS */
    u8 *tp = (u8 *)tss;
    for (int i = 0; i < 104; i++) {
        tp[i] = 0;
    }

    /* 2. 设 sp0 = 一个临时内核栈顶（后续 sched_yield 会更新成正确的）
     *   这里用一个栈顶符号——用 tss 结构之后的某个地址也行，
     *   但用真实的栈顶更安全。
     *   实际值：sched_yield 在切到第一个任务前会调 arch_tss_set_sp0 覆盖。
     *   所以这里只是占位，避免 sp0=0 导致 ring 3→ring 0 时 CPU 把栈指到 0。
     *   用 tss 自己的地址（高地址，作为临时栈顶）避免 0。 */
    struct tss64 *t = (struct tss64 *)tss;
    t->rsp0 = (u64)&tp[104];   /* 临时值，sched_yield 会覆盖 */

    /* 3. 不设 iobase（struct 无此字段；limit=103，CPU 不读 IO 位图） */

    /* 4. 构造 TSS 描述符并写到 GDT[6] */
    u64 base = (u64)tss;
    struct tss_descriptor desc;
    desc.limit_low  = (u16)103;           /* sizeof(tss)-1 */
    desc.base_low   = (u16)(base & 0xFFFF);
    desc.base_mid   = (u8)((base >> 16) & 0xFF);
    desc.access     = 0x89;                /* P=1, DPL=0, S=0, type=available 64-bit TSS */
    desc.flags_limit = 0x0;                 /* G=0, limit high 4 bits = 0 */
    desc.base_high  = (u8)((base >> 24) & 0xFF);
    desc.base_high32 = (u32)((base >> 32) & 0xFFFFFFFF);
    desc.reserved   = 0;

    /* 写到 GDT 的 TSS 位置（16 字节）
     *
     *   不能用 `u64 *src = (u64 *)&desc;` —— desc 是 __attribute__((packed))
     *   结构（对齐 1），强转 u64 *（对齐 8）会触发
     *   -Waddress-of-packed-member 警告，且在某些平台真的可能产生未对齐访
     *   问。用 __builtin_memcpy 按字节复制最安全：
     *     - 不要求 src 对齐
     *     - GCC 内置，无 libc 依赖
     *     - 编译器可优化为 2 条 8 字节 mov（对齐时）或更优指令
     *   dst 侧（gdt64 + GDT_TSS_OFFSET）保证 8 字节对齐（GDT 项天然对齐），
     *   所以写 dst[0]/dst[1] 是对齐访问，安全。 */
    u8 *gdte = gdt64 + GDT_TSS_OFFSET;
    __builtin_memcpy(gdte, &desc, sizeof(desc));

    /* 5. ltr 0x30 加载 TR */
    arch_tss_load(TSS_SEG);
}

/* ---------------------------------------------------------------
 * arch_tss_set_sp0 — 更新 TSS.sp0
 *
 *   只写一个字段（rsp0）。tss 是 .bss 全局，identity mapped，可写。
 *   写 8 字节对齐的 rsp0 是原子的（x86 64 位对齐写原子）。
 * --------------------------------------------------------------- */
void arch_tss_set_sp0(u64 sp0) {
    struct tss64 *t = (struct tss64 *)tss;
    t->rsp0 = sp0;
}

/* ================================================================
 * 用户页映射
 * ================================================================ */

/* ---------------------------------------------------------------
 * arch_user_map_code — 映射 user 代码页并装入 image
 *
 *   见 arch/user.h 的接口注释。
 *
 *   【关键】flags = PAGE_FLAG_PRESENT | PAGE_FLAG_USER
 *     - P 存在
 *     - U 用户可访问
 *     - 不带 W → user 只读（不能改自己代码）
 *     - 不带 NX → 可执行（NX 未启用，所有页都可执行）
 *
 *   【vmm.c 配合修改】arch_vmm_map_page 看到 flags 含 USER 时，
 *     会把中间页表项（PML4/PDPT/PD）也设 U 位，确保 ring 3 能走页表到这一页。
 *
 *   【首次拷贝 image】
 *     映射完后，VMA 指向物理页（P|U，无 W）。
 *     ring 0（内核）不受 U/W 限制，可以直接 memcpy 写入。
 *     写完后 user 读这些页时能看到 image 字节（代码）。
 * --------------------------------------------------------------- */
s64 arch_user_map_code(u64 vma, const void *image, u64 image_len) {
    if (vma == 0 || image == NULL || image_len == 0) {
        return -1;
    }
    /* 必须页对齐 */
    if ((vma & (PAGE_SIZE - 1)) != 0) {
        return -1;
    }

    /* 计算页数（向上取整） */
    u64 npages = (image_len + PAGE_SIZE - 1) / PAGE_SIZE;

    u64 flags = PAGE_FLAG_PRESENT | PAGE_FLAG_USER;

    for (u64 i = 0; i < npages; i++) {
        paddr_t phys = arch_pmm_alloc_frame();
        if (phys == 0) {
            /* OOM：回滚已映射的页 */
            arch_user_unmap_pages(vma, i);
            return -1;
        }
        vaddr_t va = vma + i * PAGE_SIZE;
        if (arch_vmm_map_page(va, phys, flags) != 0) {
            arch_pmm_free_frame(phys);
            arch_user_unmap_pages(vma, i);
            return -1;
        }
    }

    /* 拷贝 image 到 VMA（内核 ring 0 可写只读页） */
    u8 *dst = (u8 *)vma;
    const u8 *src = (const u8 *)image;
    for (u64 i = 0; i < image_len; i++) {
        dst[i] = src[i];
    }

    return (s64)npages;
}

/* ---------------------------------------------------------------
 * arch_user_map_stack — 映射 user 栈页
 *
 *   栈从 stack_top 往下生长。映射 [stack_top - npages*4K, stack_top)。
 *   flags = P | W | U（可读写）。
 * --------------------------------------------------------------- */
s64 arch_user_map_stack(u64 stack_top) {
    if (stack_top == 0 || (stack_top & (PAGE_SIZE - 1)) != 0) {
        return -1;
    }

    u64 flags = PAGE_FLAG_PRESENT | PAGE_FLAG_WRITE | PAGE_FLAG_USER;

    for (u64 i = 0; i < USER_STACK_PAGES; i++) {
        paddr_t phys = arch_pmm_alloc_frame();
        if (phys == 0) {
            /* 回滚 */
            vaddr_t mapped_base = stack_top - i * PAGE_SIZE;
            arch_user_unmap_pages(mapped_base, i);
            return -1;
        }
        vaddr_t va = stack_top - (i + 1) * PAGE_SIZE;
        if (arch_vmm_map_page(va, phys, flags) != 0) {
            arch_pmm_free_frame(phys);
            vaddr_t mapped_base = stack_top - i * PAGE_SIZE;
            arch_user_unmap_pages(mapped_base, i);
            return -1;
        }
        /* 栈页清零（user 栈初始内容应为 0，避免读到垃圾数据） */
        u8 *p = (u8 *)va;
        for (u64 j = 0; j < PAGE_SIZE; j++) {
            p[j] = 0;
        }
    }

    return 0;
}

/* ---------------------------------------------------------------
 * arch_user_unmap_pages — 取消映射并释放 user 页（含深度中间页表清理）
 *
 *   逐页调用 arch_vmm_unmap_user_page：
 *     - 清 PT 项 + 释放数据页物理帧
 *     - 若 PT 全空 → 释放 PT 帧 + 清 PD 项
 *     - 若 PD 全空 → 释放 PD 帧 + 清 PDPT 项
 *     - 若 PDPT 全空 且 pml4_idx ∈ [1,510] → 释放 PDPT 帧 + 清 PML4 项
 *
 *   深度清理保证 user 任务退出后 PMM 帧数完全回收，无中间页表泄漏。
 *   （旧版只清 PT 项 + 释放数据页，每任务泄漏 PT/PD/PDPT 共 ~3 帧。）
 * --------------------------------------------------------------- */
void arch_user_unmap_pages(u64 vma, u64 npages) {
    for (u64 i = 0; i < npages; i++) {
        vaddr_t va = vma + i * PAGE_SIZE;
        arch_vmm_unmap_user_page(va);
    }
}
