# Abstract 审查入口

可直接粘贴到提交页 Abstract 字段的 Markdown 正文见
[submission-abstract.md](submission-abstract.md)，其中包含纯文本结构树，不插入图片。
这是当前唯一维护的提交页摘要，数据来自最终同版本 Windows A/B 与双核 TinyCore 实验。

## 中文范围说明

已验证的顺序是：Windows 1 没有 VMware 运行时进入、退出 KSword；随后先进入 KSword，
再启动 VMware/TinyCore，并对 TinyCore 保留页做 EPT 替换、写入隔离和恢复。
**没有验证将已运行的 VMware/TinyCore 随 Windows 一起热插入新增监控器。**

Windows 与 KSword 各 2 vCPU，TinyCore 正常 `/init`、2 vCPU；最外层 Windows 0 的
HVCI 保持启用。EPT01/12/02 是 KSword 内部局部层号；backing 地址属于 Windows 1
可见物理地址域，最外层 Hyper-V 仍控制最终机器地址翻译。

当前数据：10 组进入/退出、20 次双核读回闭环、50 次故障控制试验及一次 10 分钟观测。
阶段日志不完整的试验单独保留。性能与故障结果均限定于实际配置，不推导普适兼容性。

[论断与证据映射](README.md#claims-and-supporting-evidence) ·
[完整报告与限制](../hvm-paper-gap-closure.md) ·
[原始数据及重算方法](../paper-data/20260915-gap-closure/README.md)
