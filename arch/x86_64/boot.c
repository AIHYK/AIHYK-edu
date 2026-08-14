/* ================================================================
 * arch/x86_64/boot.c — 启动信息解析（64 位长模式，兼容 multiboot2 和 PVH）
 *
 * 实现 arch/boot.h 定义的接口。
 *
 * 【Lesson 2 启动方式支持】
 *   本文件同时支持两种启动方式，由 entry.asm 保存的 EAX 值决定：
 *
 *   A. multiboot2（GRUB + ISO 启动，三平台都用：QEMU/VMware/实体机）
 *      - EAX = 0x36d76289 (MULTIBOOT2_BOOTLOADER_MAGIC)
 *      - EBX = multiboot2 info 结构物理地址
 *      - info 结构是 tag-based 链表（遍历查找 cmdline / mmap 等）
 *
 *   B. PVH（QEMU -kernel 直接加载，通过 .note.pvh ELF Note）
 *      - EAX = 0x336ec578 (PVH magic)
 *      - EBX = hvm_start_info 结构物理地址
 *      - info 结构是固定布局（直接读字段）
 *
 *   entry.asm 在 32 位阶段把 EAX/EBX 保存到 x86_64_boot_eax/ebx。
 *   切换到长模式后，本文件（64 位 C 代码）读取这两个变量，
 *   根据 EAX 判断启动方式，调用对应的解析函数。
 *
 * 【Lesson 4 新增】
 *   - 解析 multiboot2 mmap tag（type=6），把 BIOS e820 风格的
 *     内存区域表填到 boot_info.regions
 *   - 解析 PVH hvm_start_info v1.1+ 的 memmap_paddr / memmap_entries
 *     （Xen 规范定义，QEMU PVH 提供此字段）
 *   - 把两种来源都转换成统一的 struct mem_region 数组
 *   - 自动标记内核镜像区域为 MEM_KERNEL
 *
 * multiboot2 info 结构：
 *   [total_size: u32][reserved: u32]
 *   [tag1: type(u32) + size(u32) + data...]
 *   [tag2: type(u32) + size(u32) + data...]
 *   ...
 *   [end tag: type=0, size=8]
 *
 *   每个 tag 8 字节对齐（size 向上取整到 8 的倍数）
 *   我们遍历 tag 链表，找需要的 tag（cmdline、mmap 等）
 *
 * PVH hvm_start_info 结构（Xen PVH 启动协议 v1）：
 *   [magic: u32 = 0x336ec578]
 *   [version: u32 = 1]
 *   [flags: u32]
 *   [nr_modules: u32]
 *   [modlist_paddr: u64]
 *   [cmdline_paddr: u64]   ← Lesson 2 解析
 *   [rsdp_paddr: u64]
 *   [memmap_paddr: u64]    ← Lesson 4 解析（v1.1+ 才有）
 *   [memmap_entries: u32]  ← Lesson 4 解析
 *   [reserved: u32]
 *
 *   QEMU 的 PVH 实现版本是 1.1+，会提供 memmap_paddr。
 *   旧版 Xen 只到 v1.0，没有 memmap 字段。
 *   我们做版本检查，没有 memmap 时 panic 给出明确错误。
 * ================================================================ */

#include <arch/boot.h>
#include <arch/console.h>
#include <kernel/types.h>
#include <kernel/panic.h>

/* ---------------------------------------------------------------
 * 启动 magic 常量
 *
 * bootloader 加载内核成功后，EAX 必须等于对应的 magic：
 *   multiboot2: 0x36d76289（GRUB 加载 multiboot2 内核）
 *   PVH:        0x336ec578（QEMU 通过 PVH 协议加载）
 *
 * 如果 EAX 不是这两个值之一，说明不是合法 bootloader 加载的，
 * EBX 指向的 info 地址无效，继续解析会 triple fault。
 * --------------------------------------------------------------- */
#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36d76289
#define PVH_MAGIC                  0x336ec578

/* 命令行最大长度（防止越界读取） */
#define CMDLINE_MAX_LEN 256

