# HVM 内存监视 · 嵌套 Hyper-V 靶机实测

issue #195 第二十二节验收，2026-09-19。

## 靶机

| 项 | 值 |
| --- | --- |
| 虚拟机 | `KSword-HVM-Target`（Hyper-V，宿主 Windows 11 Pro for Workstations 26300） |
| 来宾 | Windows 11 Pro，build 22621 |
| vCPU | **2**（按 issue 验收要求从 4 改为 2） |
| 内存 | 8 GiB，静态 |
| 嵌套 | `ExposeVirtualizationExtensions = True`，来宾 `HypervisorPresent = True` |
| testsigning | Yes |
| **Monitor Trap Flag** | **不可用**——驱动 selfcheck 原话：「没有（嵌套 Hyper-V 不向客户机通告它）」 |
| execute-only EPT | 可用（`IA32_VMX_EPT_VPID_CAP` bit0 置位） |

MTF 不可用这一条正是本功能存在的前提：`ALLOW_ONCE` 靠 monitor-trap 把权限收回来，
在这台机器上恒不可用；`WATCH_ONCE` 去掉了收回那一步，所以不需要它。

## 产物

签名走仓库自带的自签测试证书（`scripts\Sign-KswordArkDriverTest.ps1`，
`CN=KswordARK Test Signing Certificate`，指纹 `34E402E8…`），证书导入来宾的
`LocalMachine\Root` 与 `TrustedPublisher`。

> 期间发现 `.cert\KswordARK-TestSigning.cer` 与同目录 `.pfx` **不是同一张证书**
> （`.cer` 是 `62FDA24A…`，`.pfx` 里是 `34E402E8…`，同主题不同代）。导入 `.cer`
> 不会为该签名建立信任，必须从 `.pfx` 导出配套公钥证书。这是仓库里一个既有的
> 陷阱，与本功能无关，但会让任何人的测试签名部署卡在"证书装了却还是不认"。

| 文件 | SHA-256 |
| --- | --- |
| `KswordARK.sys`（最终版） | `365A3341190C8AE7FA918BB1B34841B689813DD4FA39DCA6A66F97112E73AB5D` |
| `hvm_ctl.exe`（最终版） | `9C9FB91D1E2641D95A1622E1CACE4B0D3802100BA79F13CB27CA4B9A7BBBA8B2` |

每次部署都在来宾侧重算哈希与本地比对，确认跑的是刚构建的那一份而不是残留副本。

## 第 1 项 · WRITE First-touch

`hvm_ctl --json watch-selftest`，退出码 0，**PASS 13/13，失败 0，问不出来 0**。

```
watchId        2561
physicalPage   0x00000001FA31F000
residentBefore 2
residentAfter  2
```

| # | 检查 | 期望 | 实测 |
| --- | --- | --- | --- |
| 1 | 安装成功并分配非零编号 | `status=OK` 且 `watchId != 0` | `0xA01` |
| 2 | 实际生效掩码等于请求 | 写监视不触发架构归一化 | `0x2` |
| 3 | 武装后状态 | `armed` | `armed` |
| 4 | 装监视之前那次写不算命中 | `hitCount = 0` | `0` |
| 5 | 写触发一次命中 | `hitCount = 1` | `1` |
| 6 | 命中后自动解除 | `disarmed` | `disarmed` |
| 7 | 命中 GPA 落在被监视页 | `gpa & ~0xFFF` = 监视页 | `0x1FA31F000` |
| 8 | 命中现场记下非零 RIP | `rip != 0` | `0x00007FF7AC77A726` |
| 9 | 有效 GLA 指向实际被写地址 | `gla = &page[0]` | `0x00000227F0E10000` |
| 10 | 事件证据没有丢 | `lastHitStatus = published` | `published` |
| 11 | 第二次写不再命中 | `hitCount` 不变 | `1` |
| 12 | 被监视的写最终完成 | `page[0] = 0xB2` | `0xB2` |
| 13 | 命中没让任何处理器退出虚拟化 | `residentAfter = residentBefore` | `2 = 2` |

第 12、13 两条是这个功能与别的处置的分界：**命中不阻止访问**（与 `ENFORCE` 的
分界），**命中不结束常驻**（与严格 tripwire 的根本区别，也就是 issue 里那句
"命中 Watch ≠ HVM 退场"）。

## 第 4 项 · Nested Hyper-V（P0 关键验收项）

上表就是在这台 2-vCPU 嵌套靶机上跑出来的：

- 不依赖 Monitor Trap Flag —— 驱动明确报告本机没有它；
- WRITE First-touch 正常 Arm 并正常命中；
- `residentBefore = 2`，`residentAfter = 2`；
- 来宾不挂死（宿主侧全程 `State=Running / 正常运行`）；
- 不触发全局 fail-closed。

## 第 6 项 · 同页冲突

同一物理页上已有一条 watch 时再装：

```
status=leaf-conflict  owner=watch#3841
```

明确拒绝，指名占用者，原有 watch 不变。**PASS**

## 第 8 项 · HVM restart

```
watchId=5889   装上=armed   常驻中=armed   停常驻后=invalidated
rearm status=ok  state=armed  hitCount=0
```

停止常驻后 watch 转 `invalidated` 而不是静默保持 `armed`——watch 是"某段时间里
有人在看"的断言，跨过一次没人看的空档还报"未命中"是编造的观测结果。显式
`watch-rearm` 后恢复 `armed`。**PASS**

## 实机揪出的四个缺陷

离线套件（157/157）一条都抓不到这四个，它们只有真机第一次调用才暴露：

1. **外层契约门的白名单没同步。** `KswordARKHvmEptRuleControl` 有一张 flags 与
   operation 的白名单，`WATCH_ONCE` / `REARM` / `WATCH_QUERY` 只加进了协议头和
   内层 `...Locked`，这道门没加 —— 每一条 watch 请求在到达处置逻辑之前就被判
   `STATUS_INVALID_PARAMETER`，用户侧只看到 `win32=87`。这道门没有宿主侧对应物，
   加多少离线断言都跑不到那一行。
2. **我给 watch 加的"必须已常驻"门与既有安全不变式互斥。** 规则表在整个常驻期间
   冻结（退出路径不取 PASSIVE 锁就扫它），所以 watch 根本装不上。修法不是削弱那条
   冻结 —— 它是真实的安全不变式 —— 而是去掉多余的门：watch 与其余 EPT 规则一样
   **常驻停着时装，启动常驻后生效**。
3. **冻结时回报的状态码在说一件没发生的事。** 原先复用 `PARTIAL`（"部分处理器未能
   完成失效"），而实际上一个字段都没改过，还把读者引向失效机制。改成
   `RESIDENT_FROZEN`，直说"先停常驻"。顺带把 `WATCH_QUERY` 从冻结与高危策略审计里
   豁免：读表必须能在常驻期间做，那正是命中会发生的整个窗口。
4. **自检自己越界崩了。** 用例数组写成 12，实际填 13 条，靶机上直接 `0xC0000005`。
   一个会自己崩掉的自检产出的是"没有读数"，不是"失败"。

另外记一次自己的假读数：验收第 8 项第一遍判成 FAIL，实际是我拿 JSON 里的字符串
`"invalidated"` 去和整数 `4` 比。功能一直是对的，错的是判据。

## 未做

issue 第二十二节的 2（READ First-touch）、3（EXECUTE First-touch）、5（SMP 同时
命中）、7（VA 映射变化）、9（Event loss）本轮没跑。5 与 9 需要专门的并发/压力夹具，
2、3、7 需要各自的触发路径，都不是改判据就能顺带得到的。
