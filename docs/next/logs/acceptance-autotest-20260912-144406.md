# KSword 自动化验收记录

时间(UTC): 2026-09-12T18:44:06.1958236Z → 2026-09-12T18:45:39.1981682Z
机器: Microsoft Windows 11 Pro for Workstations Insider Preview build 26300
CPU: 13th Gen Intel(R) Core(TM) i7-13700F

**总判定: PASS**

| 段 | 判定 | 说明 |
|---|---|---|
| 离线断言套件 | PASS | 12 个套件 / 4182 条断言 |
| guest 工具 | PASS | hvm_probe + hvm_ctl，/MT 静态链接 |
| 在机验证 | PASS | 级别 full |

## 离线套件明细

| 套件 | 通过 | 总数 |
|---|---:|---:|
| F evidence contract | 192 | 192 |
| X cross-view | 180 | 180 |
| G entity graph | 351 | 351 |
| C offline dump facts | 461 | 461 |
| D snapshot compare | 387 | 387 |
| S security state | 442 | 442 |
| N wfp analysis | 545 | 545 |
| T timeline | 381 | 381 |
| I image integrity | 395 | 395 |
| M memory evidence | 199 | 199 |
| HVM ept switch | 467 | 467 |
| HOOK patch compose | 182 | 182 |

## 在机步骤

| 步骤 | 结果 | 说明 |
|---|---|---|
| deploy:hvm_ctl | OK |  |
| status | OK |  |
| prepare | OK |  |
| prune:before-soak-0912-142026 | OK | 自动修剪旧检查点以回收磁盘 |
| checkpoint:self-test | OK |  |
| alive:self-test | OK | 虚拟机仍然响应，且没有重启过 |
| self-test | OK |  |
| prune:before-self-test-0912-142222 | OK | 自动修剪旧检查点以回收磁盘 |
| checkpoint:soak | OK |  |
| alive:soak | OK | 虚拟机仍然响应，且没有重启过 |
| soak | OK |  |
| stop | OK |  |
| teardown | OK |  |

---

BLOCKED 表示缺能力/权限/样本，**不等于通过**；NOT_RUN 表示这一段本次没跑。
原始 JSON: `acceptance-autotest-20260912-144406.json`

