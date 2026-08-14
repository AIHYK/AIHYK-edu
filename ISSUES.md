# AIHYK-edu 问题清单与 0.2 发布决策

> 来源：4 路并行代码审计（kernel core / arch layer / worklog 挖掘 / 可扩展性评估）
> 所有 `file:line` 引用基于当前 main 分支（commit 96c7c4f）
> 审计记录见 `/home/z/my-project/worklog.md` Task 1-a / 1-b / 1-c / 1-d

---

## 决策摘要

### 推荐方案：**发 edu 0.2，只修 C 类（11 项，约 1-2 天），然后 fork 出 pro 分支**

| 类别 | 数量 | 处置 | 理由 |
|---|---|---|---|
| **C 类** 真 bug / 文档与代码不符 / 测试不诚实 | 11 | **edu 0.2 修** | 这些会**误导学生**，比"没有 SMP"严重得多。文档说一套代码做一套是教学致命伤。 |
| **D 类** pro 必需的架构重铸 | 18 | **延后 pro** | per-process 页表、SYSCALL/SYSRET、SMP 等。在 edu 改了反而模糊 edu/pro 边界，违反"toy"承诺。 |
| **B 类** 已知简化，注释已标注 | 9 | **保留** | 教学合理简化，注释明确说明"为什么简化"+ "产品级要怎么做"。 |
| **A 类** 教学合理简化 | 14 | **保留** | round-robin、first-fit、16 tasks 等教学经典做法。 |

### 为什么不直接开 pro

- 带着已知文档错误（C3/C4/C5）和假测试（C6/C7）开 pro，pro 第一步又要返工修这些
- C 类工作量极小（~2 小时编码），但能显著提升教学可信度
- edu 0.2 作为"教学版最后一次稳定快照"，pro 从此 fork，分支关系清晰

### 为什么不在 edu 里把 D 类也修了

- D 类的核心是 per-process 页表（~2-3 周），这会让 edu 失去"toy"定位
- 共享 PML4、int 0x80、静态表——这些是 edu 的**教学简化**，注释明确标注，发 0.2 不应改
- pro 才是"把这些简化重铸成生产形态"的分支

---

## C 类：edu 0.2 应修（11 项）

> 标准：真 bug / 文档与实现不符 / 测试不诚实 / 会误导学生。
> 修复后 edu 0.2 应保持 179/179 测试通过（部分 C 类修复会要求同步改测试）。

### C1. SYS_write 无界用户指针读 → 内核 DoS

- **位置**: `kernel/syscall.c:97-100`，接口声明 `kernel/syscall.h:86-90`
- **问题**: 
  ```c
  case SYS_write:
      for (u64 i = 0; i < len; i++) arch_console_putchar(buf[i]);
  ```
  用户传 `len=2^60` + `buf` 近页边界 → 内核走过页尾 → #PF → `panic` → halt。**任何用户任务可让整个内核崩溃**。
- **影响**: 教学探索时极易触发（学生写 `sys_write(buf, 99999)` 就崩）。当前注释 `syscall.c:31` 写"教学简化：暂不校验...产品级要 copy_from_user + 范围检查"——但"不校验"和"会 panic"是两回事。
- **修复**: 加一个最小范围检查（5 分钟）：
  ```c
  if (len > 4096) return -1;  /* 教学简化：单次写上限 4KB 防溢出 */
  ```
  完整 `arch_copy_from_user` 留给 pro。
- **工作量**: 5 分钟

### C2. cap_recv_with_cap 静默丢弃 cap（无注释）

- **位置**: `kernel/cap.c:621-623`
- **问题**: 接收方 cspace 满时，附带的 cap 被静默丢弃，函数返回 `CAP_OK`。接收方**无法检测丢失**。
- **影响**: 真 bug 且**无任何注释标注**。学生会困惑"为什么我的 cap 没到"。审计 1-a 明确指出"receiver cannot detect the loss"。
- **修复**: 返回新错误码 `CAP_ERR_CSPACE_FULL`（或至少 `CAP_ERR_PARTIAL`），更新 `include/kernel/cap.h:214-220` 错误码枚举。
- **工作量**: 15 分钟（含改 1-2 个测试断言）

