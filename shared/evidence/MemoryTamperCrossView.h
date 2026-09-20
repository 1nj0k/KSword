#pragma once

// 内存内容交叉视图 —— 检测"CPU 读到的内容"与"绕开 CPU 读到的内容"不一致。
//
// 与 CrossViewDiff.h 的分工：那一层比的是**对象在不在**（进程、驱动对象、设备
// 对象藏没藏），本层比的是**同一段内存的内容**。两者的硬规则相同，结论类型不同。
//
// 为什么需要这一层：
// - SLAT / EPT 级别的隐藏不改页表、不改内容校验、也不留下任何 API 层面的痕迹。
//   它让处理器在**取指**时走真页、在**读数据**时走影子页。于是所有经由 CPU 的
//   读取——ReadProcessMemory、MmCopyVirtualMemory、甚至按物理地址映射再读——
//   看到的都是影子页里那份"干净"的字节，而真正被执行的代码已经被改过。
// - 单靠"内存 vs 磁盘映像"抓不到它：影子页里放的正是磁盘上那份原始字节，比对
//   结果会是完美一致。这类隐藏**只能**靠一条不经过 CPU 页表的读取路径来拆穿，
//   本项目里那条路径是 DDMA（磁盘控制器总线主控 DMA）。
//
// 三条贯穿本模块的硬规则（与 X 模块同源）：
// - T-01：读失败、通道不可用、范围未覆盖，一律**不是**"这一页没被篡改"。可比对
//   的视图不足两个时结论只能是 Inconclusive，绝不允许良性化成 Consistent。
// - T-02：一次性的不一致不构成结论。内存随时可能正在被合法写入（自修改代码、
//   热补丁、正在写的数据页），采样窗口内的竞态会产生与篡改无法区分的差异。
//   必须跨多轮复采样，只有**每一轮都不一致**才升为结论。
// - T-03：读不到的字节永远是缺失标记，不补 00 参与比较。补零会把"没读到"伪装成
//   "读到了 0"，进而凭空造出一条差异，或者凭空造出一次"一致"。
//
// 本层不认识 rootkit，也不产出恶意判定：它只回答"哪几条读取路径对同一段内存给出
// 了不同的答案，以及这种分歧落在哪一种已知模式上"。
//
// C++20、Qt-free、Win32-free。

#include <cstdint>
#include <string>
#include <vector>

