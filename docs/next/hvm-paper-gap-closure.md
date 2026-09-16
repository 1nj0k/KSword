# HVM 论文缺口处理：2026-09-15 追加证据

> 历史 2×2 批次。后续四核 Windows、双核 TinyCore、性能优化、HTTP 场景及匿名包见
> [4×2 报告](hvm-4x2-results.md)。本页的十分钟观测不属于后续驱动。

## 已走通的实验顺序

1. VMware 未运行时，Windows 1 在同一次启动内进入、退出 KSword 常驻。
2. KSword 常驻后启动 VMware 和 TinyCore，保持它们运行，替换、写入并恢复
   TinyCore 的保留页。当前 TinyCore 已走完正常 `/init` 并进入双核 shell。

这两项是独立实验。完整运行中的 VMware/TinyCore 随 Windows 一起进入新增
监控器的 VMX 所有权交接仍未实现，不能由上述两项拼接推出。
用户随后要求暂时搁置这条整链平移路径；它不属于当前已验收能力，也没有被确认为
VMware 自身缺陷。此次检查没有移除 VMX 所有权保护或部署未经验证的接管实现。

## 本次代码修复

源码提交 `a7e1842f2d4a9bb3d3e77c8c5e98a53c8b2f3263`：

- 修复提交失效失败后仍留下活动换页规则的问题：撤销发布，再执行全核失效；
  只有全核确认后才能释放 backing，失败时保留在 retired 槽，支持后续重试。
- 加入真实的规则对象/替换页分配与释放计数，区分逻辑撤销和实际回收。
- 加入 5 种逐请求故障模式，分别覆盖替换页分配失败、发布前取消、提交后回滚、
  提交失效调用失败、撤销失效调用失败。后两项模拟调用边界失败，不冒充硬件故障。
- 加入可关联到操作编号的阶段时间戳；增加 `metrics` 的版本化只读接口。
- 将 VM-exit 计数移到嵌套处理提前返回之前，补上此前遗漏的退出；统计实际 INVEPT。
- 主程序内嵌 HVM 命令面板和独立 CLI 继续使用同一命令目录与执行引擎。
  主程序、驱动、KswordCLI 已构建；未执行 GUI 测试。

驱动构建显式使用 `KswordArkSkipAutoVariantSign=true` 和
`KswordArkSkipAutoTestSign=true`，之后用已有普通测试证书签名。
双核实验驱动 SHA256：
`667D21B398B7C6A6FA9D00BAC203576C05E32B6F33F0536573C99011D3EFC407`。

## 双核 EPT 效果与故障恢复

环境：Windows 1 / KSword 各 2 vCPU；VMware Workstation 17.6.4；TinyCore 17.1，
Linux 6.18.35-tinycore64，2 vCPU。引导没有 `nosmp`、`hpet=disable` 或
`rdinit=/bin/sh`；仅保留控制台、视频和 `memmap=4K$0x7000000` 测试参数。
完整身份见 [environment.json](paper-data/20260915-gap-closure/smp/environment.json)。

在 GPA `0x07000000` 保留整页并填充 A5。两个观测进程分别固定到 CPU 0、CPU 1，
每轮报告 guest boot ID、uptime、前四字节和整页 MD5。MD5 用于检查预期测试内容，
不作为对抗性密码学完整性保证。读四字节和读整页不是同一原子操作。

- **20/20 次双核换页/恢复完成读值闭环**，其中空闲 10 次、双核 CPU 负载 10 次。
  空闲 10 次与负载 6 次阶段日志完整；负载的 4 次早期记录缺少事件页，不能用于阶段耗时。
  Windows boot、VMware PID/创建时间、
  guest boot ID 保持不变；两颗 CPU 都读到整页替换值，随后恢复整页 A5。
- 映射控制主体中位数 **132.9 µs**，恢复 **115.85 µs**；提交失效区间中位数
  **104.7 µs**。这些是驱动 QPC 区间，不是 CLI 返回耗时或应用暂停时间。
- 负载期间两颗 CPU 的 `/proc/stat` 忙碌比例均超过 99%。6 次阶段日志完整的负载
  试验中，映射主体中位数 **177.55 µs**，恢复 **153.65 µs**；与空闲结果分开统计。
