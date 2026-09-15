# 2026-09-15 HVM 单机原始证据

结论、开销表、环境、故障结果和 15 类证据覆盖说明见
[论文证据报告](../../hvm-paper-evidence.md)。

| 路径 | 内容 |
| --- | --- |
| `manifest.json` / `build-manifest.json` | 实验声明、源码与产物身份、编译器、排除项 |
| `root-environment.json` / `windows1-environment.json` | root HVCI、Hyper-V、Windows 1、CPU、BIOS、VMX 与进程身份 |
| `windows1-<configuration>-<workload>-<iteration>-*.json` | 每次微基准；iteration=00 为预热；包含原始 stdout、前后状态和 PID |
| `nested-page-*.json` | 每次换页／恢复或拒绝测试；页地址、代次、时间、串口读值 |
| `write-isolation-*.json` | A5 → D1 → B2 → A5 写入隔离全记录 |
| `transition-*.json` | 每次常驻启停；忙循环与普通条件分别记录，超时不删除 |
| `stability-*.jsonl` | 21 次持续采样原始记录 |
| `tinycore-serial.txt` | 重启准备之前完整 TinyCore 串口；所有来宾基准与页读数的来源 |
| `observer-errors.jsonl` | 采集工具、传输或脚本错误；与 HVM 故障分开 |
| `orchestration.jsonl` | 标准靶机重启与恢复操作；不算运行中平移成功 |
| `coverage.json` | null 值及未执行原因，包含用户跳过 Win10 LTSC |
| `raw-index.json` | 原始文件 SHA256 与大小 |
| `derived/summary.json` | 可复算的总汇，包含失败／缺项 |
| `derived/*.csv` | 逐次表、统计表、退出率、每核间隙 |
| `derived/linux-runs/*.json` | 从完整串口按标记拆出的每次来宾执行记录，保留原文片段 |
| `derived/windows-overhead.svg` / `.png` | 可用于编辑／排版的单机性能图 |
| `derived/validation.json` | 数据完整性校验，不能替代运行时验收 |

复算入口位于 [tools/hvm_paper](../../../../tools/hvm_paper/README.md)。
本目录不包含运行所需的密码。Windows 1 内的专用测试目录为 `C:\ksword\paper`。
只有同一 boot 的无 VMware off/pre、on、off/post 三个块用于计算 steady-state overhead。
旧 boot 的 VMware-present on 样本独立保留。

所有成功率描述的是这个明确配置中的重复操作；不是独立安装、启动或不同硬件的成功率。
诊断 shell、单 guest vCPU、未知完整暂停时间、部分计数范围及缺少 nested baseline 的限制
均见报告；不要只复制表中的 PASS 或百分比。