/* ---------------------------------------------------------------
 * x86_64_boot_eax — 保存 bootloader 传来的启动 magic
 * x86_64_boot_ebx — 保存 bootloader 传来的 info 结构物理地址
 *
 * entry.asm 在 32 位阶段执行：
 *   mov [x86_64_boot_eax], eax
 *   mov [x86_64_boot_ebx], ebx
 *
 * 变量定义为 u64：
 *   - 32 位 mov 只写低 4 字节
 *   - .bss 默认全 0，所以高 4 字节是 0
 *   - 64 位代码读取时直接得到 64 位值（32 位零扩展）
 * --------------------------------------------------------------- */
u64 x86_64_boot_eax = 0;
u64 x86_64_boot_ebx = 0;

/* ---------------------------------------------------------------
 * 静态缓冲区：保存解析出的内存区域
 *
 * PMM/VMM 后续会用这些 region 来初始化位图。
 * 静态分配（不放 .bss 之外的堆），避免"先有鸡还是先有蛋"问题
 * （PMM 还没初始化，没法 malloc）。
 *
 * 【为什么 64 项】
 *   - BIOS e820 通常 6~10 项
 *   - EFI memory map 通常 30~60 项
 *   - 留 64 项足够覆盖各种情况
 *   - 不够的话截断 + 警告（不 panic，启动能继续） */
#define MAX_REGIONS 64
static struct mem_region regions_buf[MAX_REGIONS];
static int regions_count = 0;

/* 内核镜像的物理地址范围（由 entry.asm 的 . = 1M 决定）
 * 我们在 arch_boot_init 里用链接符号 __kernel_start/__kernel_end
 * 精确获取，不写死。 */
extern u8 __kernel_start[];
extern u8 __kernel_end[];

/* ---------------------------------------------------------------
 * 启动方式枚举（内部使用，判断 EAX 是哪个 magic）
 * --------------------------------------------------------------- */
enum boot_method {
    BOOT_UNKNOWN,        /* 未知启动方式（panic）*/
    BOOT_MULTIBOOT2,     /* GRUB + multiboot2 */
    BOOT_PVH,            /* QEMU -kernel + PVH note */
};

/* ---------------------------------------------------------------
 * multiboot2 info 头部
 *
 * GRUB 把 multiboot2 info 结构放在 EBX 指向的物理地址。
 * 头部 8 字节：
 *   total_size - 整个 info 结构的总大小（含所有 tag）
 *   reserved   - 保留，必须为 0
 * --------------------------------------------------------------- */
struct multiboot2_info_header {
    u32 total_size;
    u32 reserved;
};

/* ---------------------------------------------------------------
 * multiboot2 tag 通用头
 *
 * 每个 tag 开头都是这个结构：
 *   type - tag 类型（1=cmdline, 4=membasic, 6=mmap, 0=end）
 *   size - tag 总大小（含 type+size，8 字节对齐）
 *
 * tag 数据跟在 size 后面，格式因 type 而异
 * --------------------------------------------------------------- */
struct multiboot2_tag {
    u32 type;
    u32 size;
};

/* tag 类型常量 */
#define MULTIBOOT2_TAG_END              0   /* 结束标记 */
#define MULTIBOOT2_TAG_CMDLINE          1   /* 启动命令行 */
#define MULTIBOOT2_TAG_BOOT_LOADER_NAME 2   /* bootloader 名字 */
#define MULTIBOOT2_TAG_MODULE           3   /* 启动模块 */
#define MULTIBOOT2_TAG_BASIC_MEM_INFO   4   /* 基本内存信息 */
#define MULTIBOOT2_TAG_MMAP             6   /* 内存映射（重点！Lesson 4 用）*/

/* ---------------------------------------------------------------
 * multiboot2 cmdline tag 结构
 *
 * type = 1
 * size = 8 + strlen(cmdline) + 1（含结尾 \0，8 字节对齐）
 * cmdline 是变长字符串
 * --------------------------------------------------------------- */
struct multiboot2_tag_cmdline {
    u32 type;
    u32 size;
    char cmdline[];       /* C99 flexible array member */
};

