# AMD 嵌套 SVM：第二阶段实现记录

## 当前边界（2026-09-18）

22:24 更新：1 vCPU/1 次受控硬件探针已通过，导出 30 文件独立核验。每核 entries=1/reflections=1/NPF=5，原始退出0x72、内层marker0x4B534E31，原生返回后完成序列2；teardown后资源归零。SYS/PDB age9匹配，来宾签名/加载及KD私有符号已确认。下文“尚待硬件”的表述记录接线时状态，以本段为最新结果。该自检按CPU串行运行，后续多CPU探针也不能替代并发内层OS验证。

上一阶段已本地提交 `4c6cd6a0`，未推送。1/2 核各 20 轮、4 核 100 轮启停已由回传原始日志独立核验。
用户暂停 8 核压力时，`stop` 返回 OK：generation 304→305，prepared=8、selfTestPassed=8、failed=0、resident=0、累计 VMEXIT=12496。
这证明该次全核停止成功；两小时压力是用户中止，不记为通过，也不据代次数量补记 8 核 100 轮验收。

本阶段目标是在 KSword 常驻后向内层 VMM 提供 SVM。现已接通受控探针的实际资源、汇编和 VMEXIT 分派：虚拟 EFER/HSAVE、VMLOAD/VMRUN/VMSAVE、稀疏 NPT02 缺页合成、CPUID 退出反射及原生返回。**代码与模拟测试完成，新的硬件往返尚待执行；还不能启动任意内层操作系统。**
现有 AMD 常驻入口仍隐藏 SVM。专用 `prepare-svm-probe` / `self-test-svm-nested` 仅允许驱动拥有的固定 VMCB/指令序列；探针准备配置禁止 resident。新候选使用独立目录，原 irqfix 候选保留。

文中 NPT12/NPT01/NPT02 的编号以 KSword 为参考层：内层 VMM 的映射、KSword 的映射、合成映射。
“host physical”是 KSword 所见物理地址；当 KSword 自身运行于 VMware 时，不表示已经取得物理宿主的真实机器地址。

## 已写入源码

| 模块 | 实际行为 |
| --- | --- |
| `hvm_svm_nested_npt.h` | AMD 四级页表遍历；4 KiB/2 MiB/1 GiB；MAXPHYADDR、保留位、Present/RW/US/NX 校验；PAT 索引解码；完整路径记录；比较交换更新 A/D；WB/UC 4 KiB 叶项合成 |
| `hvm_svm_nested_mmu.c/.h` | NPT12→NPT01 翻译，连 NPT12 的页表页也经 NPT01 转译；区分内层 NPF、外层页表 NPF、外层数据 NPF、物理读取失败；保留 EXITINFO1[33:32]；源 A/D 提交后才输出叶项 |
| `hvm_svm_nested_shadow.c/.h` | 使用预分配页创建稀疏 NPT02；最多每 CPU 256 页；预算不足不修改原树；重置推进 epoch；拒绝过期或混合来源的结果；全部修改标记需 TLB flush |
| `hvm_svm_nested_state.c/.h` | 分开搬运 VMRUN 与 VMLOAD 状态；反射 baseline VMEXIT 字段时保留 VMCB12 控制字段/物理指针；CR/DR/异常及两组杂项 intercept 分类，未知范围单独返回 |

新增 `hvm_svm_nested_resources.c` 在 PASSIVE_LEVEL 分配每核 64 页影子池、8 KiB VMCB/虚拟 HSAVE 操作数和私有栈；真实物理窗口回调仅允许保留的 RAM 清单内对齐八字节读取/CAS。`hvm_svm_nested_probe.c` 处理专用退出状态机，真实汇编探针执行虚拟 MSR/SVM 指令和内层标记。所有源码已加入工程和 filters，逐句注释、单文件小于 1000 行。

HVM v5 增加 SVM_NESTED_PROBE 控制标志（仅 prepare/self-test，Intel 拒绝）。metrics 独立升级为 v4，逐核记录 nestedProbe 的完成序列、状态、进入/反射/NPF 数和原始退出码。完成序列仅在真实返回并核对原 EFER/HSAVE 后变为有效偶数；旧 metrics v3 客户端明确拒绝。命令目录、help、双语词条与 CLI 文档同步。

## 调用约束，接线时不可省略

1. MMU 回调只接受已核验 RAM 的物理页，使用每 CPU `hvm_phys_window`；不能对任意 guest PA 做 WB 映射。Windows 适配器已接入，完整对齐八字节读、原子 compare-and-OR 均不在 root 路径分配或等待。
2. `Config.Epoch` 是证据标签，不会自动持锁或监视页表。调用者须持有同一 NPT01 生命周期/失效代次，在 VMEXIT 中解析和安装；页表/CPU 资源均预先分配。不能只把相同整数写进两个结构体就认为没有竞态。
3. 首个读映射刻意清 RW，使第一次写再次 NPF，经源叶项 D 更新后才可放开 RW。A/D 回写使用 CAS，不覆写并发 remap；失败输出 Leaf=0。部分已经设置的 A 位可以保留。
4. `FlushPending` 只是请求。下一次真实 VMRUN 必须设置 TLB_CONTROL 完整 flush；不能把软件 Reset 当作硬件失效。进入内层 VMRUN、虚拟 INVLPGA、NCR3/ASID/TLB 请求切换均需处理软件缓存失效。
5. 四级页表当前接受 32–48 位物理宽度。带 PWT/PCD 的 NCR3、五级页表及扩展保护语义尚未接入，须在入口明确拒绝，不能静默屏蔽后继续。
6. 叶项合成只实现 WB/UC 子集；这不等于完整三层 PAT/MTRR 虚拟化。正式入口还须检查内层 guest PAT、虚拟 MTRR/CD 语义；不能仅因当前某页使用 WB 就对内层 VMM 宣称全部缓存模式可用。
7. 状态搬运函数只操作已拥有的 VMCB 快照，不做 guest 物理读取或合法性审查。LBR/CET/SEV/AVIC/VGIF 等扩展不在这些 baseline 拷贝函数的支持范围。VMRUN 合法性校验、段 canonicalization、虚拟 host restore 和 GPR/RAX/RSP 特例必须在入口/退出状态机处理。
8. MSR/IOIO 原始 intercept 位只表示需要进一步查权限图，不等于直接反射。控制位、MSRPM/IOPM 合并和 ownership 分派尚未实现；未知退出不能盲目重入。

