# 最小 KVM VM demo 实验记录

## 1. Hello/KVM 最小闭环

第一阶段提交保留了最小 VM/vCPU/KVM_RUN/KVM_EXIT_IO/KVM_EXIT_HLT 闭环。

## 2. CPUID、MMIO、中断和异常综合实验

本阶段在最小闭环上继续验证：

- `KVM_SET_CPUID2` 改写 guest 可见 CPUID vendor string。
- 访问未注册 GPA `0x5000` 触发 `KVM_EXIT_MMIO`。
- MMIO read 由 userspace 填 `run->mmio.data[]` 返回给 guest。
- `run->request_interrupt_window` 触发 `KVM_EXIT_IRQ_WINDOW_OPEN`。
- `KVM_INTERRUPT` 注入 vector `0x20`，guest 通过 IVT 跳到 handler。
- `int3` 触发 #BP/vector 3，guest 通过 IVT[3] 跳到 handler。
- `ud2` 触发 #UD/vector 6，guest handler 修改栈上返回 IP 后跳过 faulting instruction。
- `KVM_GET_REGS` 在 VM-exit 后观察 vCPU 的 `RIP/RSP/RFLAGS/RAX/RBX/RCX/RDX`。
- `KVM_SET_REGS` 修改 `RAX.AL`，让 guest 后续 `out` 输出被 userspace 改写后的值。
- `trace-cmd` 可对照底层 `kvm_entry` / `kvm_exit` / `kvm_userspace_exit`。

预期输出：

```text
KVM API version: 12
guest output: KVMDEMO12345
KVM_EXIT_MMIO: addr=0x5000 len=2 is_write=1 data=0x7856
KVM_EXIT_MMIO regs: rip=0x104a rsp=0x2000 rflags=0x12 rax=0xa rbx=0x444d rcx=0x3534 rdx=0x314f
KVM_EXIT_MMIO: addr=0x5000 len=2 is_write=0 data=0x4f4b
KVM_EXIT_MMIO regs: rip=0x104a rsp=0x2000 rflags=0x12 rax=0xa rbx=0x444d rcx=0x3534 rdx=0x314f
OK
B
U
?KVM_EXIT_IO before KVM_SET_REGS regs: rip=0x105c rsp=0x2000 rflags=0x12 rax=0x4b3f rbx=0x444d rcx=0x3534 rdx=0x314f
KVM_EXIT_IO after KVM_SET_REGS regs: rip=0x105c rsp=0x2000 rflags=0x12 rax=0x4b52 rbx=0x444d rcx=0x3534 rdx=0x314f
R
KVM_EXIT_IRQ_WINDOW_OPEN regs: rip=0x1066 rsp=0x2000 rflags=0x212 rax=0x4b0a rbx=0x444d rcx=0x3534 rdx=0x314f
KVM_EXIT_IRQ_WINDOW_OPEN: inject IRQ 0x20
I
KVM_EXIT_HLT regs: rip=0x1067 rsp=0x2000 rflags=0x212 rax=0x4b0a rbx=0x444d rcx=0x3534 rdx=0x314f
KVM_EXIT_HLT
io exits: 25
mmio exits: 2
```

---

# 最小 KVM VM demo 与 CPU 虚拟化第一章笔记

## 1. 实验目标

本文围绕《Linux 系统虚拟化原理与实现》第 1 章“CPU 虚拟化”，用一个最小 KVM userspace demo 验证 CPU 虚拟化的核心闭环：

```text
userspace VMM
    -> ioctl(KVM_RUN)
    -> KVM 内核模块
    -> VM-entry
    -> guest 指令执行
    -> VM-exit
    -> KVM 返回 userspace
    -> userspace 根据 exit reason 模拟或结束
```

这个 demo 不是完整 VMM，不加载 Linux kernel，不模拟 PCI/virtio/中断控制器；它只创建一个 VM、一个 vCPU、一段 guest RAM，并让 guest 执行几条 x86 real-mode 指令。

## 2. 第一章核心概念

### 2.1 trap-and-emulate

CPU 虚拟化的基本思路不是逐条解释 guest 指令，而是：

```text
guest 普通指令直接在物理 CPU 上执行
guest 执行需要 hypervisor 接管的敏感操作时，CPU 退出到 hypervisor
hypervisor 模拟这次操作的效果
然后继续运行 guest
```

在 KVM 中，userspace VMM 可见的结果就是：

```text
ioctl(KVM_RUN) 返回
struct kvm_run->exit_reason 告诉 userspace 为什么退出
```

### 2.2 VMX root / non-root

书中以 Intel VMX 解释：

```text
VMX root mode      : hypervisor / host 运行环境
VMX non-root mode  : guest 运行环境
```

当前机器是 AMD 平台，对应硬件机制是 SVM，但学习模型等价：

```text
Intel VMX root/non-root 约等于 AMD SVM host/guest 执行模式
Intel VMCS              约等于 AMD VMCB
Intel EPT               约等于 AMD NPT
```

本 demo 仍然使用统一的 Linux KVM API。userspace 不直接执行 VMX/SVM 指令，真正进入/退出 guest 的硬件操作由 KVM 内核模块完成。

### 2.3 VM-entry / VM-exit

在 userspace 视角：

```text
ioctl(vcpu_fd, KVM_RUN)
```

是进入 guest 的边界。

但需要区分：

```text
KVM_RUN 是 KVM UAPI
VMLAUNCH / VMRESUME / VMRUN 是内核 KVM 在硬件层执行的虚拟化指令
```

当 guest 执行 `out 0xe9, al` 时，KVM 把它暴露为：

```text
KVM_EXIT_IO
```

当 guest 执行 `hlt` 时，KVM 把它暴露为：

```text
KVM_EXIT_HLT
```

### 2.4 vCPU 生命周期

最小 vCPU 生命周期可以简化成：

```text
KVM_CREATE_VCPU
    -> mmap struct kvm_run
    -> 设置初始 sregs/regs
    -> KVM_RUN
    -> KVM_EXIT_IO
    -> userspace 处理 I/O
    -> KVM_RUN
    -> KVM_EXIT_HLT
    -> 清理资源
```

kvmtool 的正式 vCPU 也是这个模型，只是它还包含多 vCPU 线程、Linux bzImage 加载、设备模型、virtio、中断控制器等工程化内容。

## 3. 最小 KVM API 生命周期

Demo 文件：

```text
tests/kvm-minimal/minimal-kvm.c
```

构建文件：

```text
tests/kvm-minimal/Makefile
```

核心调用顺序：