/* ---------------------------------------------------------------
 * multiboot2 mmap tag 结构（重点：Lesson 4 用）
 *
 * type = 6
 * size = 16（头）+ N × entry_size
 * entry_size — 每条 mmap 项的字节数（通常 24，但规范允许变长）
 * entry_version — 0（保留）
 *
 * 后面跟 N 个 multiboot2_mmap_entry（紧密排列，无 padding）
 *
 * 【为什么 entry_size 不固定】
 *   multiboot2 规范允许实现扩展 entry（增加字段），
 *   我们读 entry_size 决定步长，不写死 24。
 * --------------------------------------------------------------- */
struct multiboot2_tag_mmap {
    u32 type;
    u32 size;
    u32 entry_size;      /* 每条 entry 的字节数（通常 24） */
    u32 entry_version;   /* 0 */
    /* 后面跟 entry_size × N 字节的数据 */
};

/* 单条 multiboot2 mmap entry（标准 v0，24 字节） */
struct multiboot2_mmap_entry {
    u64 addr;     /* 起始物理地址 */
    u64 len;      /* 长度（字节） */
    u32 type;     /* 1=可用, 2=保留, 3=ACPI 可回收, 4=ACPI NVS, 5=坏内存 */
    u32 zero;     /* 保留，0 */
};

/* multiboot2 mmap 类型常量（来自 BIOS e820） */
#define MB2_MMAP_AVAILABLE       1   /* 可用内存 */
#define MB2_MMAP_RESERVED        2   /* 保留（BIOS、硬件） */
#define MB2_MMAP_ACPI_RECLAIMABLE 3  /* ACPI 表，可回收 */
#define MB2_MMAP_NVS             4   /* ACPI NVS（不可用） */
#define MB2_MMAP_BADRAM          5   /* 坏内存 */

/* ---------------------------------------------------------------
 * PVH hvm_start_info 结构（Xen PVH 启动协议 v1.1）
 *
 * QEMU 通过 PVH 协议加载内核后，EBX 指向这个结构。
 * 字段顺序和大小固定，用 packed 防止编译器插入 padding。
 *
 * 字段说明：
 *   magic         = 0x336ec578（PVH magic，校验用）
 *   version       = 1（v1）或 1.1（memmap 可用）
 *
 *   v1.0 字段（version >= 1）：
 *     magic, version, flags, nr_modules, modlist_paddr,
 *     cmdline_paddr, rsdp_paddr
 *
 *   v1.1 新增字段（version >= 0x10001 即 1.1，但我们直接读字段，
 *                  v1.0 时这些字段读到的可能是垃圾值）：
 *     memmap_paddr    — 内存映射表物理地址
 *     memmap_entries  — 内存映射表条目数
 *     reserved        — 0
 *
 * 【QEMU 的 PVH 实现】
 *   QEMU 把 version 设为 1.1（高 16 位 = 1, 低 16 位 = 1）
 *   实测 memmap_paddr 不为 0，memmap_entries 通常为 4~6 项
 *   （包含低 1MB、内核镜像、ACPI、可用 RAM 等区域） */
struct hvm_start_info {
    u32 magic;            /* offset 0:  0x336ec578 */
    u32 version;          /* offset 4:  1 或 0x10001（1.1） */
    u32 flags;           /* offset 8:  保留 */
    u32 nr_modules;      /* offset 12: 启动模块数 */
    u64 modlist_paddr;   /* offset 16: 模块列表地址 */
    u64 cmdline_paddr;   /* offset 24: 命令行地址 */
    u64 rsdp_paddr;      /* offset 32: ACPI RSDP 地址 */
    /* v1.1+ 字段（QEMU 实现） */
    u64 memmap_paddr;    /* offset 40: 内存映射表物理地址 */
    u32 memmap_entries;  /* offset 48: 内存映射表条目数 */
    u32 reserved;        /* offset 52: 0 */
} __attribute__((packed));

