# HVM 4×2：多核、性能、HTTP 场景与匿名证据

本批次于 2026-09-15 至 2026-09-16 UTC 完成。Windows 1 配置为 **4 个 vCPU**，
TinyCore 配置为 **2 个 vCPU**。以下新结果均来自驱动 SHA256
`8850e28f7f43cf12f95116c862e0aa9fb4b609d60446ee1d46ddc1b900ab7a04`，
旧版本对照和历史十分钟观测另列，不合并成功率。
对应实现提交为 `b9bdda8358fc7278e79053f2cc4dbf2056a900b9`；
provenance 同时保存构建输入字节哈希、规范化文本哈希及 Git blob 身份。

## 1. 本轮交付

| 项目 | 结果 | 证据 |
| --- | --- | --- |
| 4×2 多核 | 20/20 换页恢复，15/15 故障控制试验，3/3 非法请求拒绝；每次要求双核整页读回和完整阶段记录 | [SMP 汇总](paper-data/20260915-4x2/smp/derived/smp-summary.json) |
| 性能优化 | 嵌套来宾复制耗时下降 94.53%，CPUID 耗时下降 29.05%；网络 RTT 仍有明显开销 | [嵌套前后表](paper-data/20260915-4x2/derived/nested-improvements.csv)、[Windows A/B](paper-data/20260915-4x2/after/derived/windows-overhead.csv) |
| 真实应用 | BusyBox httpd，3/3 完整故障注入/恢复试验、90 次 HTTP 响应；逐请求身份与 PFN 检查通过 | [HTTP 结果](paper-data/20260915-4x2/application-v2/derived/http-page-summary.json) |
| 匿名证据 | 1281 个文件，8,017,092 字节；脱敏后重算、扩展身份扫描、ZIP/manifest 校验通过，私有映射不在 ZIP 中 | [打包结果](paper-data/20260915-4x2/anonymous-package.json) |

这轮完成的是当前配置的实现、优化和证据闭环。CPUID/RTT 剩余成本、其他硬件、
中间 Hyper-V 和一般应用页原子性仍有边界，不能据此声称“所有性能问题已消除”。

## 2. 拓扑与环境

```text
Hyper-V
├── Root partition
│   └── Windows 0 · HVCI active
└── Guest partition
    └── KSword VMX monitor
        ├── Windows 1 · 4 vCPUs · same boot during measured insertion
        │   └── VMware · started after monitor insertion
        │       └── TinyCore · 2 vCPUs · normal /init
        │           ├── CPU 0 + CPU 1: reserved-page observers
        │           └── BusyBox httpd: real file-cache page
        └── Composed EPT
            ├── original backing → original response
            ├── private backing  → injected data fault
            └── original backing → restored response
```

| 字段 | 实际记录 |
| --- | --- |
| CPU | Intel Core i7-13700F，Family 6 Model 183 Stepping 1，16 核/24 逻辑处理器 |
| Root Windows | Windows 11 Pro for Workstations Insider，26300.9022，HVCI 运行中 |
| Hyper-V | `hvix64.exe` 10.0.26100.9022；`vmms.exe`/`vmwp.exe` 10.0.26100.8875；逐文件哈希已保存 |
| Windows 1 | Windows 11 Home 22621.4317，4 vCPU，固定 8 GiB RAM |
| 外层 VM | Generation 2，ExposeVirtualizationExtensions=true，Secure Boot 启用 |
| TinyCore | 17.1，Linux 6.18.35-tinycore64，online CPU `0-1` |
| BIOS | ASUS S501ME.331；固件菜单中的 VMX/VT-d 开关未直接观察 |
| VMware | Workstation 17.6.4 build-24832109；虚拟硬件版本 21；TinyCore 分配 768 MiB RAM |

来源：[外层环境](paper-data/20260915-4x2/outer-environment.json)、
[Hyper-V 配置](paper-data/20260915-4x2/platform-details.json)、
[Windows 1/VMware](paper-data/20260915-4x2/smp/environment.json)、
[构建身份](paper-data/20260915-4x2/build-provenance.json)。
Hyper-V 运行时 WMI 返回的 VMX=false 不等同于 BIOS 禁用了虚拟化。
最外层 HVCI 与 Windows 1 的 VBS 状态分别记录，不能互相代替。

配置从 2 vCPU 改为 4 vCPU 时重启了 HVM target，记录在
[topology-change.json](paper-data/20260915-4x2/topology-change.json)，属于实验准备。
“无重启插入”仅指之后的测量区间。没有声称运行中的 VMware/TinyCore 整条链已经平移。