- **3/3 非法请求**干净拒绝：超出 52 位的 GPA、未对齐 GPA、未知 EPT12 根。
  未对齐请求由 CLI 拒绝；不能宣称所有不存在的物理地址都已由驱动预先验证。
- **50 次故障控制试验**均满足预期状态/资源断言；**44 次证据完整**，
  **6 次早期记录不完整**。修正事件分页和读回等待后的独立重跑 **25/25** 完整通过。
  不完整记录没有删除或改写为 PASS。
- 双核写入隔离的独立记录保留了 A5 → D1 → B2 → A5 序列；B2 只写了页头四字节。
  读者的整页校验随后覆盖 `B2 × 4 + D1 × 4092`，恢复后再次覆盖 `A5 × 4096`。

原始记录和逐次判定：
[双核数据目录](paper-data/20260915-gap-closure/smp/)、
[汇总](paper-data/20260915-gap-closure/smp/derived/smp-summary.json)、
[故障明细](paper-data/20260915-gap-closure/smp/derived/faults.csv)、
[换页明细](paper-data/20260915-gap-closure/smp/derived/cycles.csv)。

## 无 KSword 基线与 Hyper-V 中间层

三次独立尝试分别使用原配置、启用 WHP、启用 WHP+VMP；均在没有 KSword 常驻时
启动 VMware，并保存新产生的临时启动日志。三次都被 VMware 拒绝，不能计为
成功启动，更不能从中计算 nested guest 的开销。日志表明检测到外层 Hyper-V，
但这三次失败不能证明有效 WHP 路径不可用：再次核对发现
`vmp-baseline-diagnostics.json` 在恢复组件之前仍记录 `hypervisorlaunchtype Off`，
尽管更早的启用记录显示 BCD 命令返回成功。组件启用和命令成功没有证明内层 hypervisor
实际启动。旧失败记录全部保留，WHP 基线的有效性标为未确认。试验后已恢复组件状态和
BCD，并重启 HVM TARGET。

