# ================================================================
# Makefile - Build + Run + Clean + ISO (x86-64 长模式 + multiboot2 + PVH)
#
# 支持两种启动方式（同一份 kernel.elf）：
#
#   A. PVH 直接启动（推荐，快速调试）
#      make run-elf
#      -> qemu-system-x86_64 -kernel kernel.elf
#      QEMU 通过 .note.pvh ELF Note 识别内核，用 PVH 协议加载
#
#   B. ISO + GRUB 启动（兼容 VMware / 实体机）
#      make run      或    make run-iso
#      -> grub-mkrescue 生成 aihyk.iso，GRUB 用 multiboot2 加载
#
# Usage:
#   make           Compile kernel (produces kernel.elf + kernel.bin)
#   make run-elf   PVH direct boot (qemu -kernel kernel.elf) <- 最快
#   make run       ISO boot (build ISO + qemu -cdrom)
#   make iso       Build a bootable ISO image (aihyk.iso)
#   make run-iso   Run the ISO in QEMU
#   make clean     Remove build artifacts
#   make debug     ISO + GDB (-s -S, wait for gdb to connect)
#   make debug-elf PVH + GDB (-s -S, wait for gdb to connect)
# ================================================================

# Architecture (x86-64 长模式；目录实现位于 arch/x86_64/)
ARCH ?= x86_64

# Directory structure
ARCH_DIR    = arch/$(ARCH)
KERNEL_DIR  = kernel
INCLUDE_DIR = include

# Header search paths
INCLUDES = -I$(INCLUDE_DIR) -Iarch/include -I$(ARCH_DIR)

# C compiler flags (64-bit)
# -m64: 生成 64 位代码
# -ffreestanding -nostdlib -nostdinc: 不依赖标准库（内核环境）
# -fno-pie -no-pie: 不生成位置无关代码（内核加载到固定地址 1MB）
# -fno-stack-protector: 不插入栈保护（内核没有 __stack_chk_fail）
# -mcmodel=large: 大内存模型（允许代码在任意 64 位地址，不限于低 2GB）
# -mno-red-zone: 禁用 red zone（中断安全，中断处理程序不会破坏栈）
# -g: 调试信息
CFLAGS = -m64 -ffreestanding -nostdlib -nostdinc -fno-pie -fno-stack-protector \
	-mcmodel=large -mno-red-zone \
	-Wall -Wextra -Wno-unused-parameter -g \
	$(INCLUDES)

# Linker flags (64-bit)
LDFLAGS = -m elf_x86_64 -T linker.ld -nostdlib -no-pie

