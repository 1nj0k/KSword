# 进程注入痕迹检查（issue #196 第一阶段）

## 分层

- `shared/evidence/InjectionSurvey.{h,cpp}`：唯一判据层，C++20、Qt-free、Win32-free。
  地址空间索引、模块交叉视图（加载器 L / 映像映射 I / 非映像载荷候选 P）、工作集页筛选、
  线程起点落点、比较范围计划、例外规则准入与匹配、观测语义表、四态结论。
  离线测试 `KswordARKLightTests/InjectionSurveyTests.cpp`（套件名 `J injection survey`）。
- `Ksword5.1/Ksword5.1/ksword/process/injection_trace_collector.{h,cpp}`：Win32 现场采集。
  只读、不挂起目标、不改页保护；全程一个句柄，扫描前后各核一次 PID + 创建时间。
- `Ksword5.1/Ksword5.1/ProcessDock/ProcessDetailWindow.InjectionTrace.cpp`：进程详情窗口
  「模块」页的「快速注入检查」「深度注入检查」两个按钮（`m_injectionTraceButton` /
  `m_injectionTraceDeepButton`，两个共用一次扫描，启停要一起改）。

**加规则要加在判据层**，那里有离线测试；采集器只负责"把现场读出来"，不产生结论。
归一化映像比较复用 `PeImageMap` + `ImageDiff`，不得再引入第二套 PE 解析器。

## 不变式

- 结论只有 `AnalysisConclusion` 四态，没有 `isInjected` / score / 权重。
- 时间字段只有 `firstObservedUtc100ns`（首次观测时间），**不是**注入时间。
- `injectorAttribution` 恒为 `OwnerAttribution::Unknown`，本层不提供把它升格的入口。
- 例外规则必须绑定 目标程序版本 + 被修改模块身份 + 具体 RVA 范围（≤ 64 KiB），缺一即拒；
  规则要求字节检查而现场读不到字节时**不命中**（fail-closed）。
  `modifiedModuleIdentity` 必须用 `ModuleIdentityKeyFor()` 生成，手写路径大小写不同就永远匹配不上。
- 只有三类观测能撑起 `DifferenceObserved`：归一化后仍与可靠参考不同、交叉视图**矛盾**、
  载荷结构且**可靠展开的帧**进入其中。私有 RX/RWX 本身只到 `Indeterminate`。

## 两个容易搞反的分界

- **能力限制 vs 覆盖缺口**：`capabilityLimitKeys`（本版本不做）只缩小适用范围，
  `coverageGapKeys`（打算查没查成）压制"未发现差异"。闸门用 `scopeIntact`，
  `coverageComplete` 只用于展示。混成一张表会让四态在生产里退化成三态。
- **交叉视图矛盾 vs 不对称**：`ModuleCrossIssueIsContradiction()`。
  explorer.exe 上稳定有十几个"映像映射无加载器项"（资源映射 / 元数据映像 / .NET），
  那是不对称，只到 `Indeterminate`；路径/大小不符、主映像自相矛盾才是矛盾。

## 进程列表的「注入面」列

`ProcessDock` 的 `TableColumn::InjectionSurface`（Security 分组，默认隐藏），
由右键「筛选注入面」手动填充，走 `ks::process::ScreenProcessInjectionSurface()` ——
只枚举地址空间 + 分类，不读内存、不碰模块/PE/工作集/线程/驱动。

**为什么是计数不是"状态"**：实测本机 496 个进程里，310 个可打开的有 **284 个（92%）**
都有私有/映射可执行内存，280 个还带可写可执行。所以"有没有动态代码"当告警等于全亮；
有区分度的是**数量的离群程度**（同一次采样 avpui 841 块、kpm 682 块，多数进程个位数）。
列里因此显示 `N 块 / M 可写可执行`，而 `SurfaceScreenState` 非 Screened 时显示
「未筛选 / 访问受限 / 身份不符 / 筛选失败」——**绝不显示 0**，那会把"打不开"读成"干净"。

**为什么是手动不是周期**：全机一轮 1073 ms（中位 2.52 ms、p95 9.5 ms、最慢 51 ms），
而进程表每秒刷新一次。生产入口单进程实测 explorer 23.6 ms。
与 PPL 列相反，这一列的值**跨刷新轮沿用**（每轮清空等于列永远是空的）；
缓存按进程身份做键，PID 复用会走新条目，退出保留行会清空计数。

## R0 扫描后端（issue #196 §五 第一、二层）

- 协议 `shared/driver/KswordArkInjectionScanIoctl.h`：
  `IOCTL_KSWORD_ARK_ENUMERATE_PROCESS_VAD`(0x912) 与
  `IOCTL_KSWORD_ARK_SCAN_PROCESS_EXECUTABLE_PTE`(0x913)，两条都是 `FILE_WRITE_ACCESS`、只读、可游标续扫。
- 实现 `KswordARKDriver/src/features/injection/`：`injection_vad.c`（EPROCESS.VadRoot 平衡树中序遍历）、
  `injection_pte_scan.c`（四级页表自顶向下、整页读表、只下降到 present 子树）、`injection_ioctl.c`。
- **不能转调 `ZwQueryVirtualMemory` 冒充第二视图**：它和 R3 的 `VirtualQueryEx` 同源，
  交叉核对它等于自己和自己比。VAD 直接读平衡树、页表读 CR3 下的物理页，才是独立来源。
- R3 侧 `ArkDriverClient/ArkDriverInjectionScan.cpp`，CLI 子命令 `memory enum-vad` / `memory scan-exec-pte`。
- 判据在 `EvaluateKernelCrossView()`：结论**只到 Indeterminate**，因为内核交叉差异的
  合法成因目录还没在实机数据上建立（`kLimitKernelBenignBaseline`）。有基线后再考虑升档。

