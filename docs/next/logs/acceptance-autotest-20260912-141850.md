# KSword 自动化验收记录

时间(UTC): 2026-09-12T18:18:50.4641441Z → 2026-09-12T18:21:30.1645823Z
机器: Microsoft Windows 11 Pro for Workstations Insider Preview build 26300
CPU: 13th Gen Intel(R) Core(TM) i7-13700F

**总判定: FAIL**

| 段 | 判定 | 说明 |
|---|---|---|
| 离线断言套件 | FAIL |  个套件 /  条断言 |
| guest 工具 | PASS | hvm_probe + hvm_ctl，/MT 静态链接 |
| 在机验证 | PASS | 级别 full |

## 在机步骤

| 步骤 | 结果 | 说明 |
|---|---|---|
| deploy:hvm_ctl | OK |  |
| status | OK |  |
| prepare | OK |  |
| checkpoint:self-test | OK |  |
| alive:self-test | OK | 虚拟机仍然响应，且没有重启过 |
| self-test | OK |  |
| prune:before-driver-load-0912-1348 | OK | 自动修剪旧检查点以回收磁盘 |
| checkpoint:soak | OK |  |
| alive:soak | OK | 虚拟机仍然响应，且没有重启过 |
| soak | OK |  |
| stop | OK |  |
| teardown | OK |  |

---

BLOCKED 表示缺能力/权限/样本，**不等于通过**；NOT_RUN 表示这一段本次没跑。
原始 JSON: `acceptance-autotest-20260912-141850.json`