/* ---------------------------------------------------------------
 * PVH hvm_memmap_entry 结构（Xen PVH 规范）
 *
 * PVH 用 EFI memory type 来描述区域（不是 BIOS e820 type）：
 *   7  = EfiConventionalMemory  → 可用 RAM
 *   4  = EfiBootServicesData     → 可用 RAM（boot services 退出后）
 *   3  = EfiBootServicesCode     → 可用 RAM（boot services 退出后）
 *   6  = EfiRuntimeServicesData  → 保留（runtime services 用）
 *   5  = EfiRuntimeServicesCode → 保留
 *   0  = EfiReservedMemoryType  → 保留
 *   其他 → 保留
 *
 * 我们把 7（ConventionalMemory）当作 MEM_USABLE，其他当作 MEM_RESERVED。
 *
 * 【为什么 PVH 用 EFI 而不是 e820】
 *   PVH 协议是 Xen 设计的，Xen 用 EFI memory map。
 *   QEMU 的 PVH 把 BIOS e820 转成 EFI memory map 后给内核。
 *
 * struct 字段：
 *   addr — 起始物理地址
 *   size — 长度（字节）
 *   type — EFI memory type（7 = 可用）
 *   reserved — 0
 * --------------------------------------------------------------- */
struct hvm_memmap_entry {
    u64 addr;        /* offset 0: 起始物理地址 */
    u64 size;        /* offset 8: 长度（字节） */
    u32 type;        /* offset 16: EFI memory type */
    u32 reserved;    /* offset 20: 0 */
} __attribute__((packed));

#define EFI_MEMORY_CONVENTIONAL  7   /* 可用 RAM */

/* ---------------------------------------------------------------
 * strnlen — 计算字符串长度，最多查 maxlen 个字符
 * --------------------------------------------------------------- */
static usize_t strnlen(const char *s, usize_t maxlen) {
    usize_t i;
    for (i = 0; i < maxlen; i++) {
        if (s[i] == '\0') {
            return i;
        }
    }
    return maxlen;
}

/* ---------------------------------------------------------------
 * align_up_8 — 向上对齐到 8 的倍数
 *
 * multiboot2 规范要求每个 tag 8 字节对齐
 * 下一个 tag 的偏移 = 当前 tag 的 size 向上取整到 8
 * --------------------------------------------------------------- */
static u32 align_up_8(u32 v) {
    return (v + 7) & ~((u32)7);
}

/* ---------------------------------------------------------------
 * add_region — 把一条 mem_region 追加到全局数组
 *
 * 数组满了就截断（不 panic，启动应能继续）。
 * 后续 PMM 会处理这个数组。
 * --------------------------------------------------------------- */
static void add_region(paddr_t base, usize_t length, enum mem_type type) {
    if (regions_count >= MAX_REGIONS) {
        arch_console_set_color(CON_COLOR_YELLOW);
        arch_console_print("[WARN] mem_region table full (");
        arch_console_print("ignoring region)\n");
        arch_console_set_color(CON_COLOR_DEFAULT);
        return;
    }
    regions_buf[regions_count].base   = base;
    regions_buf[regions_count].length = length;
    regions_buf[regions_count].type   = type;
    regions_count++;
}

/* ---------------------------------------------------------------
 * detect_boot_method — 判断启动方式
 *
 * 两种启动方式的 magic 位置不同：
 *   multiboot2: magic 在 EAX（GRUB 设的）
 *     EAX = 0x36d76289 (MULTIBOOT2_BOOTLOADER_MAGIC)
 *   PVH: magic 在 hvm_start_info 结构里（QEMU 设的）
 *     EAX = 任意值（实测 = 0，PVH 协议不保证）
 *     EBX = hvm_start_info 物理地址
 *     hvm_start_info.magic = 0x336ec578 (PVH_MAGIC)
 *
 * 【为什么 PVH 的 magic 不在 EAX】
 *   PVH 协议设计上，EAX 是"未定义"的（不同实现可能不同）
 *   真正的 magic 在 hvm_start_info 结构的第一个字段
 *   内核通过读 EBX 指向的结构来判断是否 PVH 启动
 *
 * 返回值：
 *   BOOT_MULTIBOOT2 - GRUB + multiboot2 启动
 *   BOOT_PVH        - QEMU -kernel + PVH 启动
 *   BOOT_UNKNOWN    - 未知启动方式（调用方应 panic）
 * --------------------------------------------------------------- */