namespace Ksword::Evidence {

// TamperReadPath：参与比对的读取路径。
//
// 分组是判据的核心，不是分类的方便：前四条都经过 CPU 的地址翻译，因此**会被
// SLAT / EPT 一起骗过**；DDMA 走磁盘控制器的 DMA，不经过 CPU 页表；最后两条是
// 静态参考，不反映内存现状。只有跨组的分歧才说明问题，组内分歧说明的是另一回事。
enum class TamperReadPath : int {
    UserModeVirtual = 0,  // R3：ReadProcessMemory。受句柄权限与用户态钩子影响。
    KernelVirtual,        // R0：MmCopyVirtualMemory。绕开句柄权限，仍走 CPU 页表。
    KernelPhysical,       // R0：先 VA→PA 再按物理地址读。仍由 CPU 发起访存。
    // HVM：改写自有页表项指向目标帧，**不调用任何文档化的内存管理器例程**。
    // 它与上面三条同属 CPU 组——名字叫 ring -1，但实现跑在 PASSIVE_LEVEL 的驱动
    // 上下文里、不进 VMX root，照样受 SLAT / EPT 约束。把它归进 DMA 组会让
    // 「CPU 视图被重定向」这个结论凭空多出一条不成立的证据。
    // 它独立的是另一件事：别的驱动挂钩 MmCopyMemory 挂不到它头上。
    HvmPrivateWindow,
    DmaPhysical,          // DDMA：磁盘控制器 DMA 读同一物理页。**不经过 CPU 页表**。
    ImageSectionClean,    // 节对象的干净参考页。静态参考，不是内存现状。
    OnDiskImage,          // 磁盘文件里对应位置。静态参考，不是内存现状。
};

const char* TamperReadPathName(TamperReadPath path) noexcept;

// TamperPathGroup：路径所属的分组，决定一条分歧怎么解释。
enum class TamperPathGroup : int {
    CpuMediated,   // 经过 CPU 地址翻译，会被 SLAT / EPT 一起影响。
    DmaMediated,   // 绕开 CPU 页表。
    StaticReference, // 静态参考，与内存现状无关。
};

TamperPathGroup GroupOf(TamperReadPath path) noexcept;

// TamperSampleStatus：一条路径在一轮采样里的结果。
//
// 四态而不是 bool：读不到、没去读、超出覆盖范围是三件不同的事，把它们塌成
// "没有数据"会让"无法判断"退化成"没有发现问题"。
enum class TamperSampleStatus : int {
    NotAttempted = 0,  // 本轮没有采这条路径（用户没选、前置条件不满足）。
    Unavailable,       // 通道本身不可用（DDMA 未配置、驱动未加载）。
    Failed,            // 采了但失败（读被拒、VA 翻译不出、目标已退出）。
    OutOfCoverage,     // 成功但不覆盖这一段（磁盘映像里没有对应节等）。
    Read,              // 成功读到，bytes 有效。
};

const char* TamperSampleStatusName(TamperSampleStatus status) noexcept;

// TamperViewSample：一条路径在一轮采样里的观测。
struct TamperViewSample final {
    TamperReadPath path = TamperReadPath::UserModeVirtual;
    TamperSampleStatus status = TamperSampleStatus::NotAttempted;
    std::vector<std::uint8_t> bytes;  // 仅 status == Read 时有意义。
    std::string failureText;          // 供界面直接展示，不做二次翻译。
};

// TamperRound：一轮采样。同一轮里的各条路径应当尽可能贴近同一时刻，
// 轮与轮之间应当隔开足够时间，否则 T-02 的复采样起不到排除竞态的作用。
struct TamperRound final {
    std::vector<TamperViewSample> views;
};

// TamperVerdict：一段内存的交叉视图结论。
enum class TamperVerdict : int {
    // 可比对的路径不足两条，或存在分歧但并非每一轮都出现。**不是"干净"。**
    Inconclusive = 0,
    // 所有可比对的路径逐字节一致。
    Consistent,
    // CPU 路径与 DMA 路径持续不一致。这是 SLAT / EPT 重定向的特征：处理器读到
    // 的内容与内存里真实的内容不是同一份。
    CpuViewRedirected,
    // R3 与 R0 持续不一致，而 DMA（若有）与 R0 一致。用户态读取路径被钩住。
    UserModeViewDiffers,
    // 所有活体路径彼此一致，但与静态参考不同。内存确实被改过，且没有隐藏——
    // 普通的 inline hook / 补丁就是这种形状。
    LiveDiffersFromReference,
    // 存在持续分歧，但不落在上面任何一种模式上。
    UnexplainedDisagreement,
};

const char* TamperVerdictName(TamperVerdict verdict) noexcept;

// TamperDisagreement：一对路径之间的一处分歧。
struct TamperDisagreement final {
    TamperReadPath left = TamperReadPath::UserModeVirtual;
    TamperReadPath right = TamperReadPath::UserModeVirtual;
    std::size_t firstDifferingOffset = 0;  // 段内偏移。
    std::size_t differingByteCount = 0;
    std::uint8_t leftByte = 0;
    std::uint8_t rightByte = 0;
    // persistentRounds / comparableRounds：这一对在多少轮里可比对、其中多少轮
    // 不一致。只有两者相等且 > 1 才满足 T-02。
    int comparableRounds = 0;
    int disagreeingRounds = 0;
};

// TamperFinding：一段内存的完整结论。
struct TamperFinding final {
    TamperVerdict verdict = TamperVerdict::Inconclusive;
    // comparableRoundCount：至少有两条路径同时读成功的轮数。
    int comparableRoundCount = 0;
    // 每条路径最后一轮的状态，供界面说明"为什么少了某条视图"。
    std::vector<TamperViewSample> lastRoundStatus;
    std::vector<TamperDisagreement> disagreements;
    // inconclusiveReason：verdict == Inconclusive 时说明卡在哪一条，
    // 让"问不出来"和"没问题"在界面上不可能被看混。
    std::string inconclusiveReason;
    // cpuMatchesStaticReference：CpuViewRedirected 时有意义。为真说明 CPU 侧读到
    // 的正是磁盘/节对象里那份原始字节，而 DMA 侧读到的是被改过的——这正是隐藏者
    // 想要的效果，也是本结论可信度最高的形状。
    bool cpuMatchesStaticReference = false;
};

// AnalyzeTamperRounds：
// - 输入：若干轮采样；
// - 处理：按 T-01 / T-02 / T-03 逐对比较并归类分歧模式；
// - 返回：结论与可回溯的分歧清单。
//
// 本函数不做任何 I/O，也不认识 PE 结构：调用方若要把磁盘映像作为参考视图，
// 必须先用 PeImageMap / ImageDiff 完成重定位与 IAT 的归一化再传进来，否则
// 每一次正常加载都会被报成差异。
TamperFinding AnalyzeTamperRounds(const std::vector<TamperRound>& rounds);

}  // namespace Ksword::Evidence