# Source files
# 【Lesson 3】
# - idt.c/isr.asm:  IDT (Interrupt Descriptor Table) + 中断 stub
# - exceptions.c:   CPU 异常处理（#DE/#GP/#PF 等打印 + panic）
# - pic.c:          8259 PIC 重映射 + EOI
# - pit.c:          8254 PIT 定时器（100Hz tick）
# - irq.c:          IRQ 注册表 + 中断分发
# - keyboard.c:     PS/2 键盘驱动（IRQ1）
# 【Lesson 4 新增】
# - pmm.c:          PMM (Physical Memory Manager) — 位图物理页分配器
# - vmm.c:          VMM (Virtual Memory Manager) — 4 级页表 + 递归映射
# - mm.c:           内核堆 kmalloc/kfree（first-fit 链表）
# - boot.c 增强：   解析 multiboot2 mmap / PVH memmap
# - exceptions.c 增强：#PF 详细解码
# 【Lesson 5 新增】
# - task.c/switch.asm: 任务管理（arch_context_switch + arch_task_stack_init）
# - sched.c:        任务调度器（round-robin + 时间片 + sleep/exit/reaper）
# - irq.c 增强：    pit handler 调用 sched_tick 驱动调度
# - main.c 增强：   创建 demo 任务 + sched_yield 启动调度
# 【Lesson 6 新增】
# - ipc.c:          IPC 子系统（通道 + 阻塞消息传递 + 直送优化）
# - sched.c 增强：  sched_wake + IPC 超时唤醒（wake_sleeping_tasks 扩展）
# - task.h 增强：   task_struct 增加 IPC 等待字段（next_waiter / wait_kind /
#                  ipc_buf / ipc_out_*  / ipc_timeout_tick / ipc_result）
# - main.c 增强：   IPC demo（logger server + 多 client + RPC calculator）
# 【Lesson 7 新增】
# - cap.c:          Capability 框架（cap_create/mint/revoke + cap_send/recv +
#                  cap_send_with_cap/recv_with_cap + cap_destroy_channel）
# - ipc.c 增强：    channel 指针版内部 API（ipc_send_on_channel 等）+ cap_snap
# - sched.c 增强：  sched_get_task_by_index/by_id（遍历任务用）+ cap_cspace_init/destroy
# - task.h 增强：   task_struct 增加 cspace + ipc_recv_cap_* 字段
# - main.c 增强：   L7 cap demo（file-server + trusted-cli + untrusted-cli）
# 【Lesson 8 新增】
# - user.c/user.asm: 用户态管理（TSS + ring 3 进入 + 用户页映射）
# - syscall.c:       syscall 分发（int 0x80 → C handler）
# - entry.asm 增强：GDT 加 user code/data + TSS 描述符；.bss 加 TSS 结构
# - idt.c 增强：     IDT[0x80] = DPL=3 中断门（syscall 门）
# - irq.c 增强：     vec 0x80 路由到 syscall_handler
# - vmm.c 增强：     arch_vmm_map_page 把 USER flag 传播到中间页表项
# - sched.c 增强：   sched_create_user_task + user_task_main + TSS.sp0 更新
# - task.h 增强：    task_struct 增加 is_user/kstack_top/user_* 字段
# - main.c 增强：    L8 demo（启动 2 个 ring-3 用户任务）
# - user/hello.asm:  用户程序（flat binary，nasm -f bin）
# - user_image.S:    .incbin 把 hello.bin 嵌入内核 .rodata
ARCH_C_SRCS   = $(ARCH_DIR)/boot.c $(ARCH_DIR)/cpu.c $(ARCH_DIR)/console.c \
	$(ARCH_DIR)/idt.c $(ARCH_DIR)/exceptions.c \
	$(ARCH_DIR)/pic.c $(ARCH_DIR)/pit.c \
	$(ARCH_DIR)/irq.c $(ARCH_DIR)/keyboard.c \
	$(ARCH_DIR)/pmm.c $(ARCH_DIR)/vmm.c \
	$(ARCH_DIR)/task.c $(ARCH_DIR)/user.c
ARCH_ASM_SRCS = $(ARCH_DIR)/entry.asm $(ARCH_DIR)/isr.asm $(ARCH_DIR)/switch.asm \
	$(ARCH_DIR)/usermode.asm
KERNEL_C_SRCS = $(KERNEL_DIR)/main.c $(KERNEL_DIR)/panic.c $(KERNEL_DIR)/mm.c \
	$(KERNEL_DIR)/sched.c $(KERNEL_DIR)/ipc.c $(KERNEL_DIR)/cap.c \
	$(KERNEL_DIR)/cap_test.c $(KERNEL_DIR)/ktest.c $(KERNEL_DIR)/syscall.c \
	$(KERNEL_DIR)/demo.c $(KERNEL_DIR)/test.c $(KERNEL_DIR)/util.c
# user_image.S 是预处理汇编（gcc 编译，用 .S 后缀）
USER_IMAGE_SRC = user_image.S

ALL_C_SRCS = $(ARCH_C_SRCS) $(KERNEL_C_SRCS)

# Object files
ARCH_C_OBJS   = $(ARCH_C_SRCS:.c=.o)
ARCH_ASM_OBJS = $(ARCH_ASM_SRCS:.asm=.o)
KERNEL_C_OBJS = $(KERNEL_C_SRCS:.c=.o)
# user_image.S → user_image.o（预处理汇编，用 gcc 编译）
USER_IMAGE_OBJ = user_image.o

ALL_OBJS = $(ARCH_ASM_OBJS) $(ARCH_C_OBJS) $(KERNEL_C_OBJS) $(USER_IMAGE_OBJ)

# 【Lesson 8】用户程序 flat binary
# nasm -f bin 组装，入口在偏移 0
# 【Lesson 9 新增】crash.bin — 故意崩溃的用户程序
USER_BIN_DIR = user
USER_BIN = $(USER_BIN_DIR)/hello.bin $(USER_BIN_DIR)/crash.bin
USER_ASM = $(USER_BIN_DIR)/hello.asm $(USER_BIN_DIR)/crash.asm

# ISO related
ISO_DIR = iso
ISO_IMAGE = aihyk.iso