static enum boot_method detect_boot_method(void) {
    /* 检查 multiboot2 magic（在 EAX）*/
    if (x86_64_boot_eax == MULTIBOOT2_BOOTLOADER_MAGIC) {
        return BOOT_MULTIBOOT2;
    }

    /* 检查 PVH magic（在 hvm_start_info.magic）
     *
     * 【C9 修复】原代码直接 `(struct hvm_start_info *)x86_64_boot_ebx`
     *   然后 `si->magic`，若 EBX 是垃圾值（非 PVH 启动且非 multiboot2），
     *   解引用会 #PF → triple fault，没有任何诊断信息。
     *
     *   PVH 协议保证（Xen PVH spec, hvm_start_info 布局）：
     *     - EBX 指向 hvm_start_info 结构，物理地址
     *     - 结构必须 4 字节对齐（magic 字段是 u32）
     *     - 结构位于低 4GB 物理地址（PVH 32-bit entry point 约束）
     *     - magic 字段在结构偏移 0，读 4 字节
     *
     *   这里做轻量预校验，过滤明显非法的 EBX：
     *     (a) 0：bootloader 没传，或未初始化（.bss 默认 0）
     *     (b) 非 4 字节对齐：违反 PVH 协议，不可能是合法 start_info
     *     (c) 超过 4GB：PVH 32-bit entry 约束
     *   通过预校验后再解引用。完整 page-table-walk 校验需要 VMM
     *   已就绪（此时 VMM 还没初始化），故只做静态范围检查。
     *
     *   注意：multiboot2 路径已经在前面的 EAX 校验中分流，
     *   走到这里说明 EAX ≠ multiboot2 magic，EBX 合法的唯一可能是 PVH。
     *   预校验失败 → BOOT_UNKNOWN → 调用方 panic 给出诊断信息。 */
    u64 ebx = x86_64_boot_ebx;
    if (ebx == 0 || (ebx & 0x3) != 0 || ebx >= (1ULL << 32)) {
        return BOOT_UNKNOWN;
    }

    struct hvm_start_info *si = (struct hvm_start_info *)ebx;
    if (si->magic == PVH_MAGIC) {
        return BOOT_PVH;
    }

    return BOOT_UNKNOWN;
}

/* ---------------------------------------------------------------
 * parse_multiboot2_mmap — 解析 multiboot2 mmap tag
 *
 * 把 multiboot2 的 type=6 tag 转换成 struct mem_region 数组。
 *
 * 流程：
 *   1. tag->entry_size 决定步长（规范允许扩展，不能写死 24）
 *   2. 遍历 entry，按 multiboot2 type 转 mem_type
 *   3. add_region 追加到全局表
 *
 * 【multiboot2 type → mem_type 映射】
 *   1 (AVAILABLE)        → MEM_USABLE
 *   2 (RESERVED)        → MEM_RESERVED
 *   3 (ACPI_RECLAIMABLE) → MEM_ACPI
 *   4 (NVS)              → MEM_RESERVED（不能给 PMM，但是 ACPI 用）
 *   5 (BADRAM)           → MEM_RESERVED
 *   其他 → MEM_RESERVED（保守起见）
 *
 * 【为什么 ACPI 单独标记】
 *   MEM_ACPI 区域的内存"暂时被占用"，但内核读 ACPI 表后可以释放。
 *   把它和 MEM_RESERVED 区分开，方便后续处理。 */