## 3. 多核、回滚与进程生命周期

- Windows-only：当前版本 10 组进入/退出，四个 resident CPU 的身份、Windows boot、
  关键进程 PID/创建时间均保持连续。随后才启动 VMware 和 TinyCore。
- 保留 GPA `0x07000000`：空闲 10 次、双核负载 10 次换页恢复，20 次都具备完整阶段日志。
  两个来宾 CPU 分别固定亲和性，比较全部 4096 字节的 MD5；负载观测为 99.96%/98.77%。
- 写隔离：A5 原页 → D1 替换页 → 替换页前四字节写 B2 → 恢复整页 A5，双核均读回。
- 故障控制：分配失败、发布前取消、提交后回滚、提交 drain 失败、回收 drain 失败各 3 次。
  后两项在调用边界模拟失败，**没有注入真实硬件 INVEPT 故障**。
- 拒绝：超出 52 位 GPA、未对齐 GPA、未知 EPT 根各一次，均 clean reject。
- 新增进程所有权：映射绑定被引用的进程对象、PID 与创建时间。进程退出先撤销可用性，
  替换页保留到全部 CPU 完成失效后再释放，避免只用 EPT 根地址作为生命周期身份。

当前版本的活动映射退出试验：关闭 VMware 时，Windows boot 不变、4 核常驻仍在，
查询为 `active=0, retired=1, ownerExited=1`；显式 remove 后两个槽都为 0，
rule/replacement 分配释放计数配平，未观察到 Hyper-V 18560 重置事件。
[原始试验](paper-data/20260915-4x2/application/vmware-teardown-20260915T235335174Z.json) ·
[独立重算](paper-data/20260915-4x2/derived/lifecycle.json)。

支持的收尾顺序是：**保持 KSword 常驻 → 关闭 VMware → 回收映射 → 停止常驻**。
旧版本先停常驻再关闭 VMware 的试验发生 vmrun 超时，原始失败保留。
进程租约没有解决同一 VMM 进程内的 guest reboot、快照恢复或 GPA 重新分配；调用方仍须先 remove。
page-control 计数不覆盖全部驱动池分配，不能扩大为“整个驱动无泄漏”。

## 4. 性能：修复内容和实际剩余成本

实现更改：

1. EPT A/D（访问/脏位）记录改为按 shadow leaf 索引的完整账本，只遍历仍可能变化的记录，
   直接读取硬件维护的叶子，避免每次重新进行多级查表。满容量时拒绝继续，不能丢弃旧记录。
2. EPT12 表快照仅接受合法 A/D 0→1；清位、地址、权限或其他位变化仍失效。
   跟踪表页容量从每 CPU 32 页增至 160 页，避免工作集反复击穿缓存。
3. VM-exit 计数改为 CPU 本地单写者，查询时汇总；反射路径避免重复 VMREAD。
   CPUID leaf 0 按 CPU 缓存，动态 leaf 仍执行原查询。
4. 新增每 CPU shadow fill、drop、overflow、A/D pending 等度量；没有靠抽样或漏计来制造性能改善。

### 4.1 TinyCore：旧实现 → 新实现

两者均为 4×2、同一静态二进制、固定来宾 CPU 0；每工作负载预热 1 次、测量 3 次。
两组跨驱动版本和 guest boot，因此是实现比较，不能当作 no-KSword steady-state overhead。

| 工作负载 | 旧中位数 | 新中位数 | 耗时变化 |
| --- | ---: | ---: | ---: |
| CPU 整数计算 | 0.143797 s | 0.139019 s | −3.32% |
| CPUID | 12.564454 s | 8.914616 s | **−29.05%** |
| memcpy，累计复制 2 GiB | 6.342334 s | 0.346955 s | **−94.53%** |
| 指针追逐 | 3/3 进程超时 | 1.611810 s，3/3 完成 | 不计算倍率 |
| TCP loopback 批量传输 | 0.026392 s | 0.021538 s | −18.39% |
| TCP loopback RTT | 0.008702 s | 0.010263 s | **+17.94%** |

20 秒超时包括进程初始化，不能用 20/1.61 推导指针追逐加速比。RTT 的新版本点估计更慢，
样本数只有 3，需保留原始分布；不能只发表有利工作负载。
没有新增 TinyCore 持久磁盘 I/O 对照，RAM 文件系统或缓存 ISO 读取不能替代磁盘开销结果。

### 4.2 Windows 1：相对关闭常驻的开销

VMware 未运行，off/pre → on → off/post；每块 1 次预热、7 次测量。
on=7，off=14；下表为耗时变化，括号是同一机器/boot 内的 bootstrap 95% 描述区间。

