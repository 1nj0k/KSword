# HVM GUI 与命令行一致性

## GUI 入口

- 主程序 **虚拟化 (KVM) → 完整操作** 子页直接显示全部操作。
- 标题栏 KVM 右键菜单 → **KVM 完整命令面板**，跳转到同一子页。
- 内核 HVM 页工具栏 → **KVM 完整命令面板**，跳转到同一子页。

操作区是主窗口内的 `QWidget`，由 `KvmDock` 的子页承载。打开入口不会另建对话框。
左侧按用途列出全部操作，支持按名称或 CLI 命令名搜索。右侧显示参数表单、
操作说明、参数校验、执行结果与完整输出的复制/保存入口。
TinyCore 换页表单可查询并选择当前 EPT12 根，支持安装与撤销单页替换。

本面板覆盖 `hvm_ctl` 全部命令；`KswordCLI r0 hvm-status`、`hvm-platform`、
`hvm-events` 分别对应状态、平台探针和事件操作，事件表单也提供起始序号与最多事件数。

## 调用链

```text
GUI 表单 ──→ HvmCommandProcess ──→ 本程序 --ksword-hvm-command ─┐
                                                            ├→ HvmCommandEngine → 共享驱动协议
hvm_ctl.exe ────────────────────────────────────────────────┘
```

命令目录、参数定义、参数校验、请求构造和结果处理均由同一份 C 代码提供。
GUI 启动自身的无界面执行入口，参数通过独立参数数组传入，不经过 shell，
也不依赖 PATH 或旁置 hvm_ctl.exe。该入口在主程序防多开、自动提权和驱动自动加载之前处理。
所有设备访问位于 ArkDriverClient。

`HvmCommandCatalog.c` 是命令与表单的共同来源；`help` 与 `commands` 也由目录生成。
原有 GUI 生命周期按钮和命令引擎共用 `KswordArkHvmBuildControlRequest` 构造请求。
GUI 快捷按钮仍采用菜单选择的设置；完整命令面板按所选 CLI 动词的固定参数执行。

修复的调用差异：

- `gdt-dump`、`msr-log` 不再从固定 argv[2] 读取参数，`--json` 不影响参数位置。
- 数值统一校验进制、位宽、完整输入与页对齐；拒绝溢出、负数、垃圾后缀及额外参数。
- `resident-vmreadbench` 默认次数明确为 512，时长默认 1000 毫秒。
- DLL 路径由 Windows Unicode 参数统一转换为 UTF-8，再转换为驱动的 UTF-16；支持空格与中文。
- 生命周期修改前读取当前代次；GUI 快捷路径在准备后核对实际 EPT 后端武装位。
- 内核页与标题栏快捷入口采用相同的 #VE、VMFUNC 和 Hypervisor 隐藏设置。
- 退出码 0、1、2、3 分别显示完成、传输失败、拒绝/无效、未实际验证。异常退出单独显示。

## 验证方法

参数校验不打开驱动，可在开发机运行：

```powershell
.\tools\hvm_ctl\hvm_ctl.exe --json commands
.\tools\hvm_ctl\hvm_ctl.exe --json --validate nested-page-map 1234501e 7000000 d1
python tools/hvm_ctl/test_command_parity.py
```

曾经还有一条 GUI 表单覆盖测试：主程序上挂 `--ksword-hvm-gui-test <报告路径>` 参数，
起一个真实 `MainWindow`，逐个树项填参数并点「仅校验参数」，把结果写成 JSON 和截图。
**2026-09-16 已整条删除**——它是塞在产品 `main()` 里的自检开关，产品的入口不是测试入口；
而且它自己那个用途也没兑现：每条命令等最多 30 秒，整轮跑不完，实测 180 秒不返回只能强杀。
`--gui-report` 这个入口随之作废。

`tools/hvm_ctl/test_command_parity.py` 的 CLI 侧校验不受影响，仍是上面那三条命令。

回归检查把每一组参数交给独立 CLI，比较全部返回值。
此外验证已知进制/参数位置回归，以及缺参、溢出、未对齐等拒绝路径。
真实 TinyCore 内存效果的边界与日志见 [单页 EPT 控制](nested-ept-page-control.md)。

## 2026-09-15 验证结果与边界

- 主程序 Release 构建成功，退出码 0；全局语言包审计通过，
  命令目录构建门通过（56 个命令、135 条目录文本）。
