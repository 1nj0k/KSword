# HVM 性能与论文证据：2026-09-15 单机试验

数据目录：[20260915-pilot](paper-data/20260915-pilot/README.md)。
机器可读汇总：[summary.json](paper-data/20260915-pilot/derived/summary.json)。
原始文件校验：[raw-index.json](paper-data/20260915-pilot/raw-index.json)。

后续审查发现该批旧驱动的 GDTR/IDTR 恢复遗漏，现已修复并单独回归，见
[描述符表连续性记录](hvm-descriptor-continuity.md)。本页保留原始观测；其中
start/stop 的成功只证明所记录的命令与运行状态，不证明完整描述符状态恢复。
性能数字也只适用于本批记录的旧二进制，不能作为修复后版本的复测结果。

本批证据支持：**Windows 在本次启动内进入／退出 KSword 常驻，以及在已运行的
VMware → TinyCore 中完成单页 EPT 替换、写入隔离与恢复。**
这两组实验分开执行；尚不能声称“运行中的 VMware 随 Windows 一起完成拓扑平移”。
TinyCore 使用诊断 shell，默认 SMP、HPET 与 `/init` 启动流程不在本批通过范围内。

## 1. 环境与产物身份

| 项目 | 本批实测值 |
| --- | --- |
| 物理 CPU | Intel Core i7-13700F；Family 6 / Model 183 / Stepping 1；16 核、24 逻辑处理器 |
| root Windows 0 | Windows 11 Pro for Workstations Insider Preview，26300.9022 / 26H2 |
| root HVCI | VBS 状态 2，SecurityServicesRunning = `[2]` |
| Hyper-V 二进制 | hvix64 10.0.26100.9022；vmms 10.0.26100.8875；vid 10.0.26100.8941 |
| BIOS | ASUS S501ME.331；没有读取完整厂商 BIOS 开关表或微码修订号 |
| Windows 1 | Windows 11 Home，22621.4317 / 22H2；VBS 状态 0 |
| Windows 1 虚拟机 | KSword-HVM-Target；Gen 2 / 配置版本 12.0；2 vCPU、固定 8 GiB |
| Hyper-V 开关 | ExposeVirtualizationExtensions=true；DynamicMemory=false；Secure Boot 启用 |
| VMware | Workstation 17.6.4 build-24832109 |
| TinyCore | 17.1；6.18.35-tinycore64；`#1 SMP Wed Jun 17 00:02:49 UTC 2026` |
| TinyCore 配置 | 1 vCPU；`nosmp hpet=disable rdinit=/bin/sh`；reserved GPA `0x07000000`；e1000 / NAT |
| KSword 源码 | 主程序／共享命令引擎 `7af2fa6b`；被测驱动源码 `7360d502` |
| 编译器／微基准 | MinGW GCC 13.1.0，C11 / O2 / static；Windows QPC，CPU affinity=0 |

原始记录：[root-environment.json](paper-data/20260915-pilot/root-environment.json)、
[windows1-environment.json](paper-data/20260915-pilot/windows1-environment.json)、
[build-manifest.json](paper-data/20260915-pilot/build-manifest.json)、
[TinyCore 串口](paper-data/20260915-pilot/tinycore-serial.txt)。
不能把 Hyper-V root 中 CIM 返回的 `VirtualizationFirmwareEnabled=false` 推导成
BIOS 关闭了 VT-x；这是该环境中被观测到的接口返回值。

关键 SHA256：

| 文件 | SHA256 |
| --- | --- |
| KswordARK.sys | `3E131F93D8C3C09BAA34B486AC19BB5BC53DA85180A220642C6E872A9D20F1AC` |
| hvm_ctl.exe | `925C2239902ABE625ECDCA702DC3CE76EFB7D42EA5F3EDFBA8577414A6BDCDDB` |
| Windows microbench.exe | `A37D9D1B3FC22D5539C51C3DC364B7C1BAA0061913B72EFEF8183E020D464E0D` |
| VMware vmware-vmx.exe | `7F58CF026DE36ECF522249D3FA26F87416D78061C456FE518F4F9D999B2B862A` |

## 2. Windows steady-state 开销

同一 Windows 1 boot、同一驱动／微基准二进制、同一 2 vCPU 配置，顺序执行
**off/pre → on → off/post**。三个块均没有 VMware。off 状态保留已准备的 HVM/EPT
资源，resident=0；on 为 `resident-nested-hidehv`，resident=2。
每块每个负载先预热 1 次，再正式测量 7 次；负载顺序使用固定种子洗牌。
所有 168 次进程执行完成，另有先前 VMware 运行条件下的 56 次独立记录。
磁盘负载一次产生读、写两个指标，所以原始执行数和指标行数不同。

