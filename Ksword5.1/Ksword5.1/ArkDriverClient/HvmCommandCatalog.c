#include "HvmCommandCatalog.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../../shared/driver/KswordArkHvmIoctl.h"

static const HVM_COMMAND_SPEC g_commands[] = {
    { "metrics", "虚拟化测量", "查询与观测", "读取逐核转换时间、INVEPT 和替换页资源计数。", HvmMetrics, 1, 0UL, 0UL, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "status", "运行状态", "查询与观测", "读取完整能力、处理器状态与退出计数。", HvmStatus, 1, 0UL, 0UL, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "cpuid-view", "CPUID 可见性", "查询与观测", "读取当前进程看到的 Hypervisor 身份；无需加载驱动。", HvmCpuid, 1, 0UL, 0UL, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "probe-platform", "平台探针", "查询与观测", "读取 CET、KVA shadow 和平台状态，不进入 VMX。", HvmPlatform, 1, 0UL, 0UL, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "selfcheck", "使用前自检", "查询与观测", "只读检查能力、后端和当前状态，报告未满足的条件。", HvmSelfcheck, 1, 0UL, 0UL, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "gdt-dump", "GDT 快照", "查询与观测", "读取指定处理器的 GDT。处理器编号为十进制。", HvmGdt, 1, 0UL, 0UL, 1,
      { { "处理器编号（十进制）", HvmDecimal32, "0" } } },
    { "events", "事件记录", "查询与观测", "读取序号大于指定值的事件；默认读取环内现有记录。", HvmEvents, 1, 0UL, 0UL, 2,
      { { "起始事件序号（十进制）", HvmDecimal64, "0" }, { "最多事件数（十进制）", HvmDecimal32, "64" } } },
    { "ept-leaf", "EPT 叶项", "查询与观测", "读取指定物理地址的基础 EPT 翻译链与权限。", HvmEptLeaf, 1, 0UL, 0UL, 1,
      { { "物理地址（十六进制）", HvmHex64, "0" } } },
    { "prepare", "准备资源", "生命周期", "分配资源，不进入 VMX；仅在资源尚未准备时执行。", HvmControl, 0, KSWORD_ARK_HVM_CONTROL_PREPARE, KSWORD_ARK_HVM_CONTROL_FLAG_UI_CONFIRMED |
      KSWORD_ARK_HVM_CONTROL_FLAG_ALLOW_NESTED, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "prepare-eptpsw", "准备 EPTP 切换后端", "生命周期", "准备资源并请求 EPTP 切换；之后检查 EPTP_SWITCH_ARMED。", HvmControl, 0, KSWORD_ARK_HVM_CONTROL_PREPARE, KSWORD_ARK_HVM_CONTROL_FLAG_UI_CONFIRMED |
      KSWORD_ARK_HVM_CONTROL_FLAG_ALLOW_NESTED |
      KSWORD_ARK_HVM_CONTROL_FLAG_ENABLE_EPTP_SWITCH, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "prepare-localept", "准备每核私有 EPT", "生命周期", "准备资源并请求每处理器私有 EPT。", HvmControl, 0, KSWORD_ARK_HVM_CONTROL_PREPARE, KSWORD_ARK_HVM_CONTROL_FLAG_UI_CONFIRMED |
      KSWORD_ARK_HVM_CONTROL_FLAG_ALLOW_NESTED |
      KSWORD_ARK_HVM_CONTROL_FLAG_ENABLE_LOCAL_EPT, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "self-test", "处理器 VMX 自检", "生命周期", "资源准备后逐处理器执行 VMXON/VMXOFF。", HvmControl, 0, KSWORD_ARK_HVM_CONTROL_SELF_TEST, KSWORD_ARK_HVM_CONTROL_FLAG_UI_CONFIRMED |
      KSWORD_ARK_HVM_CONTROL_FLAG_FORCE |
      KSWORD_ARK_HVM_CONTROL_FLAG_ALLOW_NESTED, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "resident", "启动常驻", "生命周期", "准备并通过自检后启动全处理器常驻。", HvmControl, 0, KSWORD_ARK_HVM_CONTROL_START_RESIDENT, KSWORD_ARK_HVM_CONTROL_FLAG_UI_CONFIRMED |
      KSWORD_ARK_HVM_CONTROL_FLAG_FORCE |
      KSWORD_ARK_HVM_CONTROL_FLAG_ALLOW_NESTED, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "resident-nested", "启动嵌套常驻", "生命周期", "启动常驻并开启嵌套 VMX 指令派发。", HvmControl, 0, KSWORD_ARK_HVM_CONTROL_START_RESIDENT, KSWORD_ARK_HVM_CONTROL_FLAG_UI_CONFIRMED |
      KSWORD_ARK_HVM_CONTROL_FLAG_FORCE |
      KSWORD_ARK_HVM_CONTROL_FLAG_ALLOW_NESTED |
      KSWORD_ARK_HVM_CONTROL_FLAG_ENABLE_NESTED_VMX, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "resident-nested-hidehv", "启动嵌套常驻并隐藏身份", "生命周期", "启动嵌套常驻，并对来宾用户态 CPUID 隐藏 Hypervisor 身份。", HvmControl, 0, KSWORD_ARK_HVM_CONTROL_START_RESIDENT, KSWORD_ARK_HVM_CONTROL_FLAG_UI_CONFIRMED |
      KSWORD_ARK_HVM_CONTROL_FLAG_FORCE |
      KSWORD_ARK_HVM_CONTROL_FLAG_ALLOW_NESTED |
      KSWORD_ARK_HVM_CONTROL_FLAG_ENABLE_NESTED_VMX |
      KSWORD_ARK_HVM_CONTROL_FLAG_HIDE_HYPERVISOR, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "resident-nested-fullsnapshot", "嵌套常驻完整快照对照", "生命周期", "保留 CPUID 退出的完整诊断字段读取，作为同一驱动的性能对照。", HvmControl, 0, KSWORD_ARK_HVM_CONTROL_START_RESIDENT, KSWORD_ARK_HVM_CONTROL_FLAG_UI_CONFIRMED |
      KSWORD_ARK_HVM_CONTROL_FLAG_FORCE |
      KSWORD_ARK_HVM_CONTROL_FLAG_ALLOW_NESTED |
      KSWORD_ARK_HVM_CONTROL_FLAG_ENABLE_NESTED_VMX |
      KSWORD_ARK_HVM_CONTROL_FLAG_HIDE_HYPERVISOR |
      KSWORD_ARK_HVM_CONTROL_FLAG_FULL_EXIT_SNAPSHOT, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "resident-vmreadbench", "VMREAD 开销测量", "生命周期", "每次退出额外执行指定次数的 VMREAD；仅用于测量。", HvmControl, 0, KSWORD_ARK_HVM_CONTROL_START_RESIDENT, KSWORD_ARK_HVM_CONTROL_FLAG_UI_CONFIRMED |
      KSWORD_ARK_HVM_CONTROL_FLAG_FORCE |
      KSWORD_ARK_HVM_CONTROL_FLAG_ALLOW_NESTED |
      KSWORD_ARK_HVM_CONTROL_FLAG_VMREAD_BENCH, 1,
      { { "VMREAD 次数（十进制）", HvmDecimal32, "512" } } },
    { "resident-trace", "启动常驻并记录退出", "生命周期", "普通退出也写入事件环；高频记录会覆盖旧事件。", HvmControl, 0, KSWORD_ARK_HVM_CONTROL_START_RESIDENT, KSWORD_ARK_HVM_CONTROL_FLAG_UI_CONFIRMED |
      KSWORD_ARK_HVM_CONTROL_FLAG_FORCE |
      KSWORD_ARK_HVM_CONTROL_FLAG_ALLOW_NESTED |
      KSWORD_ARK_HVM_CONTROL_FLAG_TRACE_ROUTINE_EXITS, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "soak", "有界常驻自检", "生命周期", "启动常驻，保持指定时间，再停止；时长单位为毫秒。", HvmControl, 0, KSWORD_ARK_HVM_CONTROL_SOAK, KSWORD_ARK_HVM_CONTROL_FLAG_UI_CONFIRMED |
      KSWORD_ARK_HVM_CONTROL_FLAG_FORCE |
      KSWORD_ARK_HVM_CONTROL_FLAG_ALLOW_NESTED, 1,
      { { "保持时长（毫秒）", HvmDecimal32, "1000" } } },
    { "stop", "停止常驻", "生命周期", "停止全部处理器的常驻；查看返回的活动处理器数。", HvmControl, 0, KSWORD_ARK_HVM_CONTROL_STOP_RESIDENT, KSWORD_ARK_HVM_CONTROL_FLAG_UI_CONFIRMED, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "teardown", "释放资源", "生命周期", "停止常驻后释放资源与保留的页。", HvmControl, 0, KSWORD_ARK_HVM_CONTROL_TEARDOWN, KSWORD_ARK_HVM_CONTROL_FLAG_UI_CONFIRMED, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "reset-fault", "重置故障", "生命周期", "停止常驻后清除可恢复的故障状态。", HvmControl, 0, KSWORD_ARK_HVM_CONTROL_RESET_FAULT, KSWORD_ARK_HVM_CONTROL_FLAG_UI_CONFIRMED |
      KSWORD_ARK_HVM_CONTROL_FLAG_FORCE, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "launch-test-guest", "一次性测试来宾", "嵌套与诊断", "执行一次受控 VMLAUNCH/VMCALL，然后返回。", HvmControl, 0, KSWORD_ARK_HVM_CONTROL_LAUNCH_TEST_GUEST, KSWORD_ARK_HVM_CONTROL_FLAG_UI_CONFIRMED |
      KSWORD_ARK_HVM_CONTROL_FLAG_FORCE |
      KSWORD_ARK_HVM_CONTROL_FLAG_ALLOW_NESTED |
      KSWORD_ARK_HVM_CONTROL_FLAG_ONE_SHOT_GUEST, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "validate-nested", "嵌套能力校验", "嵌套与诊断", "校验嵌套 VMX 与 eVMCS 能力；按返回结果判断完成范围。", HvmControl, 0, KSWORD_ARK_HVM_CONTROL_VALIDATE_NESTED, KSWORD_ARK_HVM_CONTROL_FLAG_UI_CONFIRMED |
      KSWORD_ARK_HVM_CONTROL_FLAG_FORCE |
      KSWORD_ARK_HVM_CONTROL_FLAG_ALLOW_NESTED |
      KSWORD_ARK_HVM_CONTROL_FLAG_ENABLE_NESTED_VMX |
      KSWORD_ARK_HVM_CONTROL_FLAG_ENABLE_EVMCS, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "nested-probe", "单核嵌套探针", "嵌套与诊断", "要求嵌套常驻已开启，实测 VMX 指令与 L2 进入和退出。", HvmNestedProbe, 0, 0UL, 0UL, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "nested-probe-all", "全核嵌套探针", "嵌套与诊断", "每个处理器并发执行嵌套探针，任一处理器失败即失败。", HvmNestedProbeAll, 0, 0UL, 0UL, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "nested-ad", "EPT A/D 拒绝探针", "嵌套与诊断", "实测不支持的 EPT A/D 配置是否正确被拒绝。", HvmNestedAd, 0, 0UL, 0UL, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "nested-selfvirt", "单核自虚拟化探针", "嵌套与诊断", "验证 L1 上下文进入 L2、退出反射以及回到 L1。", HvmSelfvirt, 0, 0UL, 0UL, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "nested-selfvirt-all", "全核自虚拟化探针", "嵌套与诊断", "每个处理器并发验证自虚拟化与返回。", HvmSelfvirtAll, 0, 0UL, 0UL, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "acl-probe", "设备访问位探针", "嵌套与诊断", "使用不同访问权限的设备句柄验证 IOCTL 访问门。", HvmAcl, 0, 0UL, 0UL, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "probe-flags", "控制标志拒绝探针", "嵌套与诊断", "发送负向测试请求验证标志检查；会改变测试状态。", HvmFlags, 0, 0UL, 0UL, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "probe-xonly", "仅执行权限探针", "嵌套与诊断", "要求常驻停止；安装测试规则、启动、读回、停止并清理。", HvmXonly, 0, 0UL, 0UL, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "rule-allowonce", "单次放行规则探针", "嵌套与诊断", "常驻停止时验证规则安装门，并立即移除测试规则。", HvmAllowOnce, 0, 0UL, 0UL, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "tlb-probe", "跨核 TLB 探针", "嵌套与诊断", "在指定毫秒数内检查跨处理器翻译失效。", HvmTlb, 0, 0UL, 0UL, 1,
      { { "保持时长（毫秒）", HvmDecimal32, "1000" } } },
    { "tlb-probe-exit", "强制退出 TLB 探针", "嵌套与诊断", "每次读之前执行 CPUID 强制退出，再验证翻译失效。", HvmTlbExit, 0, 0UL, 0UL, 1,
      { { "保持时长（毫秒）", HvmDecimal32, "1000" } } },
    { "view-query", "查询分离视图", "EPT 视图", "读取已安装的 EPT 分离视图。", HvmViewQuery, 1, 0UL, 0UL, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "view-probe", "视图安装探针", "EPT 视图", "资源已准备且常驻停止时安装测试视图并立即移除。", HvmViewProbe, 0, 0UL, 0UL, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "view-effect", "视图效果验证", "EPT 视图", "要求 EPTP 切换后端和自检；此测试会起停常驻。", HvmViewEffect, 0, 0UL, 0UL, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "view-verify", "验证现有视图", "EPT 视图", "分别报告现有视图的结构与实际读回效果。", HvmViewVerify, 0, 0UL, 0UL, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "nested-page-query", "查询嵌套页映射", "TinyCore 换页", "读取最近的 EPT12 根、替换页、合成次数和保留状态。", HvmPageQuery, 1, 0UL, 0UL, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "nested-page-map", "替换 TinyCore 物理页", "TinyCore 换页", "在线替换一页 4 KiB WB RAM。先查询当前 EPT12 根，并使用专用测试页。", HvmPageMap, 0, 0UL, 0UL, 4,
      { { "EPT12 指针（十六进制）", HvmHex64, NULL }, { "来宾物理页（十六进制，4 KiB 对齐）", HvmPageAddress, NULL }, { "影子页填充值（00–FF）", HvmByte, NULL }, { "VMM 进程 PID（0 自动选择 VMware）", HvmDecimal32, "0" } } },
    { "nested-page-map-region", "替换整段物理区间", "TinyCore 换页", "按 2 MiB 或 1 GiB 粒度替换一整段 WB RAM。整段都从原内容复制，所以刚映射完来宾看到的东西完全不变；随后用 nested-page-stage 改需要改的那几页。粒度只在 EPT12 自己也用同等或更粗的叶子映射该区间时才被接受，否则整条拒绝，不会降级成一页。", HvmPageMapRegion, 0, 0UL, 0UL, 4,
      { { "EPT12 指针（十六进制）", HvmHex64, NULL }, { "来宾物理基址（十六进制，需按粒度对齐）", HvmPageAddress, NULL }, { "叶粒度（12=4 KiB，21=2 MiB，30=1 GiB）", HvmDecimal32, "21" }, { "VMM 进程 PID（0 自动选择 VMware）", HvmDecimal32, "0" } } },
    { "nested-page-map-region-scan", "扫描源表后替换区间", "TinyCore 换页", "按大页粒度替换一整段，但允许源侧是 4 KiB 映射：先把区间内每一个源表项读一遍，权限与内存类型全部一致才放行。代价是租约变弱：后续只对首页路径做即时漂移检测，其余表项被改不会立即发现。不确定就用 nested-page-map-region。", HvmPageMapRegionScan, 0, 0UL, 0UL, 4,
      { { "EPT12 指针（十六进制）", HvmHex64, NULL }, { "来宾物理基址（十六进制，需按粒度对齐）", HvmPageAddress, NULL }, { "叶粒度（12=4 KiB，21=2 MiB，30=1 GiB）", HvmDecimal32, "21" }, { "VMM 进程 PID（0 自动选择 VMware）", HvmDecimal32, "0" } } },
    { "nested-page-stage", "改写区间内一页", "TinyCore 换页", "覆写已发布区间里第 N 个 4 KiB 页的替换内容。索引超出区间会被拒绝而不是截断。这不是原子更新：并发读的来宾可能看到新旧混合的字节。", HvmPageStage, 0, 0UL, 0UL, 2,
      { { "页索引（十进制，0 起）", HvmDecimal32, NULL }, { "填充值（00–FF）", HvmByte, NULL } } },
    { "nested-page-remove", "撤销 TinyCore 换页", "TinyCore 换页", "取消替换并等待全部处理器失效；retired 非零表示仍有保留页。", HvmPageRemove, 0, 0UL, 0UL, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "nested-page-map-test", "换页故障注入", "TinyCore 换页", "仅用于专用测试页：1 分配失败，2 提交前取消，3 提交后回滚，4 提交失效失败。", HvmPageMapTest, 0, 0UL, 0UL, 4,
      { { "EPT12 指针（十六进制）", HvmHex64, NULL }, { "来宾物理页（十六进制，4 KiB 对齐）", HvmPageAddress, NULL }, { "影子页填充值（00–FF）", HvmByte, NULL }, { "故障阶段（1–4）", HvmDecimal32, NULL } } },
    { "nested-page-remove-test", "撤销失效故障注入", "TinyCore 换页", "模拟撤销时失效失败，保留 retired 页；随后用正常撤销命令重试回收。", HvmPageRemoveTest, 0, 0UL, 0UL, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "msr-log", "记录指定 MSR", "寄存器策略", "添加指定 MSR 的日志策略；编号为十六进制。", HvmMsrLog, 0, 0UL, 0UL, 1,
      { { "MSR 编号（十六进制）", HvmHex32, "10" } } },
    { "msr-clear", "清空 MSR 策略", "寄存器策略", "清空已安装的 MSR 策略。", HvmMsrClear, 0, 0UL, 0UL, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "cr-track-cr3-on", "开启 CR3 追踪", "寄存器策略", "常驻启动前开启 CR3 追踪，供 R-1 进程操作使用。", HvmCrOn, 0, 0UL, 0UL, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "cr-track-cr3-off", "关闭 CR3 追踪", "寄存器策略", "常驻停止时关闭 CR3 追踪。", HvmCrOff, 0, 0UL, 0UL, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "inject-query", "查询注入", "R-1 进程", "读取 R-1 注入表。", HvmInjectQuery, 1, 0UL, 0UL, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "inject-test", "标记写入测试", "R-1 进程", "向目标进程的标记地址写常数；需提供执行靶页地址。", HvmInjectTest, 0, 0UL, 0UL, 4,
      { { "进程 PID（十进制）", HvmDecimal32, NULL }, { "执行靶页地址（十六进制）", HvmHex64, NULL }, { "标记地址（十六进制）", HvmHex64, NULL }, { "写入值（十六进制）", HvmHex32, "4B535744" } } },
    { "inject-dll", "注入 DLL", "R-1 进程", "使用指定执行靶页与 LoadLibraryW 地址注入 DLL；路径支持 Unicode。", HvmInjectDll, 0, 0UL, 0UL, 4,
      { { "进程 PID（十进制）", HvmDecimal32, NULL }, { "执行靶页地址（十六进制）", HvmHex64, NULL }, { "LoadLibraryW 地址（十六进制，0 自动解析）", HvmHex64, NULL }, { "DLL 完整路径", HvmPath, NULL } } },
    { "inject-release", "撤销进程注入", "R-1 进程", "撤销指定 PID 的 R-1 注入。", HvmInjectRelease, 0, 0UL, 0UL, 1,
      { { "进程 PID（十进制）", HvmDecimal32, NULL } } },
    { "inject-release-all", "撤销全部注入", "R-1 进程", "撤销全部 R-1 注入。", HvmInjectClear, 0, 0UL, 0UL, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "proc-query", "查询进程处置", "R-1 进程", "读取已安装的 R-1 进程处置。", HvmProcQuery, 1, 0UL, 0UL, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "proc-freeze", "冻结进程", "R-1 进程", "拒绝靶页执行并注入缺页异常；要求 CR3 追踪和 EPTP 切换。", HvmProcFreeze, 0, 0UL, 0UL, 2,
      { { "进程 PID（十进制）", HvmDecimal32, NULL }, { "执行靶页地址（十六进制）", HvmHex64, NULL } } },
    { "proc-terminate", "结束进程", "R-1 进程", "拒绝靶页执行并注入无效指令异常，由来宾处理进程退出。", HvmProcTerminate, 0, 0UL, 0UL, 2,
      { { "进程 PID（十进制）", HvmDecimal32, NULL }, { "执行靶页地址（十六进制）", HvmHex64, NULL } } },
    { "proc-release", "撤销进程处置", "R-1 进程", "撤销指定 PID 的处置。", HvmProcRelease, 0, 0UL, 0UL, 1,
      { { "进程 PID（十进制）", HvmDecimal32, NULL } } },
    { "proc-release-all", "清空进程处置", "R-1 进程", "撤销全部 R-1 进程处置。", HvmProcClear, 0, 0UL, 0UL, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "help", "命令帮助", "查询与观测", "显示全部命令及参数；不访问驱动。", HvmHelp, 1, 0UL, 0UL, 0,
      { { NULL, HvmDecimal32, NULL } } },
    { "commands", "命令目录", "查询与观测", "输出 GUI 与 CLI 共用的命令和参数定义。", HvmCommands, 1, 0UL, 0UL, 0,
      { { NULL, HvmDecimal32, NULL } } },
};