### C3. krealloc 文档说"原地截断"，实现总是复制

- **位置**: 文档 `include/kernel/mm.h:111-112`，实现 `kernel/mm.c:354-388`
- **问题**: 
  - 文档：`krealloc` "新块更小 → 原地截断"
  - 实现：**总是** malloc + memcpy + free，从不原地截断
- **影响**: **文档与代码不符，教学致命**。学生读 mm.h 学设计，看 mm.c 发现完全不是那么回事。
- **修复**: 改文档为"总是分配新块并复制（教学简化；产品级可加原地截断优化）"。或在 mm.c 真实现原地截断（更教学，但需 30 分钟）。
- **推荐**: 改文档（10 分钟）。
- **工作量**: 10 分钟

### C4. 调度器注释说"16 KB stack"，实际 8 KB

- **位置**: `kernel/sched.c:564` 注释，实际值 `arch/include/arch/task.h:210` `TASK_STACK_SIZE = 8192`
- **问题**: 注释声称调度器 halt-loop 假设 16KB 栈，但 `TASK_STACK_SIZE = 8192`（8KB）。halt-loop 是"内核中最危险的代码"（1-a 评），栈假设错误会让学生误判安全余量。
- **影响**: 文档错误。1-a 审计明确指出"wrong, TASK_STACK_SIZE = 8192"。
- **修复**: 改注释 `16 KB` → `8 KB`（2 分钟）。
- **工作量**: 2 分钟

### C5. cspace 文档说"是 task_struct 字段"，实际是 kmalloc 的指针

- **位置**: 文档 `include/kernel/cap.h:249-263` 和 `kernel/cap.c:174-178`，实际 `kernel/cap.c:183-190`
- **问题**: 
  - 文档声称 "cspace 是 task_struct 字段"
  - 实际 `task_struct` 里是 `struct cspace *cspace` 指针，cspace 结构体由 `kmalloc` 单独分配
- **影响**: 文档与实现不符。学生读 cap.h 学 cspace 模型，看 cap.c 发现是另一回事。
- **修复**: 改 cap.h 注释为"cspace 由 task_struct 持有指针，结构体通过 kmalloc 单独分配（生命周期与 task 一致）"。
- **工作量**: 10 分钟

### C6. ktest S4_03 "创建超过 MAX_TASKS" 测试被弱化

- **位置**: `kernel/ktest.c:977-985`，断言在 `:995-996`
- **问题**: 测试声称"创建超过 MAX_TASKS 应失败"，但：
  - 为了"避免堆耗尽"只创建 10 个任务（而非 16）
  - 断言写成 `last_ok || overflow_fail`（OR），**任一成立即通过**
  - 实际语义变成"第 10 个成功 OR 第 11 个失败"——几乎恒真
- **影响**: **测试不诚实**。声称测试边界，实际是 tautology。1-a 审计明确指出"passes if either holds, much weaker than 16th task fails"。
- **修复**: 拆成两个独立 assertion：`ASSERT(last_ok == true)` + `ASSERT(overflow_fail == true)`，并把创建数提到 16。
- **工作量**: 15 分钟

### C7. demo.c L8 内存泄漏检查被丢弃

- **位置**: `kernel/demo.c:600`
- **问题**: 
  ```c
  (void)frames_after;
  (void)frames_before;
  ```
  L8 用户态 demo 声称"检查 page leak"，实际把前后帧数对比**显式丢弃**。只有 L9 的 leak 检查真正执行。
- **影响**: **测试假通过**。学生以为 L8 也验证了无泄漏，实际没有。
- **修复**: 加真正的断言 `KASSERT(frames_after == frames_before)`，或显式注释"L8 不检查泄漏（已知 X 帧开销），仅 L9 检查"。
- **工作量**: 20 分钟

### C8. 8 份 print_dec / print_hex 复制粘贴