| 工作负载 | 当前相对开销 |
| --- | ---: |
| CPU 整数计算 | −1.01%（−5.19% 至 +0.58%） |
| memcpy | −1.23%（−8.59% 至 +13.96%） |
| 指针追逐 | −0.69%（−1.75% 至 +2.76%） |
| 虚拟磁盘 direct read | +3.26%（−3.49% 至 +10.12%） |
| 虚拟磁盘 direct write | −5.55%（−22.38% 至 +11.25%） |
| TCP loopback 批量传输 | −1.89%（−11.80% 至 +18.51%） |
| TCP loopback RTT | **+34.82%**（+29.95% 至 +37.65%） |
| CPUID leaf 0 | **9.05×**（额外 +804.95%） |

旧实现同样 4 核的 CPUID 相对耗时是 10.61×，新实现降到 9.05×，仍明显偏高。
区间跨 0 的项目不能宣称已证明加速或零开销。TCP 批量传输 off/post 相对 off/pre 漂移 −17.23%，
需要在下一阶段使用更多独立 boot 和交错实验控制漂移。一次与宿主编译重叠的采集完整保留，
明确排除出主性能统计。

### 4.3 插入时间与资源代价

当前 10 次插入：主体中位数 **2.8005 ms**，资源准备 **2.65375 ms**，
IPI rendezvous 包络 **149.5 µs**，进入返回时间 skew 中位数 **4.6 µs**、最大 **28.3 µs**。
旧 4 核主体为 1.7239 ms、包络 152.5 µs。较大的元数据预分配使主体变长。
新版本每 active CPU 的 shadow 表、快照与 A/D 账本容量合计 3.25 MiB，4 CPU 为 13 MiB；
这是相关预留结构的计算容量，不是全部驱动实测占用。

40 个 CPU-entry 记录的中位数：状态捕获/控制选择 13.85 µs，VMCS 编程 5.0 µs，
entry continuation 24.1 µs。冷 EPT 建表单次 6.0918 ms，发生在 prepare 阶段。
并行区间不可相加；quiesce 用 IPI 到达/离开边界描述，resume 包含汇编连续段，
这些数字不等于直接测量到的应用暂停，也不证明四核绝对同时进入新拓扑。
[逐 CPU 数据](paper-data/20260915-4x2/after/derived/internal-transition-cpus.csv)。

### 4.4 退出与失效成本

嵌套 benchmark 观测区间内记录 19,489,348 次 resident dispatch，约 167,733 次/秒；
VMREAD、HLT、VMWRITE、EPT violation、CPUID 分别约 636 万、564 万、265 万、255 万、82 万。
实际 INVEPT wrapper 调用 101,233 次，约 874.4 次/秒，失败 0。
状态与 metrics 查询各有自己的区间，不能混用两个分母。
[原始/差分](paper-data/20260915-4x2/after-nested/counter-interval.json)。
该窗口含控制器和启动/采集工作，不是隔离后的纯稳态计数，也不包含最外层 Hyper-V 的全部退出。
shadow exhaustion、A/D overflow、verify mismatch 均为 0；A/D 实际新增位 OR 计数也为 0，
因此不能把此批次当作所有 A/D 写回语义的运行时覆盖。比较策略另有逐位单元验证。
未取得实际硬件 INVVPID 总次数，nested reason 53 仅是被截获的指令类别。

## 5. 真实应用：可逆 HTTP 数据损坏注入

一个未经代码修改的 BusyBox httpd 服务 4096 字节 JSON 文件。小型 root 辅助程序通过
mlock/pagemap 定位并持续核对实际文件缓存页；客户端实际发送 HTTP 请求并记录响应长度和 MD5。
控制器在 Windows 1 内运行，通过 KSword 替换该 GPA，随后恢复原 backing。

- GPA：`0x2ed5f000`；Windows 1 可见原 backing：`0x38027000`；replacement：`0x204fd4000`。
- 原始/恢复响应 MD5：`53398dd1f3a4c282ace3b660acd92ff7`；零页响应：`620f0b67a91f7f74151bc5be745b7110`。
- 3 次试验各含 before/mapped/restored 10 次响应，共 90 次；全部 HTTP 传输成功、长度 4096。
- 每次请求重新读取 httpd PID/启动 ticks、guest boot ID；holder 存活且 PFN 未变，
  最旧 PFN 样本约 1.882 秒。Windows boot、VMware PID/创建时间、可执行文件与 VMX 配置哈希连续。
- 每次恢复后的 active/retired 都为 0，rule/replacement 分配与释放配平。