static void parse_multiboot2_mmap(struct multiboot2_tag_mmap *tag) {
    /* tag->size 包含了头（16 字节）+ N × entry_size
     * 实际 entry 数 = (size - 16) / entry_size */
    u32 entry_size = tag->entry_size;
    if (entry_size < 24) {
        /* 异常情况：entry 比标准小，无法读取关键字段 */
        return;
    }

    u32 entries_bytes = tag->size - sizeof(struct multiboot2_tag_mmap);
    u8 *p = (u8 *)tag + sizeof(struct multiboot2_tag_mmap);
    u8 *end = p + entries_bytes;

    while (p + entry_size <= end) {
        struct multiboot2_mmap_entry *e = (struct multiboot2_mmap_entry *)p;

        enum mem_type t;
        switch (e->type) {
            case MB2_MMAP_AVAILABLE:
                t = MEM_USABLE;
                break;
            case MB2_MMAP_ACPI_RECLAIMABLE:
                t = MEM_ACPI;
                break;
            default:
                /* 保留、NVS、坏内存都按"保留"处理（PMM 不分配） */
                t = MEM_RESERVED;
                break;
        }

        add_region(e->addr, e->len, t);
        p += entry_size;
    }
}

/* ---------------------------------------------------------------
 * parse_multiboot2 — 解析 multiboot2 info 结构
 *
 * 流程：
 *   1. 获取 multiboot2 info 头部
 *   2. 遍历 tag 链表
 *   3. 提取 cmdline（命令行参数）
 *   4. 提取 mmap（内存区域表，Lesson 4 新增）
 *
 * 为什么遍历 tag？
 *   multiboot1 是固定布局结构（直接读字段）
 *   multiboot2 是 tag 链表（按 type 查找）
 *   tag 顺序不固定，必须遍历
 * --------------------------------------------------------------- */
static void parse_multiboot2(struct boot_info *info) {
    /* 获取 multiboot2 info 头部
     *
     * x86_64_boot_ebx 是 u64，但有效值只有低 32 位。
     * 强转为指针时直接用（高 32 位是 0）。 */
    struct multiboot2_info_header *hdr;
    hdr = (struct multiboot2_info_header *)x86_64_boot_ebx;

    /* 遍历 tag 链表
     *
     * tag 从 header 后开始（偏移 8 字节）
     * 每个 tag 的 size 包含自己的 type+size（8 字节）
     * 下一个 tag 偏移 = 当前 tag 地址 + align_up_8(size)
     * 遇到 type=0 (END) 停止 */
    u32 total = hdr->total_size;
    u8 *p = (u8 *)hdr + sizeof(struct multiboot2_info_header);
    u8 *end = (u8 *)hdr + total;

    while (p + sizeof(struct multiboot2_tag) <= end) {
        struct multiboot2_tag *tag = (struct multiboot2_tag *)p;

        /* 结束标记 */
        if (tag->type == MULTIBOOT2_TAG_END) {
            break;
        }

        /* 命令行 tag */
        if (tag->type == MULTIBOOT2_TAG_CMDLINE) {
            struct multiboot2_tag_cmdline *cmd;
            cmd = (struct multiboot2_tag_cmdline *)p;
            usize_t len = strnlen(cmd->cmdline, CMDLINE_MAX_LEN);
            if (len < CMDLINE_MAX_LEN) {
                info->cmdline = cmd->cmdline;
            }
            /* 长度超限则保留默认空串 */
        }

        /* 内存映射 tag（Lesson 4 新增） */
        if (tag->type == MULTIBOOT2_TAG_MMAP) {
            parse_multiboot2_mmap((struct multiboot2_tag_mmap *)p);
        }

        /* 移动到下一个 tag（8 字节对齐） */
        u32 next_offset = align_up_8(tag->size);
        p += next_offset;

        /* 防止 size 异常导致越界 */
        if (next_offset == 0) {
            break;
        }
    }
}

/* ---------------------------------------------------------------
 * parse_pvh_memmap — 解析 PVH 内存映射表
 *
 * PVH v1.1+ 提供 memmap_paddr / memmap_entries 字段，
 * 指向一个 hvm_memmap_entry 数组。
 *
 * 流程：
 *   1. 检查 version（< 1.1 不解析，调用方决定如何处理）
 *   2. 读 memmap_paddr / memmap_entries
 *   3. 遍历 entry，按 EFI type 转 mem_type
 *   4. add_region 追加到全局表
 *
 * 【为什么 PVH memmap 用 EFI 类型而不是 e820】
 *   Xen PVH 协议原生用 EFI memory map。
 *   QEMU 把 BIOS e820 转成 EFI memory map 给 PVH 内核。
 *
 *   关键映射：
 *     EFI 7 (EfiConventionalMemory) → MEM_USABLE
 *     其他 → MEM_RESERVED
 *
 * 【为什么不处理 EfiBootServicesData / EfiBootServicesCode】
 *   这些在 EFI 启动时被占用，启动后可以释放。
 *   但 PVH 协议下，QEMU 把这些区域标记为 ConventionalMemory 或
 *   其他类型，不会标 BootServices。我们简单处理：只信 Conventional。 */
