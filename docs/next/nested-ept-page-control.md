# TinyCore 单页 EPT 替换

已在 `Hyper-V → KSword VMX → Windows 1 → VMware → TinyCore` 链路完成真实
来宾读写验证，Windows 1 位于 `KSword-HVM-Target`，外层 root Windows 保持 HVCI。
部署按标准流程重启 HVM TARGET；随后安装、使用、撤销 EPT 替换的整个过程没有
重启 Windows 或 VMware。

接口：`IOCTL_KSWORD_ARK_HVM_NESTED_PAGE`（`0x915`），协议位于
`shared/driver/KswordArkHvmIoctl.h`。它在已有 EPT12→EPT01 合成之后替换一个
4 KiB 叶项的物理页框，保留两级权限交集；只处理 WB RAM。

## 命令

`tools/hvm_ctl` 的三条命令：

```text
hvm_ctl --json nested-page-query
hvm_ctl --json nested-page-map <EPT12指针> <TinyCore物理页地址> <填充值>
hvm_ctl --json nested-page-remove
```

映射命令的参数均为十六进制。物理页地址必须按 4 KiB 对齐，填充值为
`00`–`FF`。驱动协议接收完整的 4096 字节影子页；命令行提供整页填充方式。
查询返回最近观测的 EPT12 根、当前替换、命中合成次数以及原页/影子页物理地址。
`active=1` 仅表示规则已发布；`composedCount>0` 表示目标页已按规则合成，
最终效果仍应由 TinyCore 内的读回验证。

## 生命周期

- 启动使用 `prepare-eptpsw`、`self-test`、`resident-nested-hidehv`。
- 映射与撤销期间保持 Windows、KSword 常驻和 VMware 来宾运行。
- 全处理器 IPI 进入私有 VMCALL，失效基础 EPT 与各处理器的合成 EPT。
- 规则发布后保持配置不变。撤销先取消发布，所有处理器完成失效后才释放页。
- 失效失败时返回非零状态并保留页。`retired=1` 表示撤销尚待完成，运行时未进入
  故障状态时可重试撤销；完整停止后的资源拆除也会释放保留页。
- `FAULTED` 或 `ROLLBACK_REQUIRED` 状态拒绝修改，避免在部分常驻已退出时继续换页。
- 同时仅支持一个替换。更换目标前先撤销；销毁 VMware 来宾前也应先撤销。
  EPTP 与物理页可能被 VMware 回收重用，此规则不跨虚拟机生命周期绑定。

## 专用测试页

TinyCore 诊断引导可使用以下追加参数，保留 `0x07000000` 的一页并直接进入 shell：

```text
nosmp hpet=disable rdinit=/bin/sh vga=normal nomodeset console=tty0 memmap=4K$0x7000000
```

先确认内核命令行和内存资源表中的保留范围，再从 TinyCore 写入原值、安装替换、
读回影子值、撤销并再次读回原值。不要将内核或应用正在使用的页当作填充测试页。
这是同一 TinyCore 内核及 initramfs 的诊断 shell，不代表默认启动脚本已完整通过。

此镜像有 `dd`、`od`，没有 `devmem` applet。挂载 proc 并确认保留范围后，
以下命令在 TinyCore 中将原页头四字节置为 `A5` 并读回：

```sh
mount -t proc proc /proc
grep 07000000 /proc/iomem
printf '\245\245\245\245' | dd of=/dev/mem bs=4 seek=29360128 count=1
dd if=/dev/mem bs=4 skip=29360128 count=1 | od -An -tx4
```

从 Windows 的 `nested-page-query` 获取当前 EPT12 指针，再执行
`nested-page-map <该指针> 7000000 d1`。在 TinyCore 重复最后一条读命令，
应得到 `d1d1d1d1`。将 `printf` 数据改成 `\262\262\262\262` 再写、再读，
应得到 `b2b2b2b2`。Windows 执行 `nested-page-remove` 后，TinyCore 再读应恢复
`a5a5a5a5`，证明影子页写入没有污染原页。

`logs/Invoke-TinyCoreCommand.ps1` 可经 VNC 输入这些命令。命令输出重定向到
`/dev/ttyS0` 后，由 `logs/Read-GuestSerial.ps1` 读取，作为来宾侧效果证据。

## 2026-09-15 实测

最终签名驱动 SHA256：
`3E131F93D8C3C09BAA34B486AC19BB5BC53DA85180A220642C6E872A9D20F1AC`。
TinyCore 内核为 `6.18.35-tinycore64`，VMware 来宾使用 1 个 vCPU，Windows 使用
2 个 vCPU。`/proc/iomem` 确认 `07000000-07000fff : Reserved`。

| 步骤 | TinyCore 读回 |
| --- | --- |
| 写入原页 | `a5a5a5a5` |
| 安装 `D1` 影子页 | `d1d1d1d1` |
| TinyCore 向影子页写入 `B2` | `b2b2b2b2` |
| 撤销 EPT 替换 | `a5a5a5a5` |

安装和撤销退出码均为 0，目标页合成次数为 3。验证前后 Windows 启动时间均为
`2026-09-15T10:18:32.0326920-04:00`，VMware 进程 PID 均为 `10492`；常驻处理器数
保持为 2。撤销后 `active=0`、`retired=0`。外层宿主的 VBS 状态为 2，
`SecurityServicesRunning` 包含 2，HVCI 正在运行。结束状态确认
`EPTP_SWITCH_ARMED` 已置位、CPUID 隐藏生效，且没有 `FAULTED` 或
`ROLLBACK_REQUIRED`。

证据：[最终效果日志](logs/hvm-tinycore-ept-final-effect-20260915.txt)、
[部署与产物哈希](logs/hvm-nested-page-final-deploy-20260915.txt)、
[最终构建](logs/hvm-nested-page-final-build-20260915.txt)、
[结束状态](logs/hvm-tinycore-ept-final-state-20260915.txt)。

本轮同时修复了 L2 退出后 L1 host 状态恢复、VMX 内存操作数未对齐及跨页访问、
进入 L2 前的 x87/SSE 状态恢复，以及 EPT12 与外层 EPT 的实际页框合成。
EPT 合成单元检查为 85 项通过；双核嵌套探针及自虚拟化探针均通过，后者包括
9 次进入、9 次反射、8 次恢复和 L2 自己检查的 XMM 标记。
探针在故障状态保护补丁之前的构建上验证，最终构建另行完成上述真实来宾闭环。

验证记录：[单元检查](logs/hvm-nept-unit-20260915.txt)、
[双核嵌套探针](logs/hvm-nested-page-live-20260915.txt)。

## 当前边界

- 已验证的是单个 4 KiB WB RAM 页；CLI 提供填充页，IOCTL 可传完整影子页内容。
- 当前 TinyCore 使用 `nosmp hpet=disable rdinit=/bin/sh` 诊断启动。
  默认 SMP/HPET 启动以及默认 `/init` 全流程尚未通过；未还原诊断用的单 vCPU 配置。
- 常驻期间直接拆除 VMware 的三重故障缺陷仍未修复。结束整套测试使用 HVM TARGET
  重启流程；单独停止 VMware 前须先停止 HVM 并确认常驻数归零且无回滚待完成状态。
- 原有 Windows `0xA` 崩溃仍未归因，曾在没有安装单页替换时复现。
- 尚未覆盖任意重映射的 EPT12 页表自身、MTF 后端的嵌套视图切换，以及失效失败的
  故障注入；本次结果不代表完整嵌套 VMX 功能或长期稳定性验证。