- CLI 参数回归通过 22 组有效输入与 18 组拒绝输入检查，包括 64 位地址、默认值、
  控制标志和带中文空格的 DLL 路径。初版对话框的 56 项真实表单与 CLI 比对已通过；
  随后操作区改为主程序内嵌子页，用户明确要求不再测试 GUI，内嵌版没有运行 GUI 覆盖测试。
- 自动化入口已改为真实 `MainWindow`，报告要求 `embedded=true`、`hostClass=MainWindow`；
  这是后续可用的验证入口，不是本次已通过的结果。初版对话框截图不作为内嵌版界面证据。
- [部署记录](logs/hvm-gui-deploy-20260915.txt)记录最终主程序产物。
  [TinyCore 实测](logs/hvm-gui-live-effect-20260915.txt)在内嵌改动前使用主程序的内置执行入口，
  在 EPT12 `0x1CAC605E` 的专用 GPA `0x07000000` 上完成
  `A5 → 换页读到 D1 → 影子页写入 B2 → 撤销读回 A5`。
  操作前后 Windows 启动时间和 VMware PID 均未变，两核常驻始终在运行。
  结束时 `active=0`、`retired=0`，原页恢复。

内嵌改动保留同一命令目录、参数校验与执行引擎；本次没有逐项执行所有会修改
驱动或进程的诊断命令。TinyCore 换页的内存效果已单独完成实机验证。
当前范围仍为一页 4 KiB WB RAM；诊断 shell 的引导配置及既有生命周期问题见上述单页控制文档。

## 完整命令目录

2026-09-16 增量：共享目录现有 60 个命令；参数回归通过 28 组有效输入、
20 组拒绝输入。主程序、KswordCLI 和 hvm_ctl 已重建，语言包审计通过。
新增 `resident-nested-fullsnapshot` 用于同二进制性能对照；新增页租约字段和
能力查询拒绝 JSON 由同一执行引擎输出。遵照用户要求，本轮没有运行 GUI 测试。
下表的早期验证数字不代表当前内嵌 GUI 的逐项实测结果。