# GRUB module directory
# GRUB target name for BIOS booting.
# NOTE: this is GRUB's own platform identifier ("i386-pc" = 32-bit BIOS
# bootloader), NOT our kernel architecture. Do NOT rename when changing
# the kernel arch directory.
GRUB_DIR ?= /usr/lib/grub/i386-pc

# QEMU 内存（MB）
QEMU_MEM ?= 128

# Default target
all: kernel.bin

# Extract raw binary from ELF
kernel.bin: kernel.elf
	objcopy -O binary kernel.elf kernel.bin

# Link all object files
# 【Lesson 8/9】依赖 USER_BIN（order-only prereq）：user_image.o 用 .incbin 引入
#   hello.bin + crash.bin，必须先生成。用 | （order-only）避免改 bin 触发全量重链。
kernel.elf: $(ALL_OBJS) | $(USER_BIN)
	ld $(LDFLAGS) -o kernel.elf $(ALL_OBJS)

# 【Lesson 8】组装用户程序 flat binary
# 【Lesson 9】新增 crash.bin 规则
$(USER_BIN_DIR)/hello.bin: $(USER_BIN_DIR)/hello.asm
	nasm -f bin -o $@ $<
$(USER_BIN_DIR)/crash.bin: $(USER_BIN_DIR)/crash.asm
	nasm -f bin -o $@ $<

# Assemble .asm -> .o (elf64, bits controlled by [bits 32]/[bits 64])
$(ARCH_DIR)/%.o: $(ARCH_DIR)/%.asm
	nasm -f elf64 -o $@ $<

# Compile .c -> .o
%.o: %.c
	gcc $(CFLAGS) -c -o $@ $<

# 【Lesson 8】编译预处理汇编 .S -> .o（用 gcc，走 C 预处理）
user_image.o: user_image.S | $(USER_BIN)
	gcc $(CFLAGS) -c -o $@ $<

# ================================================================
# PVH 直接启动（推荐，最快速的调试方式）
# ================================================================
run-elf: kernel.elf
	qemu-system-x86_64 -L $(QEMU_DATADIR) -kernel kernel.elf -m $(QEMU_MEM) -no-reboot

# PVH + GDB 调试
debug-elf: kernel.elf
	qemu-system-x86_64 -L $(QEMU_DATADIR) -kernel kernel.elf -m $(QEMU_MEM) -s -S

# ================================================================
# ISO + GRUB 启动（兼容 VMware / 实体机）
# ================================================================

run: iso
	qemu-system-x86_64 -L $(QEMU_DATADIR) -cdrom $(ISO_IMAGE) -m $(QEMU_MEM) -no-reboot

debug: iso
	qemu-system-x86_64 -L $(QEMU_DATADIR) -cdrom $(ISO_IMAGE) -m $(QEMU_MEM) -s -S

iso: kernel.elf
	@rm -rf $(ISO_DIR)
	@mkdir -p $(ISO_DIR)/boot/grub
	@cp kernel.elf $(ISO_DIR)/boot/
	@echo 'set timeout=0' > $(ISO_DIR)/boot/grub/grub.cfg
	@echo 'set default=0' >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo 'menuentry "AIHYK Hybrid Kernel" {' >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '  multiboot2 /boot/kernel.elf' >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '}' >> $(ISO_DIR)/boot/grub/grub.cfg
	@if [ -d "$(GRUB_DIR)" ]; then \
	grub-mkrescue --directory=$(GRUB_DIR) -o $(ISO_IMAGE) $(ISO_DIR); \
	else \
	grub-mkrescue -o $(ISO_IMAGE) $(ISO_DIR) 2>/dev/null \
	|| (echo "ERROR: grub-mkrescue failed. Cannot find GRUB modules."; \
	echo "Install: sudo apt install grub-pc-bin xorriso mtools"; \
	echo "Or specify: make iso GRUB_DIR=/path/to/grub/i386-pc"; \
	rm -rf $(ISO_DIR) && false); \
	fi
	@rm -rf $(ISO_DIR)
	@echo "Built $(ISO_IMAGE)"

run-iso: iso
	qemu-system-x86_64 -L $(QEMU_DATADIR) -cdrom $(ISO_IMAGE) -m $(QEMU_MEM) -no-reboot

# Clean build artifacts
clean:
	rm -f $(ALL_OBJS) kernel.elf kernel.bin $(ISO_IMAGE) $(USER_BIN)
	rm -rf $(ISO_DIR)

.PHONY: all run run-elf run-iso debug debug-elf iso clean