| 阶段 | demo 调用 | 含义 |
| --- | --- | --- |
| 打开 KVM | `open("/dev/kvm")` | 获取 KVM 系统 fd |
| 检查 API | `KVM_GET_API_VERSION` | 确认 KVM UAPI 版本 |
| 创建 VM | `KVM_CREATE_VM` | 创建 VM 容器，返回 VM fd |
| 分配 guest RAM | `mmap(... MAP_ANONYMOUS ...)` | 在 userspace 分配内存 |
| 注册 guest RAM | `KVM_SET_USER_MEMORY_REGION` | 建立 guest physical address 到 host userspace address 的映射 |
| 创建 vCPU | `KVM_CREATE_VCPU` | 创建一个虚拟 CPU |
| mmap run area | `KVM_GET_VCPU_MMAP_SIZE` + `mmap(vcpu_fd)` | 映射 `struct kvm_run` 共享区 |
| 设置段寄存器 | `KVM_GET_SREGS` / `KVM_SET_SREGS` | 设置 real-mode 段状态 |
| 设置通用寄存器 | `KVM_SET_REGS` | 设置 `rip/rsp/rflags` |
| 运行 guest | `KVM_RUN` | 请求 KVM 进入 guest 执行 |
| 处理 VM-exit | `KVM_EXIT_IO` / `KVM_EXIT_HLT` | userspace 处理退出原因 |

### 3.1 Guest RAM 与 guest code 放置位置

本 demo 的 `guest_code` 不是 ELF、bzImage 或 Linux kernel image，而是一段可以直接被 x86 CPU 执行的裸机器码：

```text
b0 xx    mov al, xx
e6 e9    out 0xe9, al
f4       hlt
```

也就是说，demo 故意绕过 BIOS、bootloader、ELF loader 和 Linux 启动协议，只做最小动作：把机器码字节拷贝进 guest RAM，然后把 vCPU 的 `rip` 指向这段代码。

`GUEST_CODE_ADDR` 表示这段裸机器码在 guest 物理内存中的放置地址：

```c
memcpy((uint8_t *)mem + GUEST_CODE_ADDR, guest_code, sizeof(guest_code));
regs.rip = GUEST_CODE_ADDR;
```

前面的 `KVM_SET_USER_MEMORY_REGION` 建立了映射：

```text
guest physical 0x0000 -> host userspace mem + 0x0000
guest physical 0x1000 -> host userspace mem + 0x1000
guest physical 0x2000 -> host userspace mem + 0x2000
```

所以 `mem + GUEST_CODE_ADDR` 是在 host userspace 分配的那块 RAM 里模拟 guest physical address `0x1000`。`rip = GUEST_CODE_ADDR` 则告诉 vCPU 从同一个 guest 地址开始取指。

理论上这段代码也可以放在 `0x0`，但放在 `0x1000` 更接近真实 x86 低地址布局：低地址附近通常会有 IVT、BIOS data area 等传统区域；同时也给后续扩展 stack、boot data 或其它 guest 数据留出空间。本 demo 当前把 `rsp/rbp` 设置在 `0x2000`，形成一个简单布局：

```text
0x0000  低地址保留区域
0x1000  guest code
0x2000  guest stack 起点
```

### 3.2 `KVM_GET_VCPU_MMAP_SIZE` 与 `struct kvm_run`

`KVM_GET_VCPU_MMAP_SIZE` 定义在 Linux KVM UAPI 头文件中，通常来自：

```text
/usr/include/linux/kvm.h
```

它的作用是向 KVM 查询每个 vCPU 的 `struct kvm_run` 共享区需要 mmap 多大：

```c
mmap_size = ioctl(kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
run = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu_fd, 0);
```

注意这里有两个 fd：

```text
KVM_GET_VCPU_MMAP_SIZE 用 kvm_fd   ：查询当前 KVM 实现要求的 run area 大小
mmap() 使用 vcpu_fd               ：映射某一个具体 vCPU 的 run area
```

不能直接使用 `sizeof(struct kvm_run)`，因为这个 mmap 区域大小属于 KVM ABI，可能随内核版本、架构和 KVM 扩展变化。正确做法是由内核返回大小，userspace 按这个大小 mmap。

`struct kvm_run` 是 KVM 内核和 userspace VMM 之间的共享通信区。每次：

```c
ioctl(vcpu_fd, KVM_RUN, 0);
```

guest 退出后，KVM 会把退出原因和退出相关数据写入 `run`：

```text
run->exit_reason
run->io.direction
run->io.port
run->io.size
run->io.count
run->io.data_offset
```

例如 guest 执行 `out 0xe9, al` 后，KVM 返回 userspace，并设置 `run->exit_reason = KVM_EXIT_IO`。userspace 再通过：

```c
data = (uint8_t *)run + run->io.data_offset;
```

读取 I/O 数据。这里的 `data_offset` 是 `struct kvm_run` mmap 共享区内的偏移，不是 guest 地址，也不是 host 绝对地址。

这种设计避免了每次 VM-exit 都通过 ioctl 参数复制大量数据，vCPU run loop 只需要一次 mmap，后续反复复用这块共享区。

## 4. Guest 指令与 VM-exit

### 4.1 当前 guest 机器码快照

当前 demo 的 guest 仍是 raw x86 machine code，不经过 ELF、bzImage、BIOS 或 bootloader。为了便于回溯，每次实验性修改 guest 指令流时，都应在这里记录关键源码快照；完整源码历史则建议在阶段稳定后用 git commit 记录。

当前 `tests/kvm-minimal/minimal-kvm.c` 中的关键指令流是：

