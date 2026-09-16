# HVM 描述符表恢复与撤销回归

2026-09-15：修复三条返回路径遗漏的 GDTR/IDTR 恢复，并以驱动中的
`SGDT` / `SIDT` 回读确认基址和界限。原始记录见
[本轮数据目录](paper-data/20260915-descriptor-fix/README.md)。

## 已确认的缺陷

VM-exit 加载 host GDTR/IDTR 的基址，并把两个界限设为 `0xFFFF`。
`VMXOFF` 不负责恢复调用者的表寄存器。该行为见
[Intel SDM Vol. 3C](https://cdrdv2-public.intel.com/825750/326019-sdm-vol-3c.pdf)。
旧代码在以下三处直接返回，因而不能把命令退出码 0 当作完整的体系结构状态恢复。

| 路径 | 原来的遗漏 | 修复 |
| --- | --- | --- |
| 常驻撤销 | Windows 继续使用 VM-exit 后的表寄存器，IDTR 还指向 KSword 私有 host IDT | 在 VMCLEAR 前读取当前 guest GDTR/IDTR；VMXOFF、恢复 guest CR3 后执行 LGDT/LIDT |
| 一次性来宾 | 返回原 C 调用点时遗留 host 表界限 | VM entry 前保存原表；VMXOFF 后恢复 |
| 嵌套探针 | `RtlRestoreContext` 恢复通用上下文，但不恢复表寄存器 | 探针 VMX 序列之前保存原表；虚拟 VMXOFF 后、RtlRestoreContext 前恢复 |

统一实现位于 `hvm_descriptor.c` / `hvm_descriptor.h` 和 `hvm_entry.asm`。
它只装载表寄存器，不修改 GDT/IDT 表内容或段选择子。所有路径在继续返回之前，
分别比较 GDTR、IDTR 的基址和界限。捕获不完整或回读不符不会返回成功。

## 0xA 转储：事实与推断

最新实际停止码仍是 `0xA`，故障点为 `nt!RtlpxVirtualUnwind+0x419`
（`nt+0x2308a9`），访问 `0x00007FFFFFFF0000`。这不是最初的异常：
保留的 trap frame 指向匿名内核池中的 `mov cr0,rax`，RAX 为 `0x80040033`，
正在尝试清除 CR0.WP。异常分发随后在展开栈时再次失败。

匿名例程还包含准备调用 `KeBugCheckEx(0x109, ...)` 的路径；保存的参数中，
第 4 个参数为 3，相关地址为 Windows GDT 基址。Microsoft 将该值解释为
[processor GDT corruption](https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/bug-check-0x109---critical-structure-corruption)。
这支持“描述符状态破坏触发内核完整性检查”的推断；它不表示实际执行了
`0x109`，也不能仅凭匿名分配的池标签把代码归属为 VMware。

证据：[最终停止栈](logs/hvm-latest-0a-analysis-20260915.txt)、
[首个异常和保存的参数](logs/hvm-latest-pending-bugcheck-20260915.txt)、
[匿名例程反汇编](logs/hvm-latest-fault-owner-20260915.txt)、
[保存的异常帧](logs/hvm-latest-first-exception-frame-20260915.txt)。

**确定修复的是描述符表恢复遗漏。** 重复测试未出现上述停止码，仍需超过历史
失效时间的持续运行和更多机器验证，才能扩大对 `0xA` 消除效果的结论。

## 双核回归

`Test-DescriptorContinuity.ps1` 在同一次 Windows 启动内执行三组：

```text
launch-test-guest
  → resident-nested-hidehv
    → nested-probe-all
      → nested-selfvirt-all
        → stop
```

| 命令 | 成功 / 执行次数 | GDTR/IDTR 回读行数 |
| --- | --- | --- |
| launch-test-guest | 3 / 3 | 6 |
| resident-nested-hidehv | 3 / 3 | 0（启动不执行恢复） |
| nested-probe-all | 3 / 3 | 12 |
| nested-selfvirt-all | 3 / 3 | 12 |
| stop | 3 / 3 | 12 |

42 / 42 行的基址和界限匹配，时间戳均非零。双核探针与 stop 的每次执行都包含
CPU 0 和 CPU 1 的两张表；一次性来宾三次由 CPU 0 执行。各次 Windows boot time
均为 `2026-09-15T18:25:14.8924480Z`。本机观测的恢复前界限为 `0xFFFF`，
恢复后 GDTR 为 `0x0057`、IDTR 为 `0x0FFF`；IDTR 基址也恢复为各核自己的 Windows IDT。

每条命令的前态先在外层宿主落盘并 flush，再执行命令；后态、原始输出、事件分页、
回读结果写回同一个 run 文件。记录分别位于 `verified/`，不只保存汇总 PASS。

## 事件与命令输出

复用现有 lifecycle 事件，不增加 IOCTL 或命令。
`hvm_ctl --json events <afterSequence> <maxRows>` 新增两个输出字段：
`processorGroup`、`timestampQpc`。主程序 GUI 和 CLI 共用的命令引擎已同步；本轮没有测试 GUI。

描述符事件的载荷定义如下。地址字段在此属于诊断载荷，不能解释为来宾 GPA：

| 字段 | 含义 |
| --- | --- |
| ruleId | `0x47445452`（GDTR）或 `0x49445452`（IDTR） |
| exitReason | 1：常驻 stop；2：一次性来宾；3：嵌套探针 |
| guestPhysicalAddress | 预期表基址 |
| guestLinearAddress | 恢复后的硬件表基址 |
| guestRip | 恢复前的硬件表基址 |
| qualification[15:0] | 预期界限 |
| qualification[31:16] | 恢复后的硬件界限 |
| access | 恢复前界限 |
| status | 基址、界限均一致才为 0 |
| processorGroup / processor | 实际执行回读的处理器身份 |
| timestampQpc | 事件发布时的 QPC；频率在 run 的 before.qpcFrequency |

事件环可能覆盖旧记录，不能据“当前没有一条失败事件”推出从未失败。
本回归只认本次命令起始序列之后的完整预期回读。

## 真实 VMware / TinyCore 验证

已在更新后的驱动上重新观察到 `A5 → D1 → B2 → A5`：GPA `0x07000000`
的原页不受影子页写入污染，撤销后 `active=0`、`retired=0`，常驻保持两核。
Windows boot time、`vmware-vmx` PID 4528 及其创建时间均未变；TinyCore boot ID
保持 `f481ed7d-94c2-452c-a8dd-b8756176244c`。

原始值和每步输出位于 `teardown/write-isolation-20260915T184354Z.json`。
这是 `nosmp hpet=disable rdinit=/bin/sh` 的诊断启动，不代表默认 SMP/HPET 或 `/init` 已通过。

`Test-VmwareTeardown.ps1` 专门在常驻仍为 2、替换已撤销的情况下执行
`vmrun -T ws stop <vmx> hard`，保存 Windows boot time、VMware 身份、HVM 状态、
命令输出和外层 Hyper-V 18560 事件。三次全部通过；这些结果只能说明
所测配置下未复现旧故障，尚不能单独归因于某一处修复。

### 正常 `/init` 与时钟复核

随后撤去 `nosmp`、`hpet=disable` 和 `rdinit=/bin/sh`，保留串口、文本显示和
测试页保留参数。TinyCore 17.1 通过自己的 `/init` 到达 `tc@box:~$`，串口命令
确认 PID 1 为 `init`。完整串口、内核命令行和来宾 boot ID 位于
`normal-init/shell-capture.json`。此配置仍是 1 vCPU，不代表多核 AP 启动已通过。

来宾本次选用 `lapic-deadline` clockevent；先前的 HPET 可用诊断引导中，
`sleep 10` 能够完成，来宾 uptime 从 108.92 增至 120.47 秒。因此不能再把
“此版本任何时钟等待都会挂住”当作现状；但这也不是 HPET 作为 clockevent 的
直接回归。引导仍慢：网络模块日志到来宾 uptime 约 310 秒才出现。

在正常 `/init` 启动的来宾中，又完成一次 `A5 → D1 → B2 → A5`，Windows 和
VMware 身份不变，TinyCore boot ID 始终为 `6aafbfe6-c866-429b-86fc-8352b3dbe3e5`。
证据位于 `normal-init/write-isolation-20260915T190653Z.json`。

10 分钟观察共有 21 个样本，Windows boot time 和 VMware PID/创建时间不变，
常驻均为 2，无 FAULTED/ROLLBACK_REQUIRED。该区间包含正常引导和串口交互，
不能当作“启动后 10 分钟稳态性能测试”，也不替代小时级稳定性验证。

## `gdt-dump` 查询修复

旧命令依赖 CPL3 SGDT；本机得到的用户态视图不能用于取代内核 GDTR。
旧代码还会改变调用线程亲和性而不恢复，并为 SGDT 分配可执行用户内存。

现在复用主程序已有的 `IOCTL_KSWORD_ARK_QUERY_DRIVER_INTEGRITY`，由驱动逐核
取得 GDTR 和 GDT 描述符原始字节。命令校验协议版本、返回长度、表基址/界限一致性、
8/16 字节描述符覆盖范围；缺项或读取失败返回非零，不把缺失字节填零后报成功。
它不改变调用线程的亲和性，不需要新 IOCTL 或 HVM 翻译回退。

CPU 0、1 各读回完整 88 字节、limit `0x0057`；CPU 2（本机不存在）和 64（越界）
均 clean reject。原始记录为 `verified/gdt-query-*.json`。JSON 保留既有字段，
另加 `processorGroup` 与 `source="R0"`。输入仍为当前处理器组内的十进制 CPU 编号。

## 构建和证据边界

- MSVC/WDK Release 构建零警告、零错误；API Validator 和测试签名分别留有日志。
- 已验证驱动 SHA256：`E087E968FF83EBC91C83801AC62692FB3C9AC0C5FA2157F90D3AADC2948DA58C`。
- 15 条描述符回归、三次拆除及 10 分钟观察使用的 CLI SHA256：
  `987DD0EAE124BC74EED0E76D83DADBC4FF1AAE32B04C2C46159FD8DBAB6A476D`。
- 随后修复 GDT 查询的 CLI SHA256：
  `D44B16577F0F4A3334F04908F38BDA4C975FFBD737FB97113D93AFE68363195D`。
  四次 GDT 查询和正常 `/init` 下的换页闭环使用此版本，驱动未替换。
- 初轮数据缺少有效 CPU 编号及 CLI 时间戳，因此保留但不作为完整的逐核证据。
  修正元数据、重新构建和部署后，才得到 `verified/` 中的 15 条记录。
- 旧 [pilot](hvm-paper-evidence.md) 使用旧二进制；其启动/停止命令成功没有证明
  GDTR/IDTR 完整恢复，性能数据也不能作为本次二进制的复测结果。
- 尚未证明完整资源无泄漏、多核 TinyCore 一致性、Hyper-V 中间层兼容或长期稳定。