| 分类 | GUI 操作 | CLI 命令 | 参数 |
| --- | --- | --- | --- |
| 查询与观测 | 运行状态 | `status` | 无 |
| 查询与观测 | 虚拟化测量 | `metrics` | 无 |
| 查询与观测 | CPUID 可见性 | `cpuid-view` | 无 |
| 查询与观测 | 平台探针 | `probe-platform` | 无 |
| 查询与观测 | 使用前自检 | `selfcheck` | 无 |
| 查询与观测 | GDT 快照 | `gdt-dump` | 处理器编号（十进制）（十进制 32 位，默认 `0`） |
| 查询与观测 | 事件记录 | `events` | 起始事件序号（十进制）（十进制 64 位，默认 `0`）；最多事件数（十进制 32 位，默认 `64`） |
| 查询与观测 | EPT 叶项 | `ept-leaf` | 物理地址（十六进制）（十六进制 64 位，默认 `0`） |
| 生命周期 | 准备资源 | `prepare` | 无 |
| 生命周期 | 准备 EPTP 切换后端 | `prepare-eptpsw` | 无 |
| 生命周期 | 准备每核私有 EPT | `prepare-localept` | 无 |
| 生命周期 | 处理器 VMX 自检 | `self-test` | 无 |
| 生命周期 | 启动常驻 | `resident` | 无 |
| 生命周期 | 启动嵌套常驻 | `resident-nested` | 无 |
| 生命周期 | 启动嵌套常驻并隐藏身份 | `resident-nested-hidehv` | 无 |
| 生命周期 | 完整退出快照对照 | `resident-nested-fullsnapshot` | 无 |
| 生命周期 | VMREAD 开销测量 | `resident-vmreadbench` | VMREAD 次数（十进制）（十进制 32 位，默认 `512`） |
| 生命周期 | 启动常驻并记录退出 | `resident-trace` | 无 |
| 生命周期 | 有界常驻自检 | `soak` | 保持时长（毫秒）（十进制 32 位，默认 `1000`） |
| 生命周期 | 停止常驻 | `stop` | 无 |
| 生命周期 | 释放资源 | `teardown` | 无 |
| 生命周期 | 重置故障 | `reset-fault` | 无 |
| 嵌套与诊断 | 一次性测试来宾 | `launch-test-guest` | 无 |
| 嵌套与诊断 | 嵌套能力校验 | `validate-nested` | 无 |
| 嵌套与诊断 | 单核嵌套探针 | `nested-probe` | 无 |
| 嵌套与诊断 | 全核嵌套探针 | `nested-probe-all` | 无 |
| 嵌套与诊断 | EPT A/D 拒绝探针 | `nested-ad` | 无 |
| 嵌套与诊断 | 单核自虚拟化探针 | `nested-selfvirt` | 无 |
| 嵌套与诊断 | 全核自虚拟化探针 | `nested-selfvirt-all` | 无 |
| 嵌套与诊断 | 设备访问位探针 | `acl-probe` | 无 |
| 嵌套与诊断 | 控制标志拒绝探针 | `probe-flags` | 无 |
| 嵌套与诊断 | 仅执行权限探针 | `probe-xonly` | 无 |
| 嵌套与诊断 | 单次放行规则探针 | `rule-allowonce` | 无 |
| 嵌套与诊断 | 跨核 TLB 探针 | `tlb-probe` | 保持时长（毫秒）（十进制 32 位，默认 `1000`） |
| 嵌套与诊断 | 强制退出 TLB 探针 | `tlb-probe-exit` | 保持时长（毫秒）（十进制 32 位，默认 `1000`） |
| EPT 视图 | 查询分离视图 | `view-query` | 无 |
| EPT 视图 | 视图安装探针 | `view-probe` | 无 |
| EPT 视图 | 视图效果验证 | `view-effect` | 无 |
| EPT 视图 | 验证现有视图 | `view-verify` | 无 |
| TinyCore 换页 | 查询嵌套页映射 | `nested-page-query` | 无 |
| TinyCore 换页 | 替换 TinyCore 物理页 | `nested-page-map` | EPT12 指针（十六进制 64 位，必填）；来宾物理页（十六进制，4 KiB 对齐，必填）；影子页填充值（十六进制 00–FF，必填）；VMM 进程 PID（十进制，默认 0 自动选择 VMware） |
| TinyCore 换页 | 撤销 TinyCore 换页 | `nested-page-remove` | 无 |
| TinyCore 换页 | 换页故障注入 | `nested-page-map-test` | EPT12 指针；来宾物理页；影子页填充值；故障阶段（十进制 1–4），均必填 |
| TinyCore 换页 | 撤销失效故障注入 | `nested-page-remove-test` | 无 |
| 寄存器策略 | 记录指定 MSR | `msr-log` | MSR 编号（十六进制）（十六进制 32 位，默认 `10`） |
| 寄存器策略 | 清空 MSR 策略 | `msr-clear` | 无 |
| 寄存器策略 | 开启 CR3 追踪 | `cr-track-cr3-on` | 无 |
| 寄存器策略 | 关闭 CR3 追踪 | `cr-track-cr3-off` | 无 |
| R-1 进程 | 查询注入 | `inject-query` | 无 |
| R-1 进程 | 标记写入测试 | `inject-test` | 进程 PID（十进制）（十进制 32 位，必填）；执行靶页地址（十六进制）（十六进制 64 位，必填）；标记地址（十六进制）（十六进制 64 位，必填）；写入值（十六进制）（十六进制 32 位，默认 `4B535744`） |
| R-1 进程 | 注入 DLL | `inject-dll` | 进程 PID（十进制）（十进制 32 位，必填）；执行靶页地址（十六进制）（十六进制 64 位，必填）；LoadLibraryW 地址（十六进制，0 自动解析）（十六进制 64 位，必填）；DLL 完整路径（Unicode 路径，必填） |
| R-1 进程 | 撤销进程注入 | `inject-release` | 进程 PID（十进制）（十进制 32 位，必填） |
| R-1 进程 | 撤销全部注入 | `inject-release-all` | 无 |
| R-1 进程 | 查询进程处置 | `proc-query` | 无 |
| R-1 进程 | 冻结进程 | `proc-freeze` | 进程 PID（十进制）（十进制 32 位，必填）；执行靶页地址（十六进制）（十六进制 64 位，必填） |
| R-1 进程 | 结束进程 | `proc-terminate` | 进程 PID（十进制）（十进制 32 位，必填）；执行靶页地址（十六进制）（十六进制 64 位，必填） |
| R-1 进程 | 撤销进程处置 | `proc-release` | 进程 PID（十进制）（十进制 32 位，必填） |
| R-1 进程 | 清空进程处置 | `proc-release-all` | 无 |
| 查询与观测 | 命令帮助 | `help` | 无 |
| 查询与观测 | 命令目录 | `commands` | 无 |