```c
static const uint8_t guest_code[] = {
	0x31, 0xc0,			/* xor eax, eax */
	0x0f, 0xa2,			/* cpuid */
	0x88, 0xd8, 0xe6, DEBUG_IO_PORT,	/* mov al, bl; out 0xe9, al */
	0x88, 0xf8, 0xe6, DEBUG_IO_PORT,	/* mov al, bh; out 0xe9, al */
	0x66, 0xc1, 0xeb, 0x10,		/* shr ebx, 16 */
	0x88, 0xd8, 0xe6, DEBUG_IO_PORT,
	0x88, 0xf8, 0xe6, DEBUG_IO_PORT,
	0x88, 0xd0, 0xe6, DEBUG_IO_PORT,	/* mov al, dl; out 0xe9, al */
	0x88, 0xf0, 0xe6, DEBUG_IO_PORT,	/* mov al, dh; out 0xe9, al */
	0x66, 0xc1, 0xea, 0x10,		/* shr edx, 16 */
	0x88, 0xd0, 0xe6, DEBUG_IO_PORT,
	0x88, 0xf0, 0xe6, DEBUG_IO_PORT,
	0x88, 0xc8, 0xe6, DEBUG_IO_PORT,	/* mov al, cl; out 0xe9, al */
	0x88, 0xe8, 0xe6, DEBUG_IO_PORT,	/* mov al, ch; out 0xe9, al */
	0x66, 0xc1, 0xe9, 0x10,		/* shr ecx, 16 */
	0x88, 0xc8, 0xe6, DEBUG_IO_PORT,
	0x88, 0xe8, 0xe6, DEBUG_IO_PORT,
	0xb0, '\n', 0xe6, DEBUG_IO_PORT,
	0xc7, 0x06, 0x00, 0x50, 0x78, 0x56,	/* mov word [0x5000], 0x5678 */
	0xa1, 0x00, 0x50,			/* mov ax, [0x5000] */
	0xe6, DEBUG_IO_PORT,			/* out 0xe9, al */
	0x88, 0xe0, 0xe6, DEBUG_IO_PORT,	/* mov al, ah; out 0xe9, al */
	0xb0, '\n', 0xe6, DEBUG_IO_PORT,
	0xcc,					/* int3 */
	0xfb,					/* sti */
	0x90,					/* nop */
	0xf4,					/* hlt */
};

static const uint8_t irq_handler[] = {
	0xb0, 'I', 0xe6, DEBUG_IO_PORT,	/* mov al, 'I'; out 0xe9, al */
	0xb0, '\n', 0xe6, DEBUG_IO_PORT,
	0xcf,					/* iret */
};

static const uint8_t bp_handler[] = {
	0xb0, 'B', 0xe6, DEBUG_IO_PORT,	/* mov al, 'B'; out 0xe9, al */
	0xb0, '\n', 0xe6, DEBUG_IO_PORT,
	0xcf,					/* iret */
};
```

对应指令模式：

```text
31 c0             xor eax, eax
0f a2             cpuid
88 d8 / 88 f8     mov al, bl / mov al, bh
e6 e9             out 0xe9, al
66 c1 eb 10       shr ebx, 16
88 d0 / 88 f0     mov al, dl / mov al, dh
66 c1 ea 10       shr edx, 16
88 c8 / 88 e8     mov al, cl / mov al, ch
66 c1 e9 10       shr ecx, 16
c7 06 00 50 78 56 mov word [0x5000], 0x5678
a1 00 50          mov ax, [0x5000]
e6 e9             out 0xe9, al
88 e0             mov al, ah
cc                int3
fb                sti
90                nop
f4                hlt
b0 49 e6 e9 b0 0a e6 e9 cf    irq handler: output 'I\n'; iret
b0 42 e6 e9 b0 0a e6 e9 cf    #BP handler: output 'B\n'; iret
```

`cpuid` 会把 leaf 0 的 vendor string 放在 `EBX/EDX/ECX`。当前 guest 先输出每个寄存器的低 16 位，再用 `shr r32, 16` 输出高 16 位，于是可以逐字节打印 12 字节 vendor 字符串。

最后的 `mov word [0x5000], 0x5678` 用来验证 MMIO write exit；`mov ax, [0x5000]` 用来验证 MMIO read exit。当前 memory slot 只注册 `[0x0, 0x4000)`，因此 `0x5000` 不是 guest RAM；guest 访问这个 GPA 时，KVM 会把它作为 `KVM_EXIT_MMIO` 返回给 userspace。read exit 中 userspace 把 `run->mmio.data[]` 填成 `O`、`K`，KVM 重新进入 guest 后，guest 的 `AX` 得到这个返回值，再通过 `out 0xe9, al` 打印出来。

MMIO read/write 验证完成后，`int3` 用来验证 guest 内部异常路径。CPU 产生 #BP/vector 3 后查 real-mode IVT[3]，跳到 `0x1300` 的 handler，输出 `B` 并 `iret` 返回。随后 `sti; nop; hlt` 用来验证中断注入窗口。userspace 先设置 `run->request_interrupt_window = 1`，guest 执行 `sti` 打开 IF 后再执行一条 `nop` 度过 interrupt shadow；当 KVM 发现 guest 已经可以接收外部中断时，返回 `KVM_EXIT_IRQ_WINDOW_OPEN`。userspace 随后调用 `KVM_INTERRUPT` 注入 vector `0x20`，guest 查 IVT[0x20] 后跳到 `0x1200` 的 handler，输出 `I` 并执行 `iret`，最后回到 `hlt` 结束。

### 4.2 Guest 指令与 VM-exit

Demo 使用内嵌 x86 real-mode 机器码：

```asm
mov al, imm8
out 0xe9, al
...
hlt
```

对应字节模式：

```text
b0 xx    mov al, xx
e6 e9    out 0xe9, al
f4       hlt
```

选择 `0xe9` 是为了避免实现完整 8250 串口；它常用作 debug console port，适合最小化演示。

Guest 输出字符串：

```text
Hello, KVM!
```

每个字符执行一次 `out 0xe9, al`，因此每个字符触发一次 `KVM_EXIT_IO`。最后 `hlt` 触发 `KVM_EXIT_HLT`。

这里的输出不是一次性完成的，而是每个字符都经历一次 VM-exit：

```text
KVM_RUN
  -> guest 执行 mov al, 'H'
  -> guest 执行 out 0xe9, al
  -> KVM_EXIT_IO
  -> userspace 打印 H

KVM_RUN
  -> guest 从上一条 out 后继续执行
  -> guest 执行 mov al, 'e'
  -> guest 执行 out 0xe9, al
  -> KVM_EXIT_IO
  -> userspace 打印 e

...

KVM_RUN
  -> guest 执行最后一个 out 0xe9, al
  -> KVM_EXIT_IO
  -> userspace 打印 '\n'

KVM_RUN
  -> guest 继续执行下一条指令
  -> guest 执行 hlt
  -> KVM_EXIT_HLT
```

所以输出中看到：

```text
guest output: Hello, KVM!
KVM_EXIT_HLT
```

不是 userspace 主动在输出完成后触发 HLT，而是 guest 机器码最后本来就放了 `0xf4`。当最后一个 `out` 被处理完、userspace 再次调用 `KVM_RUN` 后，vCPU 继续向后取指，下一条刚好是 `hlt`，于是 KVM 把它暴露成 `KVM_EXIT_HLT`。

`KVM_EXIT_IO` 数据读取方式：

```c
uint8_t *data = (uint8_t *)run + run->io.data_offset;
```

`data_offset` 是 `struct kvm_run` 共享区内的偏移，不是绝对地址。

## 5. 实测记录

### 5.1 环境

基础检查结果：