- **位置**: `kernel/panic.c` / `kernel/sched.c` / `kernel/ipc.c` / `kernel/cap.c` / `kernel/demo.c` / `kernel/ktest.c` / `kernel/cap_test.c` / `kernel/test.c` / `kernel/main.c`（9 个文件，每个一份近似副本）
- **问题**: 同一个十进制/十六进制打印函数被复制 8 份。1-a 审计明确指出"copy-paste smell, unflagged"。
- **影响**: 代码异味。教学项目尤其应该展示 DRY。修改任一份不会同步其他份，潜在维护陷阱。
- **修复**: 提取到 `kernel/panic.c`（或新建 `kernel/util.c`），其他文件 `#include` 或调用。9 份去重。
- **工作量**: 30 分钟

### C9. PVH boot magic 在解引用后才校验

- **位置**: `arch/x86_64/boot.c:356-357`
- **问题**: 
  ```c
  struct hvm_start_info *info = (struct hvm_start_info *)x86_64_boot_ebx;  // :356 解引用
  if (info->magic != 0x336ec578) { ... }                                   // :357 才校验
  ```
  如果 EBX 是垃圾值（非 PVH 加载），:356 行的强制类型转换本身没问题，但 :357 行的 `info->magic` 解引用垃圾指针 → #PF → triple fault。multiboot2 路径（EAX 校验）在解引用前。
- **影响**: 潜在 triple fault，无注释。1-b 审计明确指出"can #PF/triple-fault before the magic check fires"。
- **修复**: 先校验 EBX 是否在合理物理地址范围（或至少加注释说明"PVH 协议保证 EBX 有效，否则 triple fault 是预期行为"）。
- **工作量**: 10 分钟（加注释）或 30 分钟（真校验）

### C10. panic.c INT_MIN 时 `n = -n` 是未定义行为

- **位置**: `kernel/panic.c:39`
- **问题**: 
  ```c
  if (n < 0) { arch_console_putchar('-'); n = -n; }
  ```
  若 `n == INT_MIN`，`-n` 溢出，C 标准下是 UB。1-a 审计明确指出"admitted UB if anyone calls panic(file, INT_MIN, msg)"。
- **影响**: 真 UB。虽然实际触发概率极低（谁会传 INT_MIN 行号？），但**教学项目不应展示 UB 示例**。
- **修复**: 用 `unsigned int u = (unsigned int)n; if (n < 0) { putchar('-'); u = (unsigned int)(-n); }` 或 `u = -(unsigned)n` 标准写法。
- **工作量**: 5 分钟

### C11. PMM frame 0 未永久保留（ktest 自己标"严重建议修复"）

- **位置**: `arch/x86_64/pmm.c`，ktest summary 自标"严重（NULL deref 风险），建议修复"
- **问题**: PMM 位图不永久保留 frame 0。若有人 `free_frame(0)`，位图 bit 0 被清，下次 `alloc_frame()` 返回 0（物理地址 0 = NULL）。后续任何把它当指针用的代码 → NULL deref。
- **影响**: ktest summary 自己标"严重"。1-c 挖掘出这是已承认但未修的限制。
- **修复**: `arch_pmm_init` 末尾 `alloc_frame()` 占用 frame 0 一次（永不释放），或位图初始化时直接置 bit 0 = used。
- **工作量**: 5 分钟

### C 类汇总工作量

| 项 | 工作量 |
|---|---|
| C1 SYS_write 上限 | 5 min |
| C2 cap-drop 错误码 | 15 min |
| C3 krealloc 文档 | 10 min |
| C4 栈大小注释 | 2 min |
| C5 cspace 文档 | 10 min |
| C6 S4_03 测试 | 15 min |
| C7 L8 leak check | 20 min |
| C8 print_dec 去重 | 30 min |
| C9 PVH magic 注释 | 10 min |
| C10 INT_MIN UB | 5 min |
| C11 frame 0 保留 | 5 min |
| **合计** | **~2 小时编码 + 4 小时验证** |

---

## D 类：pro 必需，edu 不修（18 项）

> 这些是 pro 版"从 toy 到 production-grade toy"的核心重铸。在 edu 修会违反"toy"承诺。
> 详见 pro 路线图（待写）。

