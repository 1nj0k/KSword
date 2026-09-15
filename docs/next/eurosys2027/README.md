# EuroSys 2027 — Abstract review draft

状态：2026-09-15 作者审查稿。尚未完成全文、匿名证据包或投稿。
最终当前驱动证据见 [20260915-gap-closure](../paper-data/20260915-gap-closure/README.md)；
旧 pilot 与测量原型批次作为历史证据保留，不与当前二进制结果合并。

## Working title

**Live Hypervisor Interposition for Reversible Memory Control in Nested Virtual Machines**

此处 live interposition 指在已运行 Windows 1 下插入监控器；VMware 在插入后启动。
不表示已完成运行中 VMware/TinyCore 的 VMX 所有权交接。

## 提交页 Markdown

[submission-abstract.md](submission-abstract.md) 可直接粘贴到提交页 Abstract 字段，
包含正文、纯文本示意树和证据摘要，没有嵌入图片。[abstract.md](abstract.md) 是中文范围说明。
当前稿只维护此 Markdown 摘要；早期 TeX 与矢量图保留在本地，不作为当前投稿输入。

## Claims and supporting evidence

| 论断 | 原始/派生证据 | 范围 |
| --- | --- | --- |
| 10 组 Windows 进入/退出连续 | [transition 明细](../paper-data/20260915-gap-closure/final-windows/derived/transition-runs.csv) | 当前驱动，同一次 Windows boot，没有 VMware |
| 插入主体 1.257 ms、IPI 包络 90.55 µs | [内部阶段](../paper-data/20260915-gap-closure/final-windows/derived/internal-transition-summary.json) | QPC 内部边界，非应用暂停；冷 EPT 建表另记 |
| 双核 20 次读回闭环 | [SMP 汇总](../paper-data/20260915-gap-closure/smp/derived/smp-summary.json) | 空闲/负载各 10 次；16 次完整阶段日志，4 次不完整；同一保留页和 boot |
| 原页在替换页写入后恢复 | [写隔离](../paper-data/20260915-gap-closure/smp/write-isolation-20260915T205842Z.json) | 双核整页 MD5；写入 4 字节；采样非 4 KiB 原子读取 |
| 50 次故障控制试验 | [逐次故障判定](../paper-data/20260915-gap-closure/smp/derived/faults.csv) | 44 次完整证据，6 次不完整；调用边界注入，不是硬件 INVEPT 故障 |
| 10 分钟稳定观测 | [采样汇总](../paper-data/20260915-gap-closure/smp/derived/stability.json) | 一个 boot，21 个采样点；不能推广到数小时或全部内存泄漏 |
| RTT +36.9%、CPUID 10.30× | [最终 A/B](../paper-data/20260915-gap-closure/final-windows/derived/windows-overhead.csv) | Windows 无 VMware；off/pre → on → off/post；非 nested guest 开销 |
| 控制路径不依赖中间 VMM API/hook/注入 | [路径审计](../hvm-paper-evidence.md#5-跨层控制能证明什么) | 编排仍调用 PS Direct/VNC/vmrun；实现维护 EPT12 A/D 元数据 |

完整结果、未完成项和数值口径见 [追加证据报告](../hvm-paper-gap-closure.md)。
[相关工作](related-work.md) 逐项区分既有 late launch、嵌套分页、监控器替换与本原型；
有用应用、跨机器数据和中间 Hyper-V 仍待补充。Win10 LTSC 按用户要求暂缓。

投稿格式、匿名与 AI 披露见 [submission-notes.md](submission-notes.md)。原始档案包含
本机路径和身份，不可原样作为双盲附件；匿名副本必须保留作者侧可验证映射。