```text
uname -m:
x86_64

/dev/kvm:
crw-rw----+ 1 root kvm 10, 232 /dev/kvm

KVM modules:
kvm_amd
kvm
irqbypass
ccp
```

CPU flags 中存在：

```text
svm npt vgif avic x2avic
```

说明当前机器是 AMD SVM/NPT 环境。虽然 PDF 第一章主要使用 Intel VMX 术语，本文实验仍然验证同一类硬件辅助 CPU 虚拟化模型。

### 5.2 构建

命令：

```bash
make -C tests/kvm-minimal
```

结果：

```text
cc -Wall -Wextra -O2 -g -std=gnu11 -o minimal-kvm minimal-kvm.c
```

### 5.3 初始 Hello/KVM 版本运行

命令：

```bash
tests/kvm-minimal/minimal-kvm
```

输出：

```text
KVM API version: 12
guest output: Hello, KVM!
KVM_EXIT_HLT
io exits: 12
```

结论：

```text
12 个 guest 字符 -> 12 次 KVM_EXIT_IO
最终 hlt 指令 -> 1 次 KVM_EXIT_HLT
```

这说明 guest 确实通过 `KVM_RUN` 进入了虚拟 CPU 执行，并在 I/O 指令和 HLT 指令处退出回 userspace。

### 5.4 CPUID 实验验证

为了验证 guest 看到的 CPUID 结果确实来自 KVM 的虚拟 CPUID 模型，而不是简单透传物理 CPU，本轮在 userspace 中增加：

```c
ret = kvm_ioctl(kvm_fd, KVM_GET_SUPPORTED_CPUID, cpuid,
		"KVM_GET_SUPPORTED_CPUID");

entry->ebx = pack4('K', 'V', 'M', 'D');
entry->edx = pack4('E', 'M', 'O', '1');
entry->ecx = pack4('2', '3', '4', '5');

ret = kvm_ioctl(vcpu_fd, KVM_SET_CPUID2, cpuid, "KVM_SET_CPUID2");
```

CPUID leaf 0 的 vendor string 按 `EBX/EDX/ECX` 返回。这里故意把 guest 可见 vendor 改成：

```text
KVMDEMO12345
```

guest 端执行：

```asm
xor eax, eax
cpuid
```

然后把 `EBX/EDX/ECX` 中的 12 个字节逐个 `out 0xe9, al` 输出。

运行结果：

```text
KVM API version: 12
guest output: KVMDEMO12345
KVM_EXIT_HLT
io exits: 13
```

`io exits` 是 13，因为 vendor string 有 12 个字符，最后还额外输出了一个换行符。这个结果证明：

```text
userspace VMM 设置 vCPU 的虚拟 CPUID 表
    -> guest 执行 cpuid
    -> KVM 内核按虚拟 CPUID 表填回 EBX/EDX/ECX
    -> guest 通过 out 0xe9 把返回值打印出来
```

用 strace 还能看到 host 侧确实设置了 CPUID 表：

```text
ioctl(3, KVM_GET_SUPPORTED_CPUID, {nent=65, entries=[...]}) = 0
ioctl(5, KVM_SET_CPUID2, {nent=65, entries=[...]}) = 0
guest output: ioctl(5, KVM_RUN, 0) = 0
Kioctl(5, KVM_RUN, 0) = 0
...
5ioctl(5, KVM_RUN, 0) = 0
ioctl(5, KVM_RUN, 0) = 0
KVM_EXIT_HLT
io exits: 13
```

因此，`cpuid` 的语义可以更准确地理解为：guest 执行 CPUID 时可能发生硬件 VM-exit 到 KVM；KVM 不一定把这次退出交给 userspace，而是根据 vCPU 的 CPUID 配置直接模拟返回值，然后继续 guest。

### 5.5 MMIO exit 实验验证

为了验证 memory slot 外的 GPA 访问会返回 userspace，本轮把 guest RAM 缩小为：

```c
#define GUEST_MEM_SIZE 0x4000
#define MMIO_TEST_ADDR 0x5000
```

也就是只注册：

```text
GPA [0x0000, 0x4000) -> host userspace memory
```

而 guest 先执行 MMIO write：

```asm
mov word [0x5000], 0x5678
```

`0x5000` 不属于任何 KVM memory slot，因此 KVM 不把它当普通 RAM，而是作为 MMIO 访问返回 userspace：

```text
KVM_EXIT_MMIO: addr=0x5000 len=2 is_write=1 data=0x7856
```

随后 guest 执行 MMIO read，并把 userspace 返回的数据打印出来：

```asm
mov ax, [0x5000]
out 0xe9, al
mov al, ah
out 0xe9, al
```

userspace 在 read exit 中填返回值：

```c
if (!run->mmio.is_write) {
	run->mmio.data[0] = 'O';
	run->mmio.data[1] = 'K';
}
```

完整运行结果：

```text
KVM API version: 12
guest output: KVMDEMO12345
KVM_EXIT_MMIO: addr=0x5000 len=2 is_write=1 data=0x7856
KVM_EXIT_MMIO: addr=0x5000 len=2 is_write=0 data=0x4f4b
OK
KVM_EXIT_HLT
io exits: 16
mmio exits: 2
```

`data=0x7856` 是按 `run->mmio.data[]` 原始字节顺序打印的结果。guest 写入的值是 `0x5678`，x86 小端序在内存里保存为：

```text
低地址: 0x78
高地址: 0x56
```

所以 userspace 看到的 MMIO write data byte stream 是 `78 56`。

`data=0x4f4b` 是 userspace 填给 guest 的 MMIO read 返回值：

```text
0x4f -> 'O'
0x4b -> 'K'
```

KVM 重新进入 guest 后，未完成的 `mov ax, [0x5000]` 得到这个返回值，guest 再把 `AL/AH` 通过 PIO 打印出来。

这个实验说明：

```text
memory slot 内 GPA     -> guest RAM，通常不返回 userspace
memory slot 外 GPA     -> KVM_EXIT_MMIO，交给 userspace 模拟设备
```

这证明 MMIO read/write 的完整闭环已经打通：

```text
guest write device register
    -> KVM_EXIT_MMIO write
    -> userspace 观察并模拟写寄存器副作用

guest read device register
    -> KVM_EXIT_MMIO read
    -> userspace 填 run->mmio.data
    -> guest 获得返回值并继续执行
```

这就是后续 PCI BAR、virtio-mmio、设备寄存器模拟的基础入口。

### 5.6 strace 验证

命令：

```bash
strace -e openat,ioctl,mmap,munmap,close tests/kvm-minimal/minimal-kvm
```

关键结果：