### D1. Per-process 页表（替换共享 PML4）

- **位置**: `arch/include/arch/user.h:38-45` 显式声明"shared address space"
- **为什么 edu 不修**: 共享地址空间是教学简化（注释明确），让 syscall 直接解引用用户指针，代码极简。
- **为什么 pro 必需**: 阻塞 ELF 加载（标准 0x400000 与当前固定 VMA 槽冲突）、COW fork、KPTI、真隔离、SMP。
- **杠杆**: 1-d 评"最高杠杆 1 周投资，解锁 5 个功能"。注意 `arch_vmm_load_cr3`（`mem.h:410`）已存在但**从未被调用**——基础设施半建成，从未接通。

### D2. SYSCALL/SYSRET 替换 int 0x80

- **位置**: `arch/x86_64/idt.c:198`（IDT[0x80] = DPL=3 中断门），`arch/x86_64/irq.c:248-251`
- **为什么 edu 不修**: `syscall.h:36-41` 注释明确"int 0x80 是经典做法（Linux 2.x 用了很久），教学清晰；后续优化课程可换 syscall 指令，syscall.h 接口不变"。
- **为什么 pro 必需**: ~5x 性能；现代 CPU 路径；syscall.h 接口已预留。

### D3. #DF / #MC / NMI 无 IST

- **位置**: `arch/x86_64/idt.c:177`（256 项全 `ist=0`），`arch/include/arch/idt.h:43-44` 承认
- **为什么 edu 不修**: 单核 + 简单工作负载下极少触发。
- **为什么 pro 必需**: 内核栈溢出/损坏 → #DF 复用损坏栈 → triple fault → **无法调试**。pro 开发期频繁崩溃时这是噩梦。

### D4. arch/kernel 边界清理

- **位置**: 多处泄漏
  - `kernel/sched.c:93,356` `extern u8 stack_top_64[]`（reach into entry.asm）
  - `kernel/sched.c:650` `arch_tss_set_sp0`（调度器知道 x86 TSS）
  - `kernel/sched.c:697,787` 硬连 `arch_pit_get_tick_count`（换 APIC timer 要改调度器）
  - `arch/x86_64/exceptions.c:442` `sched_exit_with_code`（**arch 反向依赖 kernel**）
  - `kernel/main.c:151` + `kernel/sched.c:602-625` 直接 `__asm__ volatile ("hlt")` 而非 `arch_halt()`
- **为什么 edu 不修**: 当前能跑通，泄漏不致命。
- **为什么 pro 必需**: 加 APIC/SMP/新驱动会让泄漏爆炸。需抽象 `arch_context_switch_prepare()` / `arch_time_now()` / `arch_kernel_stack_top()`。

### D5. 统一 wait_queue + condition 原语

- **位置**: `kernel/sched.c:589-628` halt-loop + 4 条 ad-hoc 等待路径（sched_sleep / IPC 阻塞 / SYS_waitpid / "无可运行任务"）
- **为什么 edu 不修**: 4 条路径各自有注释，能工作。
- **为什么 pro 必需**: 1-c 挖掘出 worklog 中 **6+ 个 bug**（W3/W4/W5/W6/W8/W9/W12）都指向同一根因——**没有"等待事件"统一原语**。再加新阻塞原语（wait-for-fd、wait-for-IRQ）等于复制粘贴 bug。

### D6. mm.c kfree 合并邻居

- **位置**: `kernel/mm.c:281-290`，注释自己写了"改进方向（后续可做）"
- **为什么 edu 不修**: 注释明确标注简化，1MB 堆教学够用。
- **为什么 pro 必需**: 任何持续工作负载下堆会碎片化到死。

### D7. SMEP / SMAP / NX 启用

- **位置**: `arch/x86_64/entry.asm` 只动 CR4.PAE；`PAGE_FLAG_NX`（`mem.h:158`）定义但**从未使用**
- **为什么 edu 不修**: 教学简化。
- **为什么 pro 必需**: 内核可执行用户代码、用户页可写可执行——安全洞会随功能数线性增长。SMEP/SMAP 顺手开 CR4 几乎免费。

