# AMD 实验后端与重启续接

2026-09-18 加载入口补充：PS5.1 `powershell.exe -File` 下 param 默认表达式的 `$PSScriptRoot` 为空，正文中才有值；同一文件用 `-Command &` 则默认值正常，已用最小文件复现。Load-GuestCandidate 改为正文初始化来源目录；测试新增 3 项真实 PS5 进程入口（默认 -File、显式目录、调用运算符），另 7 项模拟 SCM 全通过。重试时 CLI 哈希相同不重复覆盖，避免刚退出的进程仍短暂持有文件时 Copy-Item 失败。当前仍等待来宾新版加载脚本实测，不能记作 VMRUN 通过。

最新进度（2026-09-18，优先于下方逐次历史）：宿主 LabHostReady；克隆 Win10 Home 19042/1 vCPU，初始化并重启，KD 已连接（普通驱动断点/匹配符号加载/受控转储尚未验证）。用户的加载输出已证明签名信任、SCM RUNNING 和 AMD status IOCTL 成功：backend=2、SVM/NPT/NRIP、ASID=64、PA=45、msrValidMask=15。未 prepare 或 VMRUN。当前阻塞是 UTF-8 中文 `没有拒绝过` 经 PS 5.1 代码页 936 解码吞掉后面的引号，已复现；不是驱动或原始 printf 丢引号。共享 JSON 打印器现在输出 ASCII Unicode escape；真实 query 格式化 + PS5/936 回归通过。加载脚本现在可重试同路径运行中候选，更新 CLI/identity 前比较 SYS/PDB 哈希，拒绝其它活动路径；7 项模拟 SCM/更新测试通过。PS5 Get-Content 的字符串 provider 属性会被 ConvertTo-Json 深度遍历，证据字符串改用 File.ReadAllText。新 CLI/加载脚本待共享目录来宾实测；不需要重启/卸载现有候选。用户已授权切回 Astra 修改代码并继续；保留 Win10、不换电脑、不推送。不得重复执行昨晚已经完成的关机授权。

2026-09-17 增加待硬件验收的 SVM/NPT 实验实现。**第一次真实 VMRUN 尚未运行，不能宣称 AMD 或多核已支持成功。**