```text
openat(AT_FDCWD, "/dev/kvm", O_RDWR|O_CLOEXEC) = 3
ioctl(3, KVM_GET_API_VERSION, 0)        = 12
ioctl(3, KVM_CREATE_VM, 0)              = 4
mmap(NULL, 65536, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) = ...
ioctl(4, KVM_SET_USER_MEMORY_REGION, {slot=0, flags=0, guest_phys_addr=0, memory_size=65536, userspace_addr=...}) = 0
ioctl(4, KVM_CREATE_VCPU, 0)            = 5
ioctl(3, KVM_GET_VCPU_MMAP_SIZE, 0)     = 12288
mmap(NULL, 12288, PROT_READ|PROT_WRITE, MAP_SHARED, 5, 0) = ...
ioctl(5, KVM_GET_SREGS, ...)            = 0
ioctl(5, KVM_SET_SREGS, ...)            = 0
ioctl(5, KVM_SET_REGS, {rip=0x1000, rsp=0x2000, rbp=0x2000, rflags=0x2}) = 0
ioctl(5, KVM_RUN, 0)                    = 0
...
ioctl(5, KVM_RUN, 0)                    = 0
munmap(...)
close(5)
close(4)
close(3)
```

观察到多次 `KVM_RUN`：每次 guest 执行 `out 0xe9, al` 后返回 userspace，userspace 打印一个字符后再次调用 `KVM_RUN`。

### 5.7 trace-cmd 计划

当前系统已安装 `trace-cmd`：

```text
/usr/bin/trace-cmd
```

但 KVM tracepoint 抓取需要 root 权限。本轮在非交互环境中执行：

```bash
sudo trace-cmd record -o /tmp/minimal-kvm-trace.dat \
  -e kvm:kvm_entry \
  -e kvm:kvm_exit \
  -e kvm:kvm_userspace_exit \
  -- $(pwd)/tests/kvm-minimal/minimal-kvm
```

结果因为 sudo 需要密码而未执行：

```text
sudo: a terminal is required to read the password
```

后续可以在交互终端手动执行：

```bash
sudo trace-cmd record -o /tmp/minimal-kvm-trace.dat \
  -e kvm:kvm_entry \
  -e kvm:kvm_exit \
  -e kvm:kvm_userspace_exit \
  -- ./tests/kvm-minimal/minimal-kvm

sudo trace-cmd report -i /tmp/minimal-kvm-trace.dat
```

预期观察点：

```text
kvm_entry            KVM 准备进入 guest，对应 VM-entry 的内核观测点
kvm_exit             guest 退出到 KVM，对应 VM-exit 的内核观测点
kvm_userspace_exit   KVM 把 exit 暴露给 userspace VMM
```

## 6. 与 kvmtool 正式源码对应关系

| 第一章/demo 概念 | kvmtool 对应位置 |
| --- | --- |
| 打开 `/dev/kvm` | `kvm.c:436` 的 `kvm__init()` |
| `KVM_GET_API_VERSION` | `kvm.c:436` 的 `kvm__init()` |
| `KVM_CREATE_VM` | `kvm.c:436` 的 `kvm__init()` |
| `KVM_SET_USER_MEMORY_REGION` | `kvm.c:237` 的 `kvm__register_mem()` |
| `KVM_CREATE_VCPU` | `x86/kvm-cpu.c:92` 的 `kvm_cpu__arch_init()` |
| `KVM_GET_VCPU_MMAP_SIZE` + mmap `struct kvm_run` | `x86/kvm-cpu.c:92` 的 `kvm_cpu__arch_init()` |
| 设置 guest 初始 CPU 状态 | `x86/kvm-cpu.c:237` 的 `kvm_cpu__reset_vcpu()`、`x86/kvm-cpu.c:212` 的 `kvm_cpu__setup_sregs()`、`x86/kvm-cpu.c:194` 的 `kvm_cpu__setup_regs()` |
| `KVM_RUN` | `kvm-cpu.c:37` 的 `kvm_cpu__run()` |
| `KVM_EXIT_IO` 分发 | `kvm-cpu.c:145` 的 `kvm_cpu__start()` |
| vCPU pthread | `builtin-run.c:840` 的 `kvm_cmd_run_work()`、`builtin-run.c:289` 的 `kvm_cpu_thread()` |

最小 demo 和 kvmtool 的关系：

```text
minimal-kvm.c:
    手工创建 VM/vCPU/guest RAM
    手工设置 real-mode RIP
    手工处理 KVM_EXIT_IO / HLT

kvmtool:
    自动加载 bzImage/rootfs
    初始化 BIOS/boot params
    创建 PIC/IOAPIC/LAPIC/PIT
    注册 PIO/MMIO/PCI/virtio 设备
    每个 vCPU 一个 pthread
    在 kvm_cpu__start() 中分发各种 KVM_EXIT_*
```

但二者最核心的 CPU 虚拟化边界相同：

```text
ioctl(KVM_RUN)
```

## 7. 中断注入窗口实验代码差异与实测

本节记录中断注入窗口实验的实际代码差异和运行结果，方便后续回溯。

目标是验证：userspace 通过 `run->request_interrupt_window` 请求 KVM 在 guest 可接收中断时退出；KVM 返回 `KVM_EXIT_IRQ_WINDOW_OPEN` 后，userspace 再通过 `KVM_INTERRUPT` 注入 vector `0x20`。guest CPU 查 real-mode IVT，跳到 guest handler 执行，handler 通过 `out 0xe9, al` 打印字符后 `iret` 返回。

### 7.1 新增宏

```c
#define IRQ_VECTOR	0x20
#define IRQ_HANDLER_ADDR 0x1200
```

`IRQ_VECTOR` 是注入给 guest 的中断向量号。`IRQ_HANDLER_ADDR` 是 guest real-mode handler 放置位置，仍在当前 RAM slot 内。

### 7.2 新增 guest interrupt handler

handler 机器码可以单独放一个数组：

```c
static const uint8_t irq_handler[] = {
	0xb0, 'I', 0xe6, DEBUG_IO_PORT,	/* mov al, 'I'; out 0xe9, al */
	0xcf,					/* iret */
};
```

含义：

```asm
mov al, 'I'
out 0xe9, al
iret
```

`iret` 用于从中断处理函数返回，恢复 CPU 自动压栈保存的 `FLAGS/CS/IP`。

### 7.3 初始化 real-mode IVT

real mode 下的中断入口表是 IVT，固定在 GPA `0x0000` 开始。每个 vector 占 4 字节：

```text
offset:  2 bytes
segment: 2 bytes
```

vector `0x20` 的 IVT 表项地址：

```text
0x20 * 4 = 0x80
```

计划在 `setup_guest_memory()` 里写：