const HVM_COMMAND_SPEC* KswordHvmCommands(size_t* count)
{
    if (count != NULL) { *count = sizeof(g_commands) / sizeof(g_commands[0]); }
    return g_commands;
}

const HVM_COMMAND_SPEC* KswordHvmFindCommand(const char* name)
{
    size_t i;
    if (name == NULL) { return NULL; }
    if (strcmp(name, "--help") == 0) { name = "help"; }
    for (i = 0; i < sizeof(g_commands) / sizeof(g_commands[0]); ++i) {
        if (strcmp(g_commands[i].name, name) == 0) { return &g_commands[i]; }
    }
    return NULL;
}

static int HvmArgumentError(char* error, size_t size, const char* name)
{
    if (error != NULL && size > 0) {
        (void)snprintf(error, size, "%s", name);
    }
    return 2;
}

int KswordHvmValidateArguments(const HVM_COMMAND_SPEC* command, int count,
    const char* const* arguments, unsigned long long values[KSW_HVM_COMMAND_MAX_ARGS],
    char* error, size_t errorSize)
{
    unsigned int i;
    if (error != NULL && errorSize != 0) { error[0] = 0; }
    if (command == NULL || values == NULL || count < 0 ||
        count > (int)command->argumentCount || (count != 0 && arguments == NULL)) {
        return HvmArgumentError(error, errorSize, "argument-count");
    }
    memset(values, 0, sizeof(*values) * KSW_HVM_COMMAND_MAX_ARGS);
    for (i = 0; i < command->argumentCount; ++i) {
        const HVM_COMMAND_ARGUMENT* spec = &command->arguments[i];
        const char* input = (int)i < count ? arguments[i] : spec->defaultValue;
        const char* digits;
        const char* cursor;
        char* end = NULL;
        unsigned long long maximum = ULLONG_MAX;
        int base = (spec->kind == HvmDecimal32 || spec->kind == HvmDecimal64) ? 10 : 16;
        if (input == NULL || input[0] == 0) {
            return HvmArgumentError(error, errorSize, spec->name);
        }
        if (spec->kind == HvmPath) {
            int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, -1, NULL, 0);
            if (length <= 1 || length > KSWORD_ARK_HVM_INJECT_MAX_PAYLOAD_BYTES / (int)sizeof(wchar_t)) {
                return HvmArgumentError(error, errorSize, spec->name);
            }
            continue;
        }
        digits = input;
        if (base == 16 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) { digits += 2; }
        if (*digits == 0) { return HvmArgumentError(error, errorSize, spec->name); }
        for (cursor = digits; *cursor; ++cursor) {
            if (!((*cursor >= '0' && *cursor <= '9') ||
                  (base == 16 && ((*cursor >= 'a' && *cursor <= 'f') || (*cursor >= 'A' && *cursor <= 'F'))))) {
                return HvmArgumentError(error, errorSize, spec->name);
            }
        }
        errno = 0;
        values[i] = _strtoui64(digits, &end, base);
        if (spec->kind == HvmDecimal32 || spec->kind == HvmHex32) { maximum = 0xFFFFFFFFULL; }
        if (spec->kind == HvmByte) { maximum = 0xFFULL; }
        if (errno == ERANGE || end == digits || *end != 0 || values[i] > maximum ||
            (spec->kind == HvmPageAddress && (values[i] & 0xFFFULL) != 0)) {
            return HvmArgumentError(error, errorSize, spec->name);
        }
    }
    if (command->handler == HvmGdt && values[0] > INT_MAX) {
        return HvmArgumentError(error, errorSize, command->arguments[0].name);
    }
    if (command->handler == HvmPageMapTest && (values[3] < 1 || values[3] > 4)) {
        return HvmArgumentError(error, errorSize, command->arguments[3].name);
    }
    return 0;
}