Windows 1 是 Home/Core 版，全 Hyper-V 角色及 vmms 不存在；微软文档明确该版本
不支持安装该角色。因此，这台靶机没有取得 Hyper-V 作为中间 VMM 的正向证据。
这属于环境限制，不能记成 KSword 兼容性失败，也不能写成已兼容。
[微软安装要求](https://learn.microsoft.com/en-us/windows-server/virtualization/hyper-v/get-started/install-hyper-v)

更换 Windows edition 也不是兼容性保证：Hyper-V 的启动/VMX 所有权路径与后启动的
VMware 不同，仍需单独验证，不能假定安装角色就能复用同一顺序。

## 性能与时间口径

### 最终同版本 Windows A/B

最终批次使用上述 `667D21B3…` 驱动和相同 CLI，Windows 同一次启动，VMware 全程
不运行。顺序为 off/pre → on → off/post；每项每块 1 次预热、7 次测量，共 168 次
微基准进程执行、192 条指标记录。off 仍保留已准备资源，因此比较的是常驻执行成本，
不是驱动不存在时的系统。所有记录有效，**10/10 组进入/退出**均保持 boot 与进程身份。

| 指标 | 耗时变化 | 条件 bootstrap 95% 区间 | off/post 相对 off/pre |
| --- | ---: | ---: | ---: |
| CPUID leaf 0 | +930.26%（10.30×） | +908.43%～+1017.00% | −3.22% |
| 整数计算 | +3.82% | −0.37%～+6.82% | +0.15% |
| memcpy 32 MiB | +0.96% | −9.42%～+33.55% | +5.41% |
| pointer chase 64 MiB | +3.74% | −2.57%～+10.07% | −1.18% |
| 磁盘读取 64 MiB | +6.53% | −0.11%～+9.82% | +0.23% |
| 磁盘写入 64 MiB | −10.69% | −26.55%～+31.00% | −8.21% |
| TCP 回环 bulk 64 MiB | +3.17% | −1.62%～+18.56% | −14.25% |
| TCP 回环 RTT 1 B | +36.85% | +32.74%～+40.60% | −0.01% |

区间来自同一次启动内的 10,000 次中位数 bootstrap，固定 seed；不代表跨启动或
跨机器置信区间。整数、内存、磁盘与 bulk 的区间跨零，不能据此宣称确定的改善或
退化。TCP 是回环网络，磁盘是来宾直接 I/O 请求；二者都不是物理硬件隔离测量。

内部时间：冷准备 1 次 **12.084 ms**，其中 EPT 建表 **11.964 ms**，在插入之前。
10 次插入的控制主体中位数 **1.257 ms**，IPI rendezvous 包络中位数 **90.55 µs**
（69.4～120.8 µs）；两核 entry 返回偏差中位数 **4.45 µs**，最大 **18.5 µs**；
IPI 到达偏差中位数 **0.2 µs**，最大 **1.5 µs**。退出主体中位数 **748.5 µs**，
rendezvous 包络 **40.95 µs**。逐核状态捕获、VMCS 编程和 entry 边界均单独落盘。

见 [最终 A/B 数据](paper-data/20260915-gap-closure/final-windows/)、
[开销表](paper-data/20260915-gap-closure/final-windows/derived/windows-overhead.csv)、
[阶段汇总](paper-data/20260915-gap-closure/final-windows/derived/internal-transition-summary.json)。

### 稳定性与版本隔离

当前双核来宾完成独立的 10 分钟观测：600.6 秒采集过程、21 个采样点，首尾采样
跨度 599.43 秒。Windows/VMware/guest 身份一致；两颗 CPU 的整页原值读回持续
前进；所有采样点 active/retired 和页面设施未释放分配数均为零。记录到
94,876,125 次 KSword VM-exit（约 158,277 次/秒）、794,960 次实际 INVEPT，
失败增量为零。VMware private bytes 首尾增长 28,672 字节，系统 nonpaged pool
减少 888,832 字节；这些总量不能证明整台系统不存在泄漏。见
[观测汇总](paper-data/20260915-gap-closure/smp/derived/stability.json)。

同一驱动随后在常驻 2 核时关闭 VMware 的独立复测 **1/1 通过**：Windows 未重启，
未出现 Hyper-V 18560 三重故障。见
[拆除原始记录](paper-data/20260915-gap-closure/smp/vmware-teardown-20260915T212150975Z.json)。
此前描述符修复版本的三次拆除通过保留在其独立数据集中，不混为当前版本的四次试验。

数据根目录的首批 10 组进入/退出、168 次 Windows 微基准来自测量原型驱动
`35380F71…`。随后回滚修复驱动是 `667D21B3…`；两个版本必须分开，不得把
早先的数字贴到新驱动上。每一组后续 A/B 使用独立目录并记录精确二进制。

`metrics` 区分准备/EPT 建表、全核 rendezvous、CPU 状态捕获与 VMCS 编程、
VM-entry 前后边界。各 CPU 区间并行，不能相加为总暂停；rendezvous 包络是
扰动上界，不是应用层直接测得的暂停。实际 INVEPT 计数包含背景嵌套工作；
VM-exit 计数只覆盖 KSword 入口，不能声称是整个 Hyper-V 系统的全部硬件退出。

Linux 同源微基准已在 Ubuntu GCC 13.3 下静态编译。向 TinyCore 传输的操作被
自动审批以 `blocked by policy` 拦截，去掉防火墙改动后的方案也被拦截；没有
执行或取得新微基准结果。原有 BusyBox 数据仍只代表其明确标注的负载。

## 仍需解决的论文问题

- 有用应用与优势比较：整页标记及写隔离只能证明机制；不能替代实际应用价值。
- 单机、单 boot、小样本的局限；其他 CPU/Windows build 由后续实验补充，
  Win10 LTSC 按用户要求暂缓。
- 一个 EPT12 根、一个保留 WB RAM 页；VM 销毁后的根复用、DMA、任意应用页的一致性
  不在当前契约中。物理指令故障、CPU 丢失、跨 processor group 仍缺实测。
- 日志中的 original/replacement physical page 是 Windows 1 可见的物理地址空间；
  最外层 Hyper-V 的真实宿主物理页映射没有被导出。TinyCore GPA 与 backing 地址不能混用。
- 默认双核启动通过不等于 HPET clockevent 单独回归通过；需要按时钟配置分别验证。
- 研究新颖性须正面比较既有 late launch、嵌套分页、CloudVisor、HyperFresh、
  HyperTP 等工作，不能使用“首次”“零开销”或笼统的“无修改”作为代替。

已补 [相关工作对比](eurosys2027/related-work.md)。原始证据包含本机路径和身份信息，
是作者工作档案，不能原样上传为双盲附件；投稿副本须保留可验证映射并单独去标识。