百分比 = `(on 耗时中位数 / off 耗时中位数 − 1) × 100`。
off 合并前后两块共 14 个样本，on 为 7 个；95% 区间来自 10,000 次中位数
百分位 bootstrap，种子 20260915。它仅描述本次单机样本，不能替代跨机器、跨启动
或随机交叉试验。正值表示变慢；负值保留原始结果，不自动解释为性能改善。

| 负载 | off 中位数 | on 中位数 | 耗时变化 | bootstrap 95% 区间 |
| --- | ---: | ---: | ---: | ---: |
| 整数 xorshift，1 亿次 | 123.336 ms | 125.555 ms | **+1.80%** | +0.70% ～ +2.86% |
| CPUID leaf 0，10 万次 | 33.321 ms | 342.993 ms | **+929.37%（10.29 倍）** | +903.00% ～ +954.21% |
| memcpy，32 MiB × 64 | 159.453 ms | 157.798 ms | −1.04% | −8.77% ～ +9.65% |
| 64 MiB 随机指针追踪 | 735.597 ms | 721.571 ms | −1.91% | −2.54% ～ +0.37% |
| 64 MiB VHDX 路径直接写入 | 36.917 ms | 33.755 ms | −8.56% | −21.30% ～ +12.35% |
| 64 MiB VHDX 路径直接读取 | 32.527 ms | 31.911 ms | −1.89% | −8.42% ～ +1.70% |
| TCP 回环批量发送，64 MiB | 23.105 ms | 21.816 ms | −5.58% | −13.15% ～ +7.41% |
| TCP 回环 1 字节往返，2000 次 | 23.746 ms | 32.760 ms | **+37.96%** | +32.06% ～ +47.24% |

![Windows 开销及置信区间](paper-data/20260915-pilot/derived/windows-overhead.png)

可导出 [SVG](paper-data/20260915-pilot/derived/windows-overhead.svg)。
逐次表：[windows-runs.csv](paper-data/20260915-pilot/derived/windows-runs.csv)；
统计表：[windows-overhead.csv](paper-data/20260915-pilot/derived/windows-overhead.csv)。
完整样本、二进制身份、PID、退出码、清理结果、前后状态均保存在对应原始 JSON。

解释限制：

- 内存复制按复制字节计数一次，不按读写总流量翻倍；指针追踪包含缓存、TLB 与嵌套翻译影响。
- `FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH` 只约束 Windows 1 的请求，
  外层 VHDX、宿主缓存、设备缓存仍可能介入，不能称为裸物理盘吞吐。
- TCP 是本机 loopback，不代表物理网卡或跨机器带宽。
- off/post 相对 off/pre 的内存复制中位数漂移 −6.74%、TCP bulk 漂移 −12.86%。
  这些负载的微小开销暂时无法可靠分辨，不能据此宣称“零开销”。
- VMware 运行条件的 `ksword-on` 记录单列，未与无 VMware 的 off 块计算开销。
- 本批没有改动 HVM 快速路径。测得的是包含现有遥测的产品构建成本。

## 3. 拓扑平移、暂停与多核连续性

匹配试验的 Windows boot time 始终为 `2026-09-15T16:33:08.3527820Z`。
每次记录 services、wininit、lsass 的 PID 和创建时间，以及两行 vCPU 的
index / group / number。无忙循环观察器的 **7 次启动、7 次停止均完成**，前后
Windows boot time 和关键进程身份一致；处理器身份为 `(0,0,0)`、`(1,0,1)`。

| 观测条件 | 操作 | 完成数 | 命令耗时中位数 | 范围 |
| --- | --- | ---: | ---: | ---: |
| 无忙循环观察器 | 启动 | 7/7 | 15.095 ms | 13.712 ～ 18.421 ms |
| 无忙循环观察器 | 停止 | 7/7 | 15.424 ms | 14.418 ～ 17.638 ms |
| 两个 CPU 持续采样 QPC | 启动 | 2/2 | 4043.062 ms | 843.193 ～ 7242.931 ms |
| 两个 CPU 持续采样 QPC | 停止 | 1/2 在期限内 | 完成的 1 次为 14776.792 ms | 另 1 次超过 30 秒期限 |

