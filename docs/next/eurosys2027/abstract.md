# Abstract 审查入口

可直接粘贴到提交页 Abstract 字段的 Markdown 见
[submission-abstract.md](submission-abstract.md)，包含正文、纯文本结构树及证据，不插入图片。

## 当前范围

- Windows 1/KSword 为 4 vCPU，TinyCore 为 2 vCPU；最外层 HVCI 保持启用。
- Windows 1 在 VMware 未运行时进入/退出 KSword；随后在 KSword 下启动 VMware/TinyCore。
  没有验证已运行 VMware/TinyCore 整条链的 VMX 所有权交接，也未确认是 VMware 缺陷。
- 新版本 10 组进入/退出、20 次完整双核读回、15 次完整故障控制试验、3 次拒绝、3 次完整 HTTP 场景。
- 新版本 nested 内存复制耗时降低 94.53%、CPUID 降低 29.05%；Windows CPUID 仍为 9.05×、RTT +34.82%。
- 不补长期观测；已有十分钟数据属于旧 2×2，不能标成新 4×2。
- EPT backing 地址属于 Windows 1 可见物理地址域，Hyper-V 仍控制最终机器地址翻译。

[完整报告与限制](../hvm-4x2-results.md) ·
[原始数据及重算方法](../paper-data/20260915-4x2/README.md) ·
[论断与证据映射](README.md#claims-and-supporting-evidence)