```c
uint8_t *ivt_entry = (uint8_t *)mem + IRQ_VECTOR * 4;

ivt_entry[0] = IRQ_HANDLER_ADDR & 0xff;
ivt_entry[1] = IRQ_HANDLER_ADDR >> 8;
ivt_entry[2] = 0;
ivt_entry[3] = 0;
memcpy((uint8_t *)mem + IRQ_HANDLER_ADDR, irq_handler, sizeof(irq_handler));
```

这样 guest 收到 vector `0x20` 后会跳到：

```text
CS:IP = 0000:1200
```

### 7.4 新增 `KVM_INTERRUPT`

userspace 注入中断需要：

```c
static int inject_interrupt(int vcpu_fd)
{
	struct kvm_interrupt irq = {
		.irq = IRQ_VECTOR,
	};

	return kvm_ioctl(vcpu_fd, KVM_INTERRUPT, &irq, "KVM_INTERRUPT");
}
```

这一步只告诉 KVM：“给 vCPU 注入 vector 0x20”。真正跳到哪里执行由 guest 自己的 IVT 决定。

### 7.5 修改 run loop

中断窗口实验不再等到 `HLT` 后注入，而是在进入 run loop 前请求 KVM 监控 interrupt window：

```c
run->request_interrupt_window = 1;
```

当 guest 执行 `sti; nop` 后，IF 已经打开且不再处于 `sti` 的 interrupt shadow 中，KVM 返回 `KVM_EXIT_IRQ_WINDOW_OPEN`：

```c
case KVM_EXIT_IRQ_WINDOW_OPEN:
	printf("KVM_EXIT_IRQ_WINDOW_OPEN: inject IRQ 0x%x\n",
	       IRQ_VECTOR);
	run->request_interrupt_window = 0;
	if (inject_interrupt(vcpu_fd) < 0)
		exit(EXIT_FAILURE);
	irq_injected = 1;
	break;
case KVM_EXIT_HLT:
	if (!irq_injected) {
		fprintf(stderr, "KVM_EXIT_HLT before IRQ window\n");
		exit(EXIT_FAILURE);
	}
	printf("KVM_EXIT_HLT\n");
	return;
```

预期路径：

```text
userspace 设置 request_interrupt_window
    -> KVM_RUN
    -> guest 执行 sti
    -> guest 执行 nop，度过 sti interrupt shadow
    -> KVM_EXIT_IRQ_WINDOW_OPEN
    -> userspace KVM_INTERRUPT(vector=0x20)
    -> KVM_RUN
    -> guest 查 IVT[0x20]
    -> guest 执行 0x1200 handler
    -> handler 输出 'I'
    -> iret 返回到 hlt
    -> KVM_EXIT_HLT
```

### 7.6 实测输出

在现有 CPUID/MMIO 输出之后，额外看到 handler 输出的 `I`：

```text
KVM API version: 12
guest output: KVMDEMO12345
KVM_EXIT_MMIO: addr=0x5000 len=2 is_write=1 data=0x7856
KVM_EXIT_MMIO: addr=0x5000 len=2 is_write=0 data=0x4f4b
OK
KVM_EXIT_IRQ_WINDOW_OPEN: inject IRQ 0x20
I
KVM_EXIT_HLT
io exits: 18
mmio exits: 2
```

当前 handler 会在输出 `I` 后额外输出换行，因此这个阶段的 `io exits` 是 18，便于和后面的 `KVM_EXIT_HLT` 区分。

strace 能看到 `KVM_INTERRUPT` ioctl 成功：

```text
ioctl(5, KVM_INTERRUPT, ...) = 0
```

路径已经验证：

```text
request_interrupt_window = 1
    -> guest 执行 sti; nop
    -> KVM_EXIT_IRQ_WINDOW_OPEN
    -> userspace KVM_INTERRUPT(vector=0x20)
    -> KVM_RUN
    -> guest 查 IVT[0x20]
    -> guest 跳到 0000:1200
    -> handler 输出 'I'
    -> iret 返回到 hlt
    -> KVM_EXIT_HLT
```

### 7.7 `request_interrupt_window` 与真正注入的时序

这里容易误解成：先把 `run->request_interrupt_window` 设为 1，就等于先给 guest 放了一个 pending interrupt，等 guest 执行 `sti; nop` 后再处理。这个理解更接近硬件内部“pending interrupt 等待 IF 打开”的模型，但不是当前 demo 使用的 KVM UAPI 流程。

当前 demo 的真实时序是：

```text
userspace: run->request_interrupt_window = 1
    -> 这不是打开 guest IF，也不是注入中断
    -> 只是请求 KVM 监控 guest 什么时候可以接收 maskable interrupt

userspace: ioctl(KVM_RUN)
    -> guest 初始 RFLAGS.IF = 0
    -> KVM 发现还不能注入，继续运行 guest

guest: sti
    -> guest 自己把 RFLAGS.IF 置 1
    -> 但 x86 规定 sti 后下一条指令仍处于 interrupt shadow
    -> 此时仍不是安全注入点

guest: nop
    -> 度过 sti interrupt shadow
    -> guest 进入真正可接收外部 maskable interrupt 的窗口

KVM: KVM_EXIT_IRQ_WINDOW_OPEN
    -> 因为 userspace 之前设置过 request_interrupt_window = 1
    -> KVM 通知 userspace：现在可以安全注入

userspace: KVM_INTERRUPT(vector=0x20)
    -> 这是当前 demo 第一次真正提交中断
    -> 不是重复注入

userspace: ioctl(KVM_RUN)
    -> KVM 把 vector 0x20 注入 guest
    -> guest 查 IVT[0x20] 并执行 handler
```

所以三者的分工是：

```text
request_interrupt_window = 1  预约通知：窗口打开时请退出到 userspace
KVM_EXIT_IRQ_WINDOW_OPEN      KVM 通知：guest 现在可以接收中断
KVM_INTERRUPT                 真正提交：把 vector 注入给 vCPU
```

如果改成“先 `KVM_INTERRUPT`，再等待 guest 执行 `sti; nop`”，那验证的是另一件事：KVM 中已有 pending interrupt 时，guest 进入可中断状态后是否会被投递。当前实验刻意采用 `request_interrupt_window -> IRQ_WINDOW_OPEN -> KVM_INTERRUPT`，是为了观察 userspace VMM 如何找到安全注入时机。

如果没有看到 `I`，优先检查：

```text
IVT[0x20] 是否写到 0x80
handler 是否拷贝到 0x1200
KVM_INTERRUPT 是否返回 0
real-mode IF/interrupt window 是否允许注入
handler 是否用 iret 而不是 ret
```