### D8. 动态化关键静态表

- **位置**: 
  - `all_tasks[16]`（`sched.h:110`）
  - `channel_table[32]`（`ipc.h:192`）
  - `cspace[32]`（`cap.h:178`）
- **为什么 edu 不修**: 静态表注释明确"方便遍历，教学够用"。
- **为什么 pro 必需**: VFS 需要 per-task fd 表、inode、page cache——**任何"真"子系统都需要动态表**。

### D9. APIC + per-CPU 准备

- **位置**: `arch/x86_64/cpu.c:149-151` `arch_get_cpu_id` 直接 `return 0`
- **为什么 edu 不修**: 单核玩具。
- **为什么 pro 必需**: SMP 前置。1-d 列出 20 处全局可变状态无 per-CPU 划分。真 SMP 仍可延后，但 `arch_get_cpu_id` 从 stub 变真是基础。

### D10. PMM >4GB 支持

- **位置**: `arch/x86_64/pmm.c:78` `PMM_MAX_FRAMES = 1M`（128KB 位图 → 4GB 上限），超出静默截断（`pmm.c:296-299`）
- **为什么 edu 不修**: 128MB QEMU 够用。
- **为什么 pro 必需**: 真机/VM 普遍 >4GB，静默截断是隐蔽 bug。

### D11. ELF 加载器

- **位置**: 当前 `user_image.S` 用 `.incbin` 把 flat binary 嵌入内核 `.rodata`
- **为什么 edu 不修**: 无 FS、无 exec()，incbin 是最简方案。
- **为什么 pro 必需**: 标准 ELF 加载到 0x400000。**前置依赖 D1**（per-process 页表）。

### D12. VFS + fd 表 + open/read/write/close

- **位置**: 当前 `syscall.h:61-70` 仅 7 个 syscall，无文件相关
- **为什么 edu 不修**: 无 FS。
- **为什么 pro 必需**: 任何"生产"工作负载的基础。

### D13. 磁盘驱动框架

- **位置**: `arch/x86_64/irq.c:71` `irq_table[16]` 是唯一驱动挂载点
- **为什么 edu 不修**: 无磁盘需求。
- **为什么 pro 必需**: 1-d 评"`arch_irq_register` 是代码库里最干净的扩展点"。但 early-EOI（`irq.c:261`）破坏 level-triggered，需补 bottom-half。

### D14. 网络栈

- **位置**: 完全缺失
- **为什么 pro 必需**: 1-d 评"~2-5k 行，需 PCI + NIC + DMA + sk_buff/socket 从零"。最大块，最后做。

### D15. 需求分页 / mmap / VMAs

- **位置**: `arch/x86_64/exceptions.c:399` #PF 直接杀用户任务（:442），无"按需分配"路径
- **为什么 edu 不修**: 当前 VMM 是"allocate-and-map-once"。
- **为什么 pro 必需**: 页表操作原语已存在，缺 VMA 策略层。~2 周。

### D16. 真调度器（优先级 / CFS）

- **位置**: `kernel/sched.c:511-678` round-robin 硬编码
- **为什么 edu 不修**: 教学经典。
- **为什么 pro 必需**: 等有真实工作负载再调。

### D17. COW fork

- **位置**: 无 fork syscall
- **为什么 pro 必需**: **前置依赖 D1**（per-process 页表），否则物理上不可能。

### D18. slab 分配器

- **位置**: `kernel/mm.c` first-fit 单链表
- **为什么 edu 不修**: first-fit 教学清晰。
- **为什么 pro 必需**: 高频小对象分配性能。**前置依赖 D6**（kfree 合并）。

---

## B 类：已知简化，注释已标注，保留（9 项）

> 这些是 edu 的"教学合理简化"，注释明确说明"为什么简化" + "产品级怎么做"。发 0.2 不改。