超时样本原样保留。超时后快照已经显示 resident=0，之后复核没有 hvm_ctl、
transition_probe 或 microbench 残留进程，也没有 FAULTED / ROLLBACK_REQUIRED。
它计为 **`command_timeout_state_reached`**，既不抹成 PASS，也不推导为来宾崩溃。
原计划 8 组忙循环试验在第 2 组中止，余下 6 组未执行。

这些数值均包含命令进程启动及返回。**它们不是 Windows 的总暂停时间。**
忙循环观察器明显改变了测量条件：该条件下每核与命令区间相交的最大执行间隙
为 0.998 ～ 16.213 ms，但同时包含 Windows/Hyper-V 调度与观察器成本。
仅保留每核最大的 16 个间隙；未命中某个区间不等于该区间间隙为零。

quiesce、state capture、VMCS setup、EPT build、resume 的内部阶段时间，
真正的 transition pause、每核进入新拓扑的时刻、skew、IPI latency **尚无计时埋点**，
在数据中均为 null。两核最终 resident=2 不能证明它们同一瞬间进入新拓扑。

逐次记录：[transition-runs.csv](paper-data/20260915-pilot/derived/transition-runs.csv)、
[每核间隙](paper-data/20260915-pilot/derived/transition-cpu-gaps.csv)、
[超时后的复核](paper-data/20260915-pilot/after-probe-timeout.json)。
标准重启只发生在匹配试验准备阶段，明确记录于
[orchestration.jsonl](paper-data/20260915-pilot/orchestration.jsonl)，不计入上述平移事件。

## 4. TinyCore EPT 换页与写入隔离

在已有 VMware 来宾内控制 GPA `0x07000000`，EPT12=`0x000000001CAC605E`。
连续换页试验中，原 backing=`0x00000001043D3000`，replacement backing=
`0x0000000204FE5000`，后者在释放后被分配器重复使用。
这些 backing 地址属于 **KSword / Windows 1 的物理地址域**，不等同于物理宿主 PFN。

- 空闲条件 10 轮；来宾 MD5 负载条件 5 轮。逐轮使用 C1…CA/C1…C5 作为可辨认值。
- **15/15** 均观测到相应替换值，并在 remove 后再次读到 A5。
- 每轮 mapped 查询有 `composedCount>0`，remove 后 `active=0, retired=0`。
- 负载区间 guest `/proc/stat` 的 idle 增量为 0；这是来宾自身记账，不等同于物理 CPU 利用率。
- map 命令中位数 **7.414 ms**，remove **8.022 ms**；包含 CLI/查询/控制开销，非单条 INVEPT 延迟。

另一次独立的写入隔离记录：

| 步骤 | 来宾读回 | 来宾 uptime 秒 |
| --- | --- | ---: |
| 原页 | `a5a5a5a5` | 7702.01 |
| D1 替换页 | `d1d1d1d1` | 7731.83 |
| 向替换页写入 B2 | `b2b2b2b2` | 7732.08 |
| remove 后 | `a5a5a5a5` | 7750.28 |

前后 Windows boot time 为 `2026-09-15T14:18:32.0326920Z`；
vmware-vmx PID=10492、创建时间 `2026-09-15T14:19:00.665121Z`；
TinyCore boot ID=`7230583c-637f-4c98-ac86-6d79c1bce8a8`，均未变。
观测点证明替换页写入没有污染此原页的前四字节；没有做整页随机数据校验。

原始闭环：[write-isolation](paper-data/20260915-pilot/write-isolation-20260915T162828Z.json)；
逐次表：[nested-page-runs.csv](paper-data/20260915-pilot/derived/nested-page-runs.csv)。
每一步包含 CLI 参数、退出码、Windows UTC/QPC、映射代次、页地址、前后状态和来宾串口片段。
来宾读写只带 `/proc/uptime` 时间，未与宿主时钟严格校准；不能跨时钟域直接相减。

## 5. 跨层控制：能证明什么

窄范围的单页控制路径为：

```text
hvm_ctl → 共享 HVM 命令引擎 → KswordARK IOCTL
  → 分配/发布 KSword replacement page
  → KeIpiGenericCall → 各核私有 VMCALL
  → INVEPT 与合成 EPT 失效 → 来宾重访时使用新 backing
```

此路径未调用 vmrun、PowerShell Hyper-V 管理接口，也未通过安装 VMM hook 或向 VMM
注入程序完成换页。来宾保持原有 VMware PID/创建时间，vmware-vmx 可执行文件和 VMX
配置的磁盘 SHA256 在隔离闭环前后相同。

