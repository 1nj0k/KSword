# AMD 实验后端与重启续接

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