| # | 位置 | 简化 | 注释标注 |
|---|---|---|---|
| B1 | `kernel/mm.c:281-290` | kfree 不合并邻居 | "简化：加到链表头，不主动合并" + 列出改进方向 |
| B2 | `kernel/syscall.c:166-176` | SYS_waitpid 轮询而非阻塞 | "教学简化：阻塞等待需要 per-child wait queue" |
| B3 | `include/kernel/cap.h:113` | cap object 裸内核指针 | "教学内核是内核态 cap，简化为裸指针" |
| B4 | `include/kernel/cap.h:183-188` | 仅 CAP_TYPE_CHANNEL | 列出 PAGE/TASK/IRQ 为 future，未实现 |
| B5 | `arch/x86_64/vmm.c:382` | huge page split 未实现（panic） | "panic（拆分 huge page 未实现）" |
| B6 | `include/kernel/mm.h:91` | 堆不归还物理页给 PMM | "简化设计" |
| B7 | `arch/include/arch/user.h:38-45` | 共享 PML4（内核+用户） | "shared address space 设计" |
| B8 | `kernel/ktest.c` summary | IPC timeout 单任务下不可靠 | "中等，正常多任务下不受影响" |
| B9 | `include/kernel/syscall.h:36-41` | int 0x80 而非 SYSCALL | "经典做法，教学清晰；后续可换" |

---

## A 类：教学合理简化，保留（14 项）

> 教学经典做法，不需要任何注释辩护。

| # | 简化 | 位置 |
|---|---|---|
| A1 | round-robin 调度（无优先级） | `kernel/sched.c` |
| A2 | first-fit kmalloc | `kernel/mm.c` |
| A3 | bitmap PMM（非 buddy） | `arch/x86_64/pmm.c` |
| A4 | MAX_TASKS = 16 静态 | `include/kernel/sched.h:110` |
| A5 | IPC_MAX_CHANNELS = 32 静态 | `include/kernel/ipc.h:192` |
| A6 | CAP_SLOTS_PER_TASK = 32 静态 | `include/kernel/cap.h:178` |
| A7 | 1MB 固定堆 | `kernel/mm.c:71` |
| A8 | 64B IPC payload | `include/kernel/ipc.h:216` |
| A9 | 无 SMP（单核） | `kernel/sched.c:508-510` |
| A10 | 无 KASLR / stack canary | README "What This Is NOT" |
| A11 | 无 vDSO | 无 |
| A12 | 用户程序 .incbin 嵌入 | `user_image.S` |
| A13 | 静态 init task（不 kmalloc） | `kernel/sched.c:73` |
| A14 | cap lineage = u64（非父子树） | `include/kernel/cap.h:75-79` |

---

## edu 0.2 发布检查清单

- [ ] 修 C1-C11（~2 小时编码）
- [ ] `make` 零警告零错误
- [ ] `make run-elf` + `make run-iso` 179/179 测试全 PASS
- [ ] 更新 README "Bugs We Hit & Fixed" 表加入 C 类修复
- [ ] 更新 `worklog.md` 记录 0.2 修复
- [ ] bump 版本 `include/kernel/types.h` 0.1.0 → 0.2.0
- [ ] git tag `v0.2.0-edu`
- [ ] **fork 出 pro 分支**，pro 从 v0.2.0-edu 开始 D 类重铸

## pro 路线图（Tier 0 → Tier 1，待详化）

### Tier 0 — 地基重铸（~2-3 周，不加用户可见功能）
1. D1 per-process 页表（解锁 D11/D17/D7 KPTI）
2. D3 #DF IST
3. D4 arch/kernel 边界清理
4. D5 统一 wait_queue
5. D6 kfree 合并
6. D2 SYSCALL/SYSRET
7. D7 SMEP/SMAP/NX 顺手开
8. D9 APIC + per-CPU 准备（真 SMP 延后）

### Tier 1 — 功能落地
1. D11 ELF 加载器
2. D12 VFS + fd 表
3. D13 磁盘驱动
4. D15 需求分页 / mmap
5. D14 网络栈（最后）

### Tier 2 — 深度 polish（可选）
D16 真调度器 / D18 slab / D10 >4GB PMM / 真 SMP / KASLR