**不能据此写成“整个实验从未调用、修改 Hyper-V/VMware”。**
实验编排使用 PowerShell Direct 和 VMware VNC；部署使用 vmrun 与 vmx86 服务控制。
此外，`hvm_nested_ept.c` 的 `KswordARKHvmNestedEptPropagateAccessedDirty`
会通过 `KswordARKHvmPhysWindowWriteQword` 把 A/D 位回写到 VMware 所拥有的 EPT12
条目。它不是 VMM 代码 hook，但属于 VMM 管理数据的写入路径；本批未统计实际回写次数。
“所有 VMM 内存始终未改动”既未测得，也不是当前实现可支持的绝对描述。

源码证据：[nested EPT](../../KswordARKDriver/src/features/hvm/hvm_nested_ept.c)、
[IPI/常驻](../../KswordARKDriver/src/features/hvm/hvm_resident.c)、
[退出处理](../../KswordARKDriver/src/features/hvm/hvm_exit.c)。
没有独立的 VMM 全内存完整性基线，也没有对物理 Hyper-V 的全部调用做审计。

## 6. 回滚、资源与故障注入

| 场景 | 执行数 | 结果与范围 |
| --- | ---: | --- |
| 超出 52 位的 GPA | 1 | 驱动 clean reject，`0xC000000D`；代次、active、retired 不变 |
| 未对齐 GPA | 1 | CLI parser clean reject；没有进入 R0 map |
| 未知 EPT12 root | 1 | 驱动 clean reject；完整 NTSTATUS 在原始 JSON |
| commit 后正常 restore | 15 + 写入隔离 1 | 来宾值恢复、active/retired 清零；不等同于失效失败后的异常回滚 |
| 连续 remap/restore | 10 | 每轮单独落盘，全部闭环 |
| 来宾 CPU 负载时操作 | 5 | 全部闭环；单 guest vCPU |
| replacement 分配失败 | 0 | 未注入；当前构建没有确定性的分配失败入口 |
| commit 前取消 | 0 | 未注入；同步接口没有独立 prepare/commit/cancel 事务 |
| guest 多核同时访问 | 0 | 未执行；当前 TinyCore 为 1 vCPU / nosmp |
| INVEPT 失败、retired 保留后重试 | 0 | 未注入；不能用正常 remove 替代这项 |

被观测的映射槽最多含一个 replacement page；正常撤销后活动槽和 retired 槽均为空。
当前接口没有总分配／释放次数、outstanding allocations 或全局 stale-mapping 计数。
**active=0/retired=0 不构成“驱动所有页无泄漏”的证明。**
没有把内存压力、非法 GPA 或未对齐拒绝冒充 replacement 分配失败。

## 7. 虚拟化计数与长期稳定性

监视器运行 600.3 秒，共 21 个成功样本、0 个采集错误；首末完成采样间隔
**594.209 秒**，用于计算退出率。所有样本 Windows／VMware 进程身份不变，
KSword 两核常驻身份不变，无 FAULTED / ROLLBACK_REQUIRED，来宾 heartbeat 的 boot ID 唯一。
监视期间包含编译准备、系统信息采集、网络准备和末段微基准活动，不能称为全程 idle。

| 指标 | 本批值 |
| --- | ---: |
| 现有普通退出计数增量 | 3,975,041 |
| 上述计数平均速率 | 6,689.63 / 秒 |
| HLT 分类增量 | 1,789,615 |
| CPUID 分类增量 | 809,918 |
| nested INVEPT 指令退出（reason 50） | 33,689 |
| nested INVVPID 指令退出（reason 53） | 33,699 |
| 普通路径 EPT violation（reason 48） | 0；不覆盖提前由 L2 路由处理的退出 |
| 事件环 dropped 增量 | 0 |
| 事件环 overwritten 增量 | 6,965 |
| vmware-vmx private bytes 增长 | 0 |
| Windows nonpaged pool 增长 | 2,633,728 bytes |

`hvm_exit.c` 的反射／已处理 L2 路径在普通直方图与 `VmExitCount` 发布前返回。
所以这里不是整条虚拟化链的硬件 VM-exit 总数。reason 50/53 也不是 KSword 实际执行
INVEPT/INVVPID 的总次数。完整分类见 [stability.json](paper-data/20260915-pilot/derived/stability.json)。
内核内部另有 L2 分类和成本计数，但当前 CLI 未导出完整数据。

