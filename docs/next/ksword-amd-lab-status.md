# AMD 实验实现状态（2026-09-18，单核自检前）

## 当前进度（以下本节优先于后面的历史记录）

- 宿主实验启动成功，HypervisorPresent=false、VBS=0；日常恢复方向尚未测试。
- 独立 VMware 克隆为 Windows 10 家庭中文版 19042、1 vCPU/8 GiB、Secure Boot=false。CPL0、SVM/NPT 外层日志已保存；原 VM 未修改。
- 来宾初始化完成并重启；WinDbg 已连接内核。普通驱动断点、匹配符号的实际加载和受控转储链仍待验证，连接成功不替代这些证据。
- 用户实测候选加载脚本到达最后 JSON 解析处：签名信任、SCM RUNNING 检查及状态 IOCTL 已成功；AMD 响应 backend=2、slatType=2、ASID=64、physicalBits=45、msrValidMask=15、VM_CR=8、EFER=0x4D01、HSAVE=0。状态仅 INITIALIZED，未 prepare/self-test/resident。
- 阻塞根因已在宿主复现：合法 UTF-8 `没有拒绝过` 经过代码页 936 解码后，末尾 UTF-8 字节和 ASCII 引号一起变坏，JSON 解析失败；437 只乱码，65001 正常。不能归因为驱动返回了损坏的 JSON（JSON 在用户态生成）。
- 修复：共享 JSON 字符串打印器转义为 ASCII 的 Unicode escape，保留原字段及中文语义；query 使用该打印器。CLI Release /W4 /WX 构建通过；真实 query 格式化代码的模拟响应通过 PS 5.1/936 管道，含特殊字符、非 BMP 与非法 UTF-8 测例；命令一致性 60 命令通过。
- 加载脚本支持正在运行且规范化路径精确匹配的候选，不 stop/config/start；从只读共享目录更新 CLI/identity 前核对原 SYS/PDB 哈希，绝不覆盖驱动。日志分开保留 stderr；修复 PS 5.1 provider 属性递归序列化造成的耗时。7 项模拟服务/更新测试通过，修复后仍需来宾实际重跑。
- 下一步在已登录克隆运行共享目录的 Load-GuestCandidate.ps1。确认 status 可解析后，先完成 KD/转储证据，再执行单核 prepare/self-test；不能跳到多核压力。

## 2026-09-17 至 09-18 早期记录（不是当前状态）

目前是可编译的实验候选，**尚未执行第一次来宾 VMRUN，未证明单核或多核常驻可用**。
本文件用于重启后续接，不能用作硬件通过报告。源代码尚未提交；候选清单绑定基准提交与修改文件哈希。

2026-09-18 续记：上述实现已提交为 `d6218fd7`，未推送。首次管理员运行环境脚本在只读 OpenObject 处发现 WMI 嵌入对象缺少实例方法，尚未执行 BCD 修改。已补充显式实例绑定及 8 项真实 System.Management 类型/schema 回归；策略 12 项和模拟事务 21 项继续通过。等待管理员重试，宿主往返和 AMD 硬件测试仍未通过。

同日实验启动成功：脚本核验 `LabHostReady`，独立读取的宿主状态为 HypervisorPresent=false、VBS=0。独立克隆以 1 vCPU/8 GiB 冷启动；VMware 日志为 `Monitor Mode: CPL0`，并报告 hv-svm/gphys-npt、SVM/NPT/NRIP 能力，Tools 运行。该结果证明外层实验条件，**不证明 KSword 曾执行 VMRUN**。来宾 Tools 不允许使用空密码的远程操作，因此已将候选与受限 Bootstrap-GuestLab.ps1 放入只读共享目录；来宾预检、Secure Boot 处理、KD、候选加载与第一次 SVM 自检尚未执行。

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