## 本轮验证

`tools/hvm_lab/build-tests.cmd` 使用 MSVC `/W4 /WX` 编译生产源码并执行：

- 新增嵌套逻辑 441 次断言通过：非身份页表/数据映射、大小页边界、权限/保留位、故障归属、A/D 竞争、缓存拒绝、预算回滚、过期代次、循环指针、状态字段所有权、intercept 和虚拟 MSR。
- 生产探针分派代码 124 次模拟退出检查通过，包括完整进入/NPF/反射/返回和错误操作数、错误标记、INVALID、NPF 失败的原生恢复路径。模拟不执行汇编或 SVM 指令。
- PS5.1 验收谓词 16 项通过；采集器真实子进程 300 次快速退出、双流大输出、空输出/非零退出/超时保留通过；CLI 实际 JSON 格式器与 CP936 管道、62 项命令目录和双语检查通过。
- 原有 AMD 73,907 次断言与 Intel 85 项检查通过。
- 标准 MSVC/WDK Release x64 编译链接、x64 ApiValidator（`Driver is 'Universal'.`）和目录生成通过，零警告、零错误。
- 标准构建跳过自动发行签名，候选单独进行实验签名；签名与加载分别记录。**新 SYS 尚无加载及硬件运行通过证据**。
- 主程序与 KswordCLI Release 编译链接通过。主程序有四条既有宏重定义/部署警告，自动 GUI 签名校验仍返回 0x80096019；不计为驱动候选签名结果。新驱动候选已用原实验签名证书签署并回读匹配，宿主未信任该实验根；来宾加载需独立确认。

本机原始日志：`tools/hvm_lab/artifacts/host-20260918/nested-probe-tests.log`、`tools/hvm_lab/build-nested-probe.log`。
测试回调使用模拟内存，不能证明真实页表写入、SVM 指令执行、IRQ/NMI、ASID/TLB 或内层虚拟机启动。

## 硬件入口和后续工作

在冷启动的 1 vCPU 调试克隆执行共享候选中的 `Start-GuestNestedProbe.ps1`。该入口要求原服务 STOPPED，验证 SYS/PDB/CLI 哈希后复制到新的独立目录，加载候选，再调用 `Invoke-GuestAcceptance.ps1 -NestedProbe -Vcpu 1 -Cycles 1`。逐核结果不完整即失败；超时不结束控制进程、不推断已回滚。运行日志和匹配身份清单自动回传 KSwordResults。先证明一个硬件往返，再增加轮次和 CPU。

当前探针的 NPT12 使用已拥有的身份页表，NPT02 从空树按真实 NPF 构造；非身份映射仅有逻辑测试。GIF 只在 IF=0 的固定短序列中记账，不声称支持任意 IRQ/NMI 嵌套。失败时恢复初始 Windows 快照仅适用于这个有界自检，不能用于正常运行的内层 VMM。下一步仍需：

1. 将已拥有固定 VMCB 的专用入口扩展为任意内层 VMCB 的快照/合法性检查，增加 MSRPM/IOPM 合并和实际 CPUID 能力发布门槛。
2. 将探针虚拟 MSR/VMLOAD/VMSAVE/STGI 扩展到一般入口，补齐 CLGI/INVLPGA、异常注入、IRQ/NMI/GIF 语义。HSAVE 是虚拟所有权，不能依赖真实硬件 HSAVE 格式，也不能写入真实 MSR让内层接管。
3. VMRUN 校验并保存 L1 continuation，构造 VMCB02，真实进入内层；VMEXIT 按 intercept 所有权反射或本地处理。保存/恢复 GIF/IF、CR2/DR、GPR、VMLOAD 状态；现有宿主 IF=0，不能再次错误设置 V_INTR_MASKING 导致 IRQ 饥饿。
4. 用可控的极小内层 VMRUN→CPUID 探针证明一次往返，再测非身份 NPT、内层异常/NPF/关机、全核 stop。通过后再向 VMware 内层系统开放，单核到多核推进。

架构依据：[AMD APM Volume 2](https://docs.amd.com/api/khub/documents/sD1_QL~h4Afq2_tvzxqqSQ/content)，重点 §15.5–15.7、§15.21、§15.25；可检索的 [AMD 原文镜像](https://www.scs.stanford.edu/~zyedidia/docs/x86/amd-manual.pdf)。未复制第三方 hypervisor 源代码。