## 8. `int3` / #BP 异常实验代码差异与实测

中断注入实验验证的是 host/userspace 主动向 guest 注入外部中断。`int3` 实验验证的是另一条路径：guest 自己执行异常指令，由 CPU 产生 #BP exception，随后按 real-mode IVT 跳到 guest handler。

新增 vector 和 handler 地址：

```c
#define BP_INT_VECTOR	0x03
#define BP_HANDLER_ADDR 0x1300
```

新增 #BP handler：

```c
static const uint8_t bp_handler[] = {
	0xb0, 'B', 0xe6, DEBUG_IO_PORT,	/* mov al, 'B'; out 0xe9, al */
	0xb0, '\n', 0xe6, DEBUG_IO_PORT,
	0xcf,					/* iret */
};
```

在 guest memory 初始化时，把 handler 拷贝到 `0x1300`，并设置 IVT[3]：

```c
memcpy((uint8_t *)mem + BP_HANDLER_ADDR, bp_handler, sizeof(bp_handler));

ivt_entry = (uint8_t *)mem + BP_INT_VECTOR * 4;
ivt_entry[0] = BP_HANDLER_ADDR & 0xff;
ivt_entry[1] = BP_HANDLER_ADDR >> 8;
ivt_entry[2] = 0;
ivt_entry[3] = 0;
```

在 guest code 中插入：

```text
cc                int3
```

预期路径：

```text
guest 执行 int3
    -> CPU 产生 #BP exception，vector 3
    -> CPU/KVM 查 real-mode IVT[3]
    -> guest 跳到 0000:1300
    -> handler 输出 'B'
    -> iret 返回 int3 后一条指令
    -> guest 继续执行 sti; nop; hlt
```

实测输出：

```text
KVM API version: 12
guest output: KVMDEMO12345
KVM_EXIT_MMIO: addr=0x5000 len=2 is_write=1 data=0x7856
KVM_EXIT_MMIO: addr=0x5000 len=2 is_write=0 data=0x4f4b
OK
B
KVM_EXIT_IRQ_WINDOW_OPEN: inject IRQ 0x20
I
KVM_EXIT_HLT
io exits: 20
mmio exits: 2
```

`B` 说明 #BP handler 已经执行。当前 #BP handler 和 IRQ handler 都会额外输出换行，所以 `io exits` 从中断窗口实验早期版本的 17 增加到 20：#BP handler 多两次 PIO 输出 `B\n`，IRQ handler 多一次换行输出。

为了让 real-mode IVT 路径更明确，当前 `setup_real_mode()` 也显式设置了 IDT 基址和 limit：

```c
sregs.idt.base = 0;
sregs.idt.limit = 0xffff;
```

## 9. `ud2` / #UD fault 异常实验代码差异与实测

`int3`/#BP 和 `ud2`/#UD 都是 guest 内部异常，但返回语义不同：`int3` 是 trap，CPU 保存的返回 IP 指向 `int3` 后一条指令；`ud2` 触发的 #UD 是 fault，CPU 保存的返回 IP 仍指向 faulting `ud2` 本身。如果 #UD handler 直接 `iret`，guest 会再次执行同一条 `ud2`，于是无限进入 #UD handler。

新增 vector 和 handler 地址：

```c
#define UD_INT_VECTOR	0x06
#define UD_HANDLER_ADDR 0x1400
```

在 guest code 中，`int3` 后插入两字节非法指令：

```text
0f 0b             ud2
```

#UD handler 需要先修改栈上保存的返回 IP，让 `iret` 返回到 `ud2` 后面的 `sti`：

```c
static const uint8_t ud_handler[] = {
	0x55,					/* push bp */
	0x89, 0xe5,				/* mov bp, sp */
	0x83, 0x46, 0x02, 0x02,		/* add word [bp + 2], 2 */
	0x5d,					/* pop bp */
	0xb0, 'U', 0xe6, DEBUG_IO_PORT,	/* mov al, 'U'; out 0xe9, al */
	0xb0, '\n', 0xe6, DEBUG_IO_PORT,
	0xcf,					/* iret */
};
```

这里不能直接编码 `add word [sp], 2`，因为 16-bit addressing mode 没有 `[sp]` 这种寻址形式。handler 使用 `bp` 建立一个临时栈帧：`push bp` 后，原异常返回 IP 位于 `[bp + 2]`；把它加 2 后，`pop bp; iret` 会跳过两字节 `ud2`。

在 guest memory 初始化时，把 handler 拷贝到 `0x1400`，并设置 IVT[6]：

```c
memcpy((uint8_t *)mem + UD_HANDLER_ADDR, ud_handler, sizeof(ud_handler));

ivt_entry = (uint8_t *)mem + UD_INT_VECTOR * 4;
ivt_entry[0] = UD_HANDLER_ADDR & 0xff;
ivt_entry[1] = UD_HANDLER_ADDR >> 8;
ivt_entry[2] = 0;
ivt_entry[3] = 0;
```

实测输出：

```text
KVM API version: 12
guest output: KVMDEMO12345
KVM_EXIT_MMIO: addr=0x5000 len=2 is_write=1 data=0x7856
KVM_EXIT_MMIO: addr=0x5000 len=2 is_write=0 data=0x4f4b
OK
B
U
KVM_EXIT_IRQ_WINDOW_OPEN: inject IRQ 0x20
I
KVM_EXIT_HLT
io exits: 22
mmio exits: 2
```

`U` 只出现一次，说明 #UD handler 成功把返回 IP 从 `ud2` 本身推进到下一条指令；后续仍能进入 interrupt window、注入 IRQ，并最终 `hlt` 退出。

## 10. `KVM_GET_REGS` / `KVM_SET_REGS` vCPU 寄存器读写实验

本阶段验证 userspace VMM 不只能通过 `struct kvm_run` 看到 exit reason，还可以在 vCPU 停在 VM-exit 边界时读取和修改 guest 通用寄存器。

新增两个 helper：

```c
static void dump_regs(int vcpu_fd, const char *where)
{
	struct kvm_regs regs;

	if (kvm_ioctl(vcpu_fd, KVM_GET_REGS, &regs, "KVM_GET_REGS") < 0)
		exit(EXIT_FAILURE);

	printf("%s regs: rip=0x%llx rsp=0x%llx rflags=0x%llx "
	       "rax=0x%llx rbx=0x%llx rcx=0x%llx rdx=0x%llx\n",
	       where, (unsigned long long)regs.rip,
	       (unsigned long long)regs.rsp,
	       (unsigned long long)regs.rflags,
	       (unsigned long long)regs.rax,
	       (unsigned long long)regs.rbx,
	       (unsigned long long)regs.rcx,
	       (unsigned long long)regs.rdx);
}

static int set_al(int vcpu_fd, uint8_t value)
{
	struct kvm_regs regs;

	if (kvm_ioctl(vcpu_fd, KVM_GET_REGS, &regs, "KVM_GET_REGS") < 0)
		return -1;

	regs.rax = (regs.rax & ~0xffULL) | value;
	return kvm_ioctl(vcpu_fd, KVM_SET_REGS, &regs, "KVM_SET_REGS");
}
```