- 用户底线：Windows 10、不换电脑、现有 Workstation 16.2.5。只验 VMware 暴露 SVM 环境下的 KSword 常驻，不提供内层 SVM。
- 实现入口 `hvm_backend.*`，AMD 模块 `hvm_svm_*`、`hvm_npt.c`；Intel 原执行路径保留。主协议 v5、metrics v3，定义只在 `shared/driver/`。
- 能力位不代表运行证据；MSR 有独立有效位。VMware 必须显式 ALLOW_NESTED；未知外层拒绝。NRIP 强制要求。
- 准备、自检与启动绑定 power generation、group:number/全局 CPU index 集合；共享 phase/unload 保护。NativeReturnSeen 防止恢复校验失败后在已退出 CPU 再发 VMMCALL。
- Intel ASM 已依赖 runtime/CPU 前缀偏移，新增字段置于尾部，不改变既有 offsets。SVM ASM 前缀字段逐项 C_ASSERT。
- root 路径预分配、每核 ring；metrics 只导出最新一致记录，完整 ring 从 KD 读取。64 位 AMD exit 不进入 Intel 96 项数组。
- NPT 完整 CPUID 范围，RAM WB/空洞 UC，先遍历精确计费，64 MiB 上限；>48 位明确拒绝。缓存合成与 native-return 仍需硬件验证。
- 两个宿主公开入口在 `tools/hvm_lab/`。首次基线/BCD 导出/所有权在 ProgramData；默认启动项不改，只设置一次性实验启动。-Check/-NoRestart，必须管理员。PendingReboot 不等于 LabHostReady，更不等于来宾 SVM 可用。
- 独立冷启动克隆已建立，路径在忽略的 `host.local.json`；尚未启动，guestOS 与干净关机快照未核验。不得恢复源 VM 挂起内存或修改源 VM。
- 仓库原自动证书出现 0x80096019 basic constraints。AMD 隔离候选使用 Sign-LabCandidate.ps1 的独立代码签名证书；仅签名写入已验证，克隆信任/驱动加载未验证，不给宿主导入该信任。
- 构建/逻辑检查与硬件进度见 `docs/next/ksword-amd-lab-status.md`。本机日志、SYS/PDB/CLI 与 identity.json 在 tools/hvm_lab 忽略目录，identity 脚本独立匹配 RSDS/PDB GUID/Age。
- 下一步：用户管理员运行 Enter-AmdLab -NoRestart，PendingReboot 后正常重启，-Check 确认 LabHostReady，再验证 VMware CPL0、来宾 Win10/SVM、KD/转储、单核、2/4/8 核与回滚。当前 agent 令牌未提升，不能自己改 BCD。
- 用户最新决定：今天不验证、不切换启动环境；收尾必要构建后创建本地 commit，不推送，然后执行 `shutdown -s -t 0`。明天再测试；没有安排自动启动实验或定时任务。
- 2026-09-18 首次管理员执行 Enter 报 `ManagementBaseObject` 无 `OpenObject`，发生在首次环境读取、任何 BCD 写入之前；本机尚无基线目录。已修复 OpenStore/OpenObject 的嵌入输出：按 FilePath 或 Id+StoreFilePath 键重新绑定 root/WMI ManagementObject，不依赖可能为空的 __PATH。Test-BcdBinding 用真实嵌入类型及 WMI schema 通过 8 项检查；策略 12/事务 21 项继续通过。实际管理员执行仍需用户重试，尚未进入 LabHostReady；不要重新关机，昨晚关机授权已执行完毕。
- 2026-09-18 后续：用户重试成功并重启，-Check=LabHostReady；agent 独立读取 HypervisorPresent=false/VBS=0，boot ID 2026-09-18T10:57:28.5000000Z。证据保存 tools/hvm_lab/artifacts/host-20260918。日常恢复方向尚未测试。
- 克隆已冷启动，按首轮测试设为 1 vCPU/8 GiB，原 VM 未动。vmware.log 明确 CPL0，加载 hv-svm.vmm/gphys-npt.vmm；Tools running，IP 192.168.172.130。Tools runtime guestInfo 报 Windows 10 Home 19042.631，尚需来宾内核实。
- 用户已确认克隆进入桌面、没有密码，后续不必等待登录确认。vmrun 匿名 captureScreen 被拒绝（需要 LoginInGuest），未尝试凭据登录；不得绕过登录或把凭据放命令行。可用 VIX SDK 在进程内安全登录已知账户，用户名还未获取。网络 PSSession 尚未建立。
- 克隆已挂只读 VMware 共享目录 KSwordLab -> tools/hvm_lab/artifacts/guest-bootstrap，包含 Prepare-Guest.ps1 与 lab-ownership.json；来宾路径 \\vmware-host\Shared Folders\KSwordLab。还没执行初始化、加载驱动、连接 KD 或 VMRUN。Get-KswordVmwareEvidence 的 $matches 与 PowerShell 自动变量冲突已修复为 $logLines，JSON 导出通过。
- 用户希望改用 GPT-5.6-Terra 继续测试以节省 token；只做必要检查。继续原测试目标，绝不将 CPL0 或 Tools 报告当成 KSword 硬件常驻通过。
- 2026-09-18 当前：hvm_lab/artifacts/guest-bootstrap 已包含签名候选、PDB、hvm_ctl、验收/负载脚本、manifest，以及新增 Bootstrap-GuestLab.ps1。后者 Inspect 只读核验克隆；Configure 必须来宾管理员且 Secure Boot=false，复制到 C:\KSwordLab\candidate，hash 核验、只导入克隆证书，再调用 Prepare-Guest Configure。Tools 对空密码 VIX 远程操作返回“不支持空密码”，故不能从宿主自动执行。用户需在已登录克隆的管理员 PowerShell 运行 \\vmware-host\Shared Folders\KSwordLab\Bootstrap-GuestLab.ps1 -Mode Inspect 并报告输出；不要直接 Configure，先确认 secureBoot。