void KswordHvmPrintJsonString(const char* text)
{
    const unsigned char* c = (const unsigned char*)text;
    putchar('"');
    for (; *c; ++c) {
        if (*c == '"' || *c == '\\') { putchar('\\'); putchar(*c); }
        else if (*c < 0x20) { printf("\\u%04x", *c); }
        else { putchar(*c); }
    }
    putchar('"');
}

void KswordHvmPrintCommands(int asJson)
{
    size_t i, count;
    const HVM_COMMAND_SPEC* commands = KswordHvmCommands(&count);
    if (asJson) { printf("{\"kind\":\"commands\",\"version\":1,\"commands\":["); }
    else { printf("hvm_ctl [--json] [--validate] <command> [arguments]\n"); }
    for (i = 0; i < count; ++i) {
        unsigned int j;
        const HVM_COMMAND_SPEC* c = &commands[i];
        if (asJson) {
            if (i) { putchar(','); }
            printf("{\"name\":"); KswordHvmPrintJsonString(c->name);
            printf(",\"title\":"); KswordHvmPrintJsonString(c->title);
            printf(",\"group\":"); KswordHvmPrintJsonString(c->group);
            printf(",\"description\":"); KswordHvmPrintJsonString(c->description);
            printf(",\"readOnly\":%s,\"command\":%lu,\"flags\":%lu,\"arguments\":[",
                c->readOnly ? "true" : "false", c->command, c->flags);
            for (j = 0; j < c->argumentCount; ++j) {
                const HVM_COMMAND_ARGUMENT* a = &c->arguments[j];
                if (j) { putchar(','); }
                printf("{\"name\":"); KswordHvmPrintJsonString(a->name);
                printf(",\"kind\":%d,\"default\":", a->kind);
                if (a->defaultValue) { KswordHvmPrintJsonString(a->defaultValue); } else { printf("null"); }
                putchar('}');
            }
            printf("]}");
        } else {
            printf("  %s", c->name);
            for (j = 0; j < c->argumentCount; ++j) {
                printf(" %c%s%c", c->arguments[j].defaultValue ? '[' : '<', c->arguments[j].name,
                       c->arguments[j].defaultValue ? ']' : '>');
            }
            printf("\n    %s\n", c->description);
        }
    }
    if (asJson) { printf("]}\n"); }
    else { printf("\n--validate: validate arguments without opening the driver.\n"
                  "Exit: 0=success, 1=transport failure, 2=refused/invalid, 3=not exercised.\n"); }
}