这是用实际服务输出验证可逆数据故障注入的场景。客户端没有验证 JSON 业务语义，
因此不能称为服务容错能力评测；也没有证明无需来宾辅助、任意工作负载页的原子替换或生产适用性。
旧观察器保存的启动身份不能当成逐请求连续性；旧试验、缺失 PFN 尾行和超时均单独保留。

测量区间中的控制脚本仅调用 Windows 本地查询、读取串口、发送 KSword IOCTL，没有执行
vmrun/Hyper-V 管理命令、注入或代码 hook。准备阶段使用 PS Direct、VNC 和 ISO，Hyper-V 始终在执行底层虚拟化；
EPT12 A/D 维护会写中间 VMM 所拥有的翻译元数据。故不能写成“整个实验从未调用 Hyper-V”或“未修改任何 VMM 内存”。

## 6. 对原始证据要求的逐项对应

| 要求 | 本轮状态 / 数据位置 |
| --- | --- |
| 环境、版本、commit、BIOS | environment、platform-details、build-provenance；固件菜单开关未知明确标注 |
| boot/PID/vCPU 连续性 | Windows transition、SMP before/after、HTTP v2；配置重启和 guest 重新引导独立列明 |
| 平移总暂停与阶段 | QPC 内部边界和逐 CPU CSV 完整；应用可见暂停仍未直接测得 |
| EPT 换页闭环 | GPA/backing、整页 hash、map/remove 时间、完整操作事件、实际 INVEPT 计数 |
| 跨层路径 | 共享引擎/驱动路径和本地应用控制器；二进制/config 身份核对；编排范围单列 |
| 回滚和资源 | 单映射 slot + retired slot + rule/replacement ledger；全部驱动池泄漏未覆盖 |
| 逐次可靠性 | JSON 逐次落盘、失败/不完整记录保留；新旧版本和不同证据等级不混用 |
| 故障注入与高负载 | 5×3 fault、3 rejection、双核高负载 10 次，真实硬件失效仍未覆盖 |
| CPU/内存/磁盘/网络 | Windows A/B 完整；nested CPU/内存/loopback 完整；nested 持久磁盘及匹配 no-monitor 基线仍缺 |
| 虚拟化开销 | 每 CPU/all-dispatch、nested 类别、EPT/INVEPT；外层全局和实际 INVVPID 不可得 |
| 多核时间/一致性 | 四核阶段/identity/skew，双核新鲜读回；不声称瞬间一致或任意读写原子性 |
| 普适性矩阵 | VMware/TinyCore 4×2 已测；中间 Hyper-V 未测；Win10 LTSC 暂缓；其他机器由作者补测 |
| 长期稳定性 | 不补测；保留旧 2×2 的 599.43 秒/21 采样点，不能推广到当前 4×2 |
| 原始证据 | JSON/JSONL、原始串口、CSV、身份与哈希，匿名包可离线重算 |
| 更多 | 进程租约撤销；分析器拒绝伪连续/过期 PFN；宿主编译干扰试验显式排除；GUI/CLI 同一参数契约 |

## 7. 构建、界面与交付边界

本地 [匿名证据 ZIP](../../dist/hvm-anonymous-20260915-4x2-r2/anonymous-evidence.zip)
SHA256：`05c01664512348ce731424ab6b808edaa66a343911f00ce17034697155fe5eea`。
解压后运行 `python evidence/reproduce.py`。包内 identity SHA 是一致别名；
`manifest.json` 才是脱敏文件的真实 SHA256。公开项目与硬件/日期组合仍可能被关联，
这份包只证明直接身份字段已脱敏，不保证不可追溯，也不是完整可构建源码 artifact。
没有上传该 ZIP 或执行投稿。

驱动标准 MSVC/WDK 构建通过，无警告，Universal/Inf2Cat 校验通过；禁用了自动变体签名，
只使用现有普通测试证书。主程序、KswordCLI、hvm_ctl 均重建并部署，page/metrics ABI 更新为 v2。
主程序内嵌 HVM 页面仍与 CLI 共用目录和执行引擎，新增 owner PID 参数同步进入同一入口；
59 个命令的目录/翻译审计、26 个有效及 20 个拒绝参数用例通过。**没有运行 GUI 测试。**
保留了用户既有 `theme.h` 改动，不提交它；主程序构建时包含该工作区输入，provenance 单独注明。

收尾后 Windows target 仍运行、四核配置保留，VMware 双核配置保留；VMware 和 KSword 常驻已停止，
资源已回收。没有新增长期观测，也没有把实验过程中经过的墙钟时间冒充稳定性测试。