### 新增 IOCTL 要登记八处

`shared/driver/*.h` 定义 → `include/ark/ark_ioctl.h` 聚合 → `ioctl_registry.c` 声明 + 表项 →
驱动 `.vcxproj` / `.vcxproj.filters`（新目录还要加 `<Filter Include=...>` 声明）→
`tools/driver_functional_ci/driver_test_plan.json`（执行或排除恰好一次，`plan_gate.py` 点名）→
`KswordCLI` 子命令 + 内置 `help` 元数据 → `docs/CLI使用文档.md`。漏任何一处都会在别处炸。

### 实机读数（2026-09-12，KSword-HVM-Target，Win11 22621.4317）

- 页表扫描跑通：explorer.exe 整个用户地址空间 **7502 段 / 443 次表读 / 22779 个可执行 4 KiB 页**，
  一次调用跑完；lsass 1049 段 / 98 次表读。守卫路径全对：逆序范围 `status=10`+`0xc000000d`、
  不存在的 PID `status=5`+`0xc000000b`、`maxEntries=0` 走驱动侧默认。停/起服务没有引发蓝屏。
- **独立实现交叉核对零分歧**：用另写的 C# `VirtualQueryEx` 枚举 explorer，驱动报的 7266 段
  可执行页 **全部** 落在 VirtualQueryEx 也认为已提交且可执行的区域内
  （`PteInVqNonExec=0`、`PteOutsideCommitted=0`）。页表口径 89 MB ≤ VQ 保护属性口径 231 MB，
  差额是"尚未换入、没有 PTE 的页"，符合预期。**干净机器上 `ExecutableBeyondR3View` 为 0**，
  不会误报刷屏。
- **VAD 枚举在这台机器上问不出真数据**：DynData 偏移表不覆盖 22621.4317，
  返回 `status=4`(DYNDATA_MISSING) + `profileVerified=0`，即设计中的"明确降级"。
  树遍历本身仍未在真数据上验证过 —— 要验得先有覆盖该 build 的 profile。
- 段合并键是**完整的生效标志位**（含 Accessed/Dirty），所以 A/D 不同的相邻页不会合并，
  explorer 因此是 7502 段而不是更少。这是精确换碎片的取舍，目前 16384 的默认够用，
  没有读数要求改它。

### 驱动侧的两个坑

- `PsLookupProcessByProcessId` / `KeStackAttachProcess` / `KeUnstackDetachProcess` / `KAPC_STATE`
  声明在 **ntifs.h**，本驱动只 include ntddk.h。按 `memory_pagetable.c` 的做法手工声明，
  ApcState 用 `DECLSPEC_ALIGN(16) UCHAR [128]` 承接。
- **私有 VAD 就是 `MMVAD_SHORT`**，它后面的 `Subsection`/`ViewLinks` 根本不存在。
  按 `sizeof(MMVAD)` 整读会跨出分配，短 VAD 落在页尾时常驻探测还会失败，
  于是一条正常的私有区域被记成"节点读不到"。先读 SHORT，确认非私有再单独读长字段。

## 结果对话框：判据不能松，说法必须改

用户反馈原话是"注入解析过于难以阅读""难以理解"。病根不是排版，是**用词全是内部术语**
（覆盖缺口 / 能力限制 / 观测语义 / 规则 ID / `key=value`）。改 UI 时守住这几条：

- **结论和它的限定条件分两行**。原来写成 `已覆盖范围内未发现相应异常（不是"从未被注入"）`，
  括号里那半句实际没人读。拆成 `conclusionHeadline()` + `conclusionCaveatText()` 两个函数。
- **条目必须按规则分组**（`QTreeWidget`，不是 `QTableWidget`）。Ksword5.1.exe 自身实测
  874 条"动态/非映像可执行内存"，平铺没人读得完。分组标题 `规则名 — N 处`，
  标题下用 `ruleMeaningText()` 给一句"这类是什么 + 常见的正常成因"，> 12 条默认折叠。
  选中分组标题时详情面板显示这一类的整体说明，不是留着上一条不动。
- **分组顺序沿用判据层产出顺序**，不重排。一重排就会被读成按严重程度排，而本层不产生严重程度。
- **显示用人读的单位**：`humanSizeText()`（4.0 KB 不是 0x1000）、`humanProtectionText()`
  （可写＋可执行 不是 RWX）、`humanRegionTypeText()`（私有内存 不是 PRIVATE）。
  只用于显示，判据层仍然只认原始值。
- **规则 ID / 检测器版本 / `key=value` 事实压到详情面板最后的「技术细节」段**，不进表格列。
- **「查了什么」页每一块前面加一句解释**：缺口 vs 能力限制是最容易被误读的一处。

**改动代价提醒**：这一轮新增 85 条待翻译文本，两个语言包各插 85 行。
i18n 门禁是 PreBuildEvent，C++ 还没编译就会先失败——先跑 audit 再跑构建，省一轮。

## 权限

最小权限优先，但两项能力需要按能力单独提权，否则**静默问不出数据**：

- `QueryWorkingSetEx` 要 `PROCESS_QUERY_INFORMATION`（`..._LIMITED_...` 下一页都问不到）。
- `NtQueryInformationThread(ThreadQuerySetWin32StartAddress)` 要 `THREAD_QUERY_INFORMATION`。

提不到就留缺口，不要把整次采集升级到更高权限。受保护进程拒绝访问时输出"访问受限"，
不是"未发现注入"。