static void parse_pvh_memmap(struct hvm_start_info *si) {
    /* QEMU PVH version = 0x10001（高 16 = 1, 低 16 = 1）即 1.1
     * 也可能是 1（v1.0），此时 memmap_paddr 字段未定义。
     *
     * 简化处理：检查 memmap_paddr != 0 && memmap_entries > 0，
     * 如果有就解析，没有就 panic（PMM 没法初始化）。 */
    if (si->memmap_paddr == 0 || si->memmap_entries == 0) {
        /* v1.0 旧 PVH 没有 memmap，或 QEMU 实现不完整 */
        panic(__FILE__, __LINE__,
              "PVH memmap not provided (start_info too old or QEMU bug)");
    }

    /* memmap 数组在物理地址 memmap_paddr（identity map 下直接读）*/
    struct hvm_memmap_entry *entries =
        (struct hvm_memmap_entry *)si->memmap_paddr;

    /* 限制最多处理 MAX_REGIONS 条（防止 memmap_entries 异常巨大） */
    u32 count = si->memmap_entries;
    if (count > MAX_REGIONS) {
        count = MAX_REGIONS;
    }

    for (u32 i = 0; i < count; i++) {
        enum mem_type t;
        /* QEMU 的 PVH 实测把 e820 type 直接放到 hvm_memmap_entry.type 字段
         * （而不是 Xen 规范定义的 EFI memory type）。
         *
         * 实测值：
         *   1 = e820 RAM（可用，对应 EfiLoaderCode）
         *   2 = e820 Reserved（保留，对应 EfiLoaderData）
         *   3 = e820 ACPI（可回收）
         *   4 = e820 NVS（保留）
         *   5 = e820 Unusable（保留）
         *
         * 为了同时兼容：
         *   - QEMU PVH（e820 类型）
         *   - 真 Xen PVH（EFI 类型）
         * 我们把 type=1 (e820 RAM) 和 type=7 (EFI ConventionalMemory)
         * 都当作 MEM_USABLE，其他都当 MEM_RESERVED。
         * ACPI 类型（e820=3 或 EFI=9）标 MEM_ACPI。 */
        if (entries[i].type == 1 /* E820_RAM */
            || entries[i].type == EFI_MEMORY_CONVENTIONAL /* = 7 */) {
            t = MEM_USABLE;
        } else if (entries[i].type == 3 /* E820_ACPI */
                   || entries[i].type == 9 /* EfiACPIReclaimMemory */) {
            t = MEM_ACPI;
        } else {
            t = MEM_RESERVED;
        }
        add_region(entries[i].addr, entries[i].size, t);
    }
}

/* ---------------------------------------------------------------
 * parse_pvh — 解析 PVH hvm_start_info 结构
 *
 * 流程：
 *   1. 校验 magic（不是 PVH 加载就 panic）
 *   2. 读 cmdline_paddr，转为字符串指针
 *   3. 限制 cmdline 长度，防止越界
 *   4. 解析 memmap_paddr（Lesson 4 新增）
 *
 * 【和 multiboot2 的区别】
 *   - multiboot2 是 tag 链表，要遍历
 *   - PVH 是固定布局结构，直接读字段
 *   - PVH 的 cmdline_paddr 是物理地址（identity map 下 == 虚拟地址）
 *
 * 【为什么 cmdline_paddr 可能是 0】
 *   如果启动时没有传 -append 参数，QEMU 不会设 cmdline，
 *   cmdline_paddr 保持 0，表示"无命令行"
 * --------------------------------------------------------------- */
