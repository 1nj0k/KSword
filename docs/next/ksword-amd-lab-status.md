# AMD 实验实现状态（2026-09-17，硬件验收前）

目前是可编译的实验候选，**尚未执行第一次来宾 VMRUN，未证明单核或多核常驻可用**。
本文件用于重启后续接，不能用作硬件通过报告。源代码尚未提交；候选清单绑定基准提交与修改文件哈希。

## 已实现与已验证

- 新增 AMD 能力/有效位、资源账本、VMCB 布局、独立汇编入口、退出处理、完整身份 NPT、逐核诊断及公共生命周期接入。
- 逐核集合检查、准备/自检电源代次、进入失败后停止已进入核、无法证明退出时保留资源/卸载保护；实现这些路径不等于已验证其硬件行为。
- 主协议 v5、metrics v3、CLI/GUI 的 AMD 展示与 Intel 专用命令拒绝。
- 两个宿主入口、只读启动核验、独立 VMware 全克隆、来宾配置与命令日志/亲和性负载脚本。
- 标准 MSVC/WDK Release x64 编译和链接通过，驱动零警告；WDK x64 输出 `Driver is 'Universal'.`；Inf2Cat 构建目标通过。
- AMD 纯逻辑测试 73,907 次断言、既有 Intel 85 项检查通过。前者包含循环边界枚举，不能解读成同等数量的独立硬件测试。
- 启动策略 12 项、模拟 BCD 事务 21 项通过；无实际 BCD 往返。CLI 命令一致性、IOCTL 风险门禁、功能矩阵门禁通过。
- hvm_ctl、KswordCLI 和包含 AMD 主菜单修正的 GUI Release 构建通过；GUI 语言门禁通过。GUI 保留两条已有宏重定义警告及部署工具的 dxcompiler/dxil、VCINSTALLDIR 提示，不能称为零警告构建。最终本机日志为 `tools/hvm_lab/build-gui.log`。

本机日志与候选在 `tools/hvm_lab/` 的忽略目录内，不入 Git。`collect_identity.py` 从 SYS 的 RSDS 与 PDB info stream 独立核对 GUID/Age，并记录 SYS/PDB/CLI 哈希；日志哈希不等于对应测例已通过。

## 签名与加载分开记录

仓库原自动测试证书验证遇到 `0x80096019`（basic constraints），不是单纯缺少信任。
隔离候选改用 `Sign-LabCandidate.ps1` 创建的代码签名证书，已写入候选 SYS；私钥不可导出。
该脚本不安装宿主信任。公开 CER 需仅在克隆中导入并验证；**来宾证书信任、内核签名验证、实际加载都未完成**。
常规 GUI 构建仍会执行仓库既有自动签名流程，其签名写入不能当作信任验证通过。

## 实验环境与缺失证据

宿主是 Windows 10 19045、Ryzen 9 8945HX、32 逻辑处理器，最近只读清点时 Hypervisor 运行、VBS=2。
Workstation 16.2.5 build-20904516 已确认。独立完整克隆固定硬件 19、单插槽、8 GiB、初始 4 vCPU；位置见本机 `host.local.json`。
克隆清除挂起 checkpoint 引用，原 VM 未启动或修改。克隆尚未启动，实际来宾 Windows 版本、干净关机快照均未确认。

以下全部未运行：

1. 管理员执行进入脚本 → 重启 → `LabHostReady` → 日常启动恢复实测。
2. VMware 原生模式日志（CPL0）、来宾 CPUID/MSR SVM/NPT/NRIP/ASID 证据。
3. 来宾测试签名与 KD 配置、普通驱动断点、匹配符号、内核转储链验证。
4. 1 vCPU 实际 SVM 往返及 20 次启停；2 vCPU 20 次；4/8 vCPU 各 100 次。
5. 8 vCPU 两小时压力、逐核失败注入与真实回滚、并发控制、卸载、电源生命周期。
6. 外部网络 I/O、Intel 硬件回归、多 Processor Group 硬件验证。

当前会话缺少提升后的管理员令牌，不能执行宿主 BCD 切换。下一步由用户在管理员 PowerShell 中运行进入脚本；重启后回到本任务继续。不要把脚本 `PendingReboot` 当成实验已可运行。

用户决定今天暂停硬件验证：本地提交后直接正常关机，不推送、不安排实验启动；明天继续上述验证。

## 与计划的差距

- 首版强制要求 NRIP，未实现固定指令编码回退；非零 XSS、活动 CET/LA57/PKS/UINTR、物理地址超过 48 位明确拒绝。
- Intel 保留原执行路径，后端调度边界首先接入 AMD，尚未把 Intel 全部动作改成统一 ops。
- NPT 缓存属性合成、GIF/IF/NMI、XSTATE、GS/描述符及原生返回链仅做静态实现与编译，必须优先实测；不能保证首轮不会崩溃。
- 未知退出保留诊断后走确定故障路径；尚未证明 host 故障时来宾 KD 或转储一定可用。
- metrics 导出最新一致记录；完整每核 64 条 ring 需 KD/转储。故障注入点是每 CPU 分配完成、进入前、进入后，非每次分配的独立注入。
- 压力脚本的 UDP 是本机环回，不替代虚拟网卡外部网络验证。

## 可行性边界

Windows 10 的 Hyper-V 外层不满足微软对 AMD 嵌套宿主的版本要求（需要 Windows 11 或 Server 2022+）：[微软文档](https://learn.microsoft.com/en-us/windows-server/virtualization/hyper-v/enable-nested-virtualization)。本项目不通过升级系统或更换电脑解决。

本期路线是停用本次启动的宿主 Hyper-V/VBS，让 VMware 原生 AMD-V 向 Windows 10 克隆暴露 SVM，再测试 KSword；这条路线尚待本机证据确认。
“KSword 常驻后再向内层 Hyper-V/VMware 提供 SVM”未实现，也不在本期验收范围内。