`dump_regs()` 用 `KVM_GET_REGS` 读取当前 vCPU 寄存器；`set_al()` 先读取完整 `struct kvm_regs`，只改 `RAX` 的低 8 位 `AL`，再用 `KVM_SET_REGS` 写回。这样不会误改 `RIP/RSP/RFLAGS`。

guest 指令流在 #UD 之后、`sti` 之前插入一个 marker：

```c
#define REGS_TEST_MARKER '?'
#define REGS_TEST_VALUE	'R'

0xb0, REGS_TEST_MARKER, 0xe6, DEBUG_IO_PORT,
0xe6, DEBUG_IO_PORT,
0xb0, '\n', 0xe6, DEBUG_IO_PORT,
```

对应汇编语义：

```asm
mov al, '?'
out 0xe9, al
out 0xe9, al
mov al, '\n'
out 0xe9, al
```

第一次 `out` 会输出 `?` 并触发 `KVM_EXIT_IO`。userspace 在这次 exit 中发现 `data[0] == '?'`，先打印修改前寄存器，再把 `AL` 改成 `'R'`：

```c
if (!*regs_updated && run->io.count == 1 &&
    data[0] == REGS_TEST_MARKER) {
	dump_regs(vcpu_fd, "KVM_EXIT_IO before KVM_SET_REGS");
	if (set_al(vcpu_fd, REGS_TEST_VALUE) < 0)
		exit(EXIT_FAILURE);
	dump_regs(vcpu_fd, "KVM_EXIT_IO after KVM_SET_REGS");
	*regs_updated = 1;
}
```

随后 userspace 再次 `KVM_RUN`，guest 从第一条 `out` 后继续执行第二条 `out`。由于 `AL` 已经被 userspace 改成 `'R'`，第二条 `out` 输出 `R`。这就证明 `KVM_SET_REGS` 写回的寄存器状态会影响后续 guest 执行。

本阶段还在 MMIO、IRQ window 和 HLT exit 处打印寄存器：

```c
dump_regs(vcpu_fd, "KVM_EXIT_MMIO");
dump_regs(vcpu_fd, "KVM_EXIT_IRQ_WINDOW_OPEN");
dump_regs(vcpu_fd, "KVM_EXIT_HLT");
```

实测输出：

```text
KVM API version: 12
guest output: KVMDEMO12345
KVM_EXIT_MMIO: addr=0x5000 len=2 is_write=1 data=0x7856
KVM_EXIT_MMIO regs: rip=0x104a rsp=0x2000 rflags=0x12 rax=0xa rbx=0x444d rcx=0x3534 rdx=0x314f
KVM_EXIT_MMIO: addr=0x5000 len=2 is_write=0 data=0x4f4b
KVM_EXIT_MMIO regs: rip=0x104a rsp=0x2000 rflags=0x12 rax=0xa rbx=0x444d rcx=0x3534 rdx=0x314f
OK
B
U
?KVM_EXIT_IO before KVM_SET_REGS regs: rip=0x105c rsp=0x2000 rflags=0x12 rax=0x4b3f rbx=0x444d rcx=0x3534 rdx=0x314f
KVM_EXIT_IO after KVM_SET_REGS regs: rip=0x105c rsp=0x2000 rflags=0x12 rax=0x4b52 rbx=0x444d rcx=0x3534 rdx=0x314f
R
KVM_EXIT_IRQ_WINDOW_OPEN regs: rip=0x1066 rsp=0x2000 rflags=0x212 rax=0x4b0a rbx=0x444d rcx=0x3534 rdx=0x314f
KVM_EXIT_IRQ_WINDOW_OPEN: inject IRQ 0x20
I
KVM_EXIT_HLT regs: rip=0x1067 rsp=0x2000 rflags=0x212 rax=0x4b0a rbx=0x444d rcx=0x3534 rdx=0x314f
KVM_EXIT_HLT
io exits: 25
mmio exits: 2
```

重点观察：

```text
before: rax=0x4b3f  -> AL = 0x3f = '?'
after : rax=0x4b52  -> AL = 0x52 = 'R'
```

因此本实验同时验证了：

```text
KVM_GET_REGS 可以在 VM-exit 后读取 guest vCPU 状态
KVM_SET_REGS 可以修改 guest vCPU 状态
修改后的寄存器会在下一次 KVM_RUN 后被 guest 继续使用
```

另外，`KVM_EXIT_IRQ_WINDOW_OPEN` 和 `KVM_EXIT_HLT` 的 `rflags=0x212` 中包含 `IF=1`，和前面的 `sti; nop` 实验相互印证：guest 已经打开 maskable interrupt，再进入可注入中断窗口。

## 11. 下一步验证方向

后续可以按以下顺序继续往下层探索：

1. 用 `trace-cmd` 抓 `kvm:kvm_entry`、`kvm:kvm_exit`、`kvm:kvm_userspace_exit`，把 VM-entry/VM-exit 对应到内核 tracepoint。
2. 对照 Linux KVM 源码阅读 `kvm_arch_vcpu_ioctl_run()`、`vcpu_enter_guest()`、`svm_vcpu_run()`、`svm_handle_exit()`。
3. 扩展 demo 增加 `cpuid` 指令，观察 CPUID 是否由 KVM 内核处理或返回 userspace。
4. 扩展 demo 增加 MMIO 访问，观察 `KVM_EXIT_MMIO`。
5. 再回到 kvmtool 的 `kvm_cpu__start()`，理解正式 VMM 如何处理更多 exit reason。

## 12. 当前结论

本轮实验已经验证了 CPU 虚拟化第一章的最小可观察闭环：

```text
userspace 创建 VM/vCPU
    -> KVM_RUN 进入 guest
    -> guest 在硬件虚拟化模式中执行真实 x86 指令
    -> I/O 指令触发 VM-exit
    -> KVM 把 KVM_EXIT_IO 交给 userspace
    -> userspace 模拟 I/O
    -> 再次 KVM_RUN
    -> guest hlt 触发 KVM_EXIT_HLT
```

这就是后续理解 kvmtool、QEMU、KVM 内核、vCPU 调度、设备虚拟化和中断/内存虚拟化的基础。