static void parse_pvh(struct boot_info *info) {
    struct hvm_start_info *si;
    si = (struct hvm_start_info *)x86_64_boot_ebx;

    /* 校验 PVH magic（防止 EBX 是垃圾值） */
    if (si->magic != PVH_MAGIC) {
        panic(__FILE__, __LINE__,
              "PVH start_info magic mismatch (not a valid PVH boot)");
    }

    /* 解析 cmdline */
    if (si->cmdline_paddr != 0) {
        /* cmdline_paddr 是物理地址，identity map 下直接当指针用
         * 低 32 位是有效地址（cmdline 通常在低 32 位地址空间） */
        const char *cmdline = (const char *)(u64)si->cmdline_paddr;
        usize_t len = strnlen(cmdline, CMDLINE_MAX_LEN);
        if (len < CMDLINE_MAX_LEN) {
            info->cmdline = cmdline;
        }
        /* 长度超限则保留默认空串 */
    }

    /* 解析 PVH memmap（Lesson 4 新增） */
    parse_pvh_memmap(si);
}

/* ---------------------------------------------------------------
 * mark_kernel_region — 把内核镜像区域标记为 MEM_KERNEL
 *
 * 内核镜像占用物理地址 [__kernel_start, __kernel_end)，
 * 这段不能被 PMM 当成"可用内存"分配出去。
 *
 * 做法：从链接脚本定义的符号取地址，
 *       添加一条 MEM_KERNEL 类型的 region。
 *
 * PMM 在初始化时会扫描所有 region：
 *   - MEM_USABLE → 标"可用"
 *   - MEM_KERNEL → 标"已用"（保留）
 *   - MEM_RESERVED / MEM_MMIO / MEM_ACPI → 标"已用"
 *
 * 这样即使 bootloader 报告的 mmap 把内核区域算成"可用"
 * （GRUB 不知道我们内核镜像占多大），
 * PMM 也会因为我们的覆盖标记而跳过这段。 */
static void mark_kernel_region(void) {
    paddr_t kstart = (paddr_t)(u64)&__kernel_start[0];
    paddr_t kend   = (paddr_t)(u64)&__kernel_end[0];
    add_region(kstart, kend - kstart, MEM_KERNEL);
}

/* ---------------------------------------------------------------
 * arch_boot_init — 启动信息解析入口
 *
 * 流程：
 *   1. 设置默认值（cmdline 空串，regions 指向静态缓冲）
 *   2. 检测启动方式（multiboot2 / PVH / 未知）
 *   3. 调用对应的解析函数（cmdline + mmap）
 *   4. 追加内核镜像区域（覆盖 bootloader 报告的"可用"区）
 *   5. 把 regions_buf / regions_count 暴露给 boot_info
 *
 * 这样设计的好处：
 *   - entry.asm 不用关心启动方式，统一保存 EAX/EBX
 *   - boot.c 集中处理两种启动方式的差异
 *   - main.c 只看 boot_info 结构，不关心启动方式
 * --------------------------------------------------------------- */
void arch_boot_init(struct boot_info *info) {
    /* 第 1 步：设置默认值 */
    info->regions      = regions_buf;
    info->region_count = 0;
    info->cmdline      = "";

    /* 第 2 步：检测启动方式并解析 */
    enum boot_method m = detect_boot_method();
    switch (m) {
        case BOOT_MULTIBOOT2:
            parse_multiboot2(info);
            break;
        case BOOT_PVH:
            parse_pvh(info);
            break;
        case BOOT_UNKNOWN:
        default:
            panic(__FILE__, __LINE__,
                  "Not loaded by a known bootloader "
                  "(neither multiboot2 EAX magic nor PVH start_info magic)");
    }

    /* 第 3 步：追加内核镜像 region（覆盖任何冲突）
     *
     * 不管 bootloader 报告了什么，内核镜像这段都标 MEM_KERNEL，
     * 防止 PMM 误把内核代码当成空闲内存分配出去。 */
    mark_kernel_region();

    /* 第 4 步：把解析结果暴露给 boot_info */
    info->regions      = regions_buf;
    info->region_count = regions_count;
}
