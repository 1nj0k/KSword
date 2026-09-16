# EuroSys 2027 — Abstract review draft

状态：2026-09-15 至 2026-09-16 UTC 的 4×2 实验已整理，尚未投稿。
当前证据见 [20260915-4x2](../paper-data/20260915-4x2/README.md)；旧批次保留，分版本分析。

## Working title

**Live Hypervisor Interposition for Reversible Memory Control in Nested Virtual Machines**

Live interposition 指在已运行 Windows 1 下插入监控器，VMware 在插入后启动。
不表示已完成运行中 VMware/TinyCore 的 VMX 所有权交接。

## 提交页 Markdown

[submission-abstract.md](submission-abstract.md) 可直接粘贴到 Abstract 字段，包含正文、
纯文本示意树和证据摘要，没有图片。[abstract.md](abstract.md) 是中文范围说明。
只维护这一份提交页 Markdown；早期 TeX 与图片不是本轮投稿输入。

## Claims and supporting evidence

| 论断 | 数据 | 限制 |
| --- | --- | --- |
| 4 核 Windows 十组进入/退出连续 | [transition](../paper-data/20260915-4x2/after/derived/transition-runs.csv) | 同一个 Windows boot，VMware 未运行 |
| 主体 2.8005 ms、IPI 包络 149.5 µs | [内部阶段](../paper-data/20260915-4x2/after/derived/internal-transition-summary.json) | 非直接应用暂停；并行阶段不可相加 |
| 双核 20/20 完整换页恢复 | [SMP](../paper-data/20260915-4x2/smp/derived/smp-summary.json) | 同一保留页；10 idle、10 load；不证明任意页原子性 |
| 15/15 故障控制、3/3 拒绝、写隔离 | [SMP](../paper-data/20260915-4x2/smp/derived/smp-summary.json) | 调用边界故障；不是硬件失效 |
| 实际 HTTP 服务的可逆页故障 | [HTTP v2](../paper-data/20260915-4x2/application-v2/derived/http-page-summary.json) | 3 次、90 响应；需要 guest root GPA/驻留辅助 |
| VMM 退出撤销映射并回收 | [lifecycle](../paper-data/20260915-4x2/derived/lifecycle.json) | 未覆盖同进程 guest reboot/快照/root reuse |
| nested memcpy −94.53%、CPUID −29.05% | [实现对照](../paper-data/20260915-4x2/derived/nested-improvements.csv) | 每工作负载 n=3，旧/新驱动；非 no-monitor baseline；RTT 点估计回退 |
| Windows CPUID 9.05×、RTT +34.82% | [当前 A/B](../paper-data/20260915-4x2/after/derived/windows-overhead.csv) | 无 VMware，描述区间和漂移完整保留 |
| 历史十分钟观测 | [旧记录](../paper-data/20260915-gap-closure/smp/derived/stability.json) | 旧 2×2 驱动，不能当成新 4×2 |
| 匿名证据包 | [生成器](../../../tools/hvm_paper/package_anonymous.py) | 原始 identity digest 为一致别名；包内 manifest 是实际校验和；不是可构建源码包 |

[完整结果和 15 项证据对应](../hvm-4x2-results.md) · [相关工作](related-work.md) ·
[投稿格式、匿名与 AI 披露](submission-notes.md)。
中间 Hyper-V、跨机器、nested 持久磁盘/no-monitor 基线和应用可见暂停仍未补齐。
Win10 LTSC、整夜稳定性、运行中完整链插入暂缓。没有进行 GUI 测试。

作者原始目录包含机器身份。匿名 ZIP 与作者私有 provenance 分离，未执行上传或投稿。