采样调用自身耗时中位数约 350 ms、最大 5332 ms；这是采集扰动。
nonpaged pool 增长没有按驱动 tag 归因，不能认定是 KSword 泄漏，也不能认定不存在泄漏。
事件环存在覆盖，不能从“日志没出现某事件”推导“从未发生”。
本批没有 1 小时或数小时连续监视证据；历史调试中的 0xA 蓝屏和拆 VMware 重置问题，
不因本批无崩溃而自动结案。

## 8. TinyCore 内部性能样本

| 项目 | 样本 | 结果 |
| --- | --- | --- |
| MD5，64 MiB RAM 文件 | 1 预热 + 7 正式 | uptime 包络中位数 0.390 秒；摘要一致 |
| 32 MiB 光驱缓冲读取 | 1 预热 + 7 正式 | dd 报告中位数 0.828030 秒，约 38.65 MiB/s |
| ICMP loopback | 1 次调用、7 包 | 全部响应；min/avg/max=0.579/2.603/4.249 ms |
| NAT DNS 端点 192.168.61.2 | 1 次调用、3 包 | 全部响应；min/avg/max=3.382/10.194/17.405 ms |
| Windows NAT 接口 192.168.61.1 | 1 次调用、7 包 | 7 包全丢；原因未归因，不能当作带宽或延迟结果 |
| SHA256 工具尝试 | 7 次 | 命令不存在，全部记 environment_error |

数据：[linux-runs.csv](paper-data/20260915-pilot/derived/linux-runs.csv)、
[逐次原始片段 JSON](paper-data/20260915-pilot/derived/linux-runs/)。
MD5 包络包含 shell、串口和 `/proc/uptime` 读取开销；uptime 分辨率 10 ms。
光驱首次读取与后续缓存读取分列。rootfs 位于 RAM，不计为磁盘写入性能。
没有同条件的无 KSword TinyCore 基线，因此 **nested guest overhead 仍为空**。
来宾内的纯内存带宽／延迟、块盘写入、TCP 吞吐也没有合格数据。
Linux 微基准二进制传输被自动审批拒绝，未执行该传输；没有绕过拒绝换路传入。

## 9. 普适性矩阵与证据覆盖

| descendant / OS | EPT 效果 | 稳态基线 | 平移与连续性 | 稳定性 |
| --- | --- | --- | --- | --- |
| VMware / TinyCore 17.1 诊断 shell | 单页闭环 16 次 | 只有 on；无等价 off | EPT 操作期间连续；运行中拓扑平移未证明 | 10 分钟观察 |
| Windows 1 / 22621.4317，无 VMware | 不作为 TinyCore EPT 证明 | off/on/off，各 7 次正式 | 7 组常规启停；另有忙循环条件超时 | 试验区间内身份连续 |
| Hyper-V descendant / TinyCore | 未执行 | 未执行 | 未执行 | 未执行 |
| VMware 或 Hyper-V descendant / Win10 LTSC | 用户要求暂时跳过 | 跳过 | 跳过 | 跳过 |
| 其他 CPU / Windows build | 无可用样本 | 无 | 无 | 无 |

用户要求的 15 类数据均已在本报告或结构化
[coverage.json](paper-data/20260915-pilot/coverage.json) 中逐项给出实测范围或缺项原因。
这不代表 15 类全部测完。最关键的剩余证据为：内部阶段暂停埋点、运行中 VMware
跨拓扑平移、nested guest 等价基线、分配失败／异常回滚注入、guest 多核访问和完整资源账本。

## 10. 复算、审计与适用边界

```powershell
python tools/hvm_paper/analyze.py docs/next/paper-data/20260915-pilot
python tools/hvm_paper/plot.py docs/next/paper-data/20260915-pilot
python tools/hvm_paper/validate.py docs/next/paper-data/20260915-pilot
```

原始 JSON/JSONL/串口不由分析脚本重写，派生表放在 `derived/`。每次 run 保留
唯一 ID、原始返回值与状态；启动后尚未结束的记录不会被统计成成功。
地址以十六进制字符串保存，QPC 保留整数计数与频率；当前数值没有超过 JSON 精确整数范围。
对未来长时间或更大计数的采集，应继续使用无损整数解析器。

附加质量记录包括：二进制/源码哈希、编译器、预热、固定随机种子、前后基线漂移、
bootstrap 方法、观察器耗时、事件覆盖数、超时后的状态复核、时间域与地址域说明。
本批未运行主程序 GUI 测试，也没有改动 GUI 功能或驱动实现。
