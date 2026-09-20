#include "MemoryTamperCrossView.h"

#include <algorithm>
#include <map>
#include <utility>

namespace Ksword::Evidence {

const char* TamperReadPathName(const TamperReadPath path) noexcept {
    switch (path) {
        case TamperReadPath::UserModeVirtual: return "R3 用户态读";
        case TamperReadPath::KernelVirtual: return "R0 虚拟地址读";
        case TamperReadPath::KernelPhysical: return "R0 物理地址读";
        case TamperReadPath::HvmPrivateWindow: return "HVM 私有页表窗口";
        case TamperReadPath::DmaPhysical: return "DDMA 物理读";
        case TamperReadPath::ImageSectionClean: return "节对象干净页";
        case TamperReadPath::OnDiskImage: return "磁盘映像";
    }
    return "未知路径";
}

TamperPathGroup GroupOf(const TamperReadPath path) noexcept {
    switch (path) {
        case TamperReadPath::UserModeVirtual:
        case TamperReadPath::KernelVirtual:
        case TamperReadPath::KernelPhysical:
        case TamperReadPath::HvmPrivateWindow:
            return TamperPathGroup::CpuMediated;
        case TamperReadPath::DmaPhysical:
            return TamperPathGroup::DmaMediated;
        case TamperReadPath::ImageSectionClean:
        case TamperReadPath::OnDiskImage:
            return TamperPathGroup::StaticReference;
    }
    return TamperPathGroup::CpuMediated;
}

const char* TamperSampleStatusName(const TamperSampleStatus status) noexcept {
    switch (status) {
        case TamperSampleStatus::NotAttempted: return "未采集";
        case TamperSampleStatus::Unavailable: return "通道不可用";
        case TamperSampleStatus::Failed: return "读取失败";
        case TamperSampleStatus::OutOfCoverage: return "不覆盖该范围";
        case TamperSampleStatus::Read: return "已读到";
    }
    return "未知状态";
}

const char* TamperVerdictName(const TamperVerdict verdict) noexcept {
    switch (verdict) {
        case TamperVerdict::Inconclusive: return "无法判定";
        case TamperVerdict::Consistent: return "各视图一致";
        case TamperVerdict::CpuViewRedirected: return "CPU 视图被重定向";
        case TamperVerdict::UserModeViewDiffers: return "用户态视图不同";
        case TamperVerdict::LiveDiffersFromReference: return "内存与静态参考不同";
        case TamperVerdict::UnexplainedDisagreement: return "存在未归类的分歧";
    }
    return "未知结论";
}

namespace {

// PathPair：一对路径，按枚举值排序，保证 (a,b) 与 (b,a) 落到同一个键上。
using PathPair = std::pair<TamperReadPath, TamperReadPath>;

PathPair MakePair(const TamperReadPath left, const TamperReadPath right) {
    return (static_cast<int>(left) <= static_cast<int>(right))
        ? PathPair{left, right}
        : PathPair{right, left};
}

// PairTally：一对路径跨轮次的累计。
struct PairTally final {
    int comparableRounds = 0;
    int disagreeingRounds = 0;
    bool firstDifferenceRecorded = false;
    std::size_t firstDifferingOffset = 0;
    std::size_t differingByteCount = 0;
    std::uint8_t leftByte = 0;
    std::uint8_t rightByte = 0;
};

// CompareBytes：逐字节比较，返回不同的字节数，并带出第一处不同。
//
// 长度不等时只比较公共前缀，并把长度差**计入**差异数：一条路径少读了一截，
// 与"读到了不同的字节"一样是分歧，不该被悄悄忽略。
std::size_t CompareBytes(
    const std::vector<std::uint8_t>& left,
    const std::vector<std::uint8_t>& right,
    std::size_t& firstDifferingOffsetOut,
    std::uint8_t& leftByteOut,
    std::uint8_t& rightByteOut) {
    const std::size_t commonLength = (std::min)(left.size(), right.size());
    std::size_t differingCount = 0;
    bool firstRecorded = false;
    for (std::size_t index = 0; index < commonLength; ++index) {
        if (left[index] == right[index]) {
            continue;
        }
        ++differingCount;
        if (!firstRecorded) {
            firstRecorded = true;
            firstDifferingOffsetOut = index;
            leftByteOut = left[index];
            rightByteOut = right[index];
        }
    }
    const std::size_t lengthGap =
        (left.size() > right.size()) ? (left.size() - right.size())
                                     : (right.size() - left.size());
    if (lengthGap != 0 && !firstRecorded) {
        firstDifferingOffsetOut = commonLength;
        leftByteOut = 0;
        rightByteOut = 0;
    }
    return differingCount + lengthGap;
}

// ReadableSamplesOf：取出一轮里所有 status == Read 的观测。
std::vector<const TamperViewSample*> ReadableSamplesOf(const TamperRound& round) {
    std::vector<const TamperViewSample*> readable;
    readable.reserve(round.views.size());
    for (const TamperViewSample& sample : round.views) {
        if (sample.status == TamperSampleStatus::Read) {
            readable.push_back(&sample);
        }
    }
    return readable;
}

// PersistentDisagreementBetween：
// - 判断某一对路径是否**每一轮都**不一致（T-02）。
// - 只在这一对至少可比对两轮时才成立：一轮无法把篡改与采样窗口内的竞态分开。
bool PersistentDisagreementBetween(const PairTally& tally) {
    return tally.comparableRounds >= 2
        && tally.disagreeingRounds == tally.comparableRounds;
}

// AnyPersistentDisagreementAcross：
// - 在两个分组之间找持续分歧。
bool AnyPersistentDisagreementAcross(
    const std::map<PathPair, PairTally>& tallies,
    const TamperPathGroup leftGroup,
    const TamperPathGroup rightGroup) {
    for (const auto& [pair, tally] : tallies) {
        const TamperPathGroup groupA = GroupOf(pair.first);
        const TamperPathGroup groupB = GroupOf(pair.second);
        const bool spansGroups =
            (groupA == leftGroup && groupB == rightGroup)
            || (groupA == rightGroup && groupB == leftGroup);
        if (spansGroups && PersistentDisagreementBetween(tally)) {
            return true;
        }
    }
    return false;
}

// AgreementAcross：
// - 两个分组之间**存在可比对且从不分歧**的一对。
bool AgreementAcross(
    const std::map<PathPair, PairTally>& tallies,
    const TamperPathGroup leftGroup,
    const TamperPathGroup rightGroup) {
    for (const auto& [pair, tally] : tallies) {
        const TamperPathGroup groupA = GroupOf(pair.first);
        const TamperPathGroup groupB = GroupOf(pair.second);
        const bool spansGroups =
            (groupA == leftGroup && groupB == rightGroup)
            || (groupA == rightGroup && groupB == leftGroup);
        if (spansGroups && tally.comparableRounds > 0 && tally.disagreeingRounds == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

TamperFinding AnalyzeTamperRounds(const std::vector<TamperRound>& rounds) {
    TamperFinding finding;
    if (!rounds.empty()) {
        finding.lastRoundStatus = rounds.back().views;
    }

    if (rounds.empty()) {
        finding.inconclusiveReason = "没有任何采样轮次。";
        return finding;
    }

    // 逐轮逐对累计。
    std::map<PathPair, PairTally> tallies;
    for (const TamperRound& round : rounds) {
        const std::vector<const TamperViewSample*> readable = ReadableSamplesOf(round);
        if (readable.size() >= 2) {
            ++finding.comparableRoundCount;
        }
        for (std::size_t leftIndex = 0; leftIndex < readable.size(); ++leftIndex) {
            for (std::size_t rightIndex = leftIndex + 1; rightIndex < readable.size();
                 ++rightIndex) {
                const TamperViewSample& leftSample = *readable[leftIndex];
                const TamperViewSample& rightSample = *readable[rightIndex];
                // 同一条路径在一轮里出现两次属于调用方的错误，跳过而不是把它
                // 和自己比出"一致"，那会凭空抬高可比对轮数。
                if (leftSample.path == rightSample.path) {
                    continue;
                }
                PairTally& tally = tallies[MakePair(leftSample.path, rightSample.path)];
                ++tally.comparableRounds;

                std::size_t firstOffset = 0;
                std::uint8_t leftByte = 0;
                std::uint8_t rightByte = 0;
                const std::size_t differingCount = CompareBytes(
                    leftSample.bytes, rightSample.bytes, firstOffset, leftByte, rightByte);
                if (differingCount == 0) {
                    continue;
                }
                ++tally.disagreeingRounds;
                if (!tally.firstDifferenceRecorded) {
                    tally.firstDifferenceRecorded = true;
                    tally.firstDifferingOffset = firstOffset;
                    tally.differingByteCount = differingCount;
                    tally.leftByte = leftByte;
                    tally.rightByte = rightByte;
                }
            }
        }
    }

    // T-01：可比对的路径不足两条时只能是"无法判定"。
    if (finding.comparableRoundCount == 0) {
        finding.inconclusiveReason =
            "没有任何一轮同时读到两条以上路径，无法互比。读失败不等于没有篡改。";
        return finding;
    }

    // 把持续分歧导出成清单，顺带把"出现过但不持续"的也记下来——它是竞态的
    // 典型形状，界面上要能看见，否则用户会以为本轮什么都没发生。
    for (const auto& [pair, tally] : tallies) {
        if (tally.disagreeingRounds == 0) {
            continue;
        }
        TamperDisagreement disagreement;
        disagreement.left = pair.first;
        disagreement.right = pair.second;
        disagreement.firstDifferingOffset = tally.firstDifferingOffset;
        disagreement.differingByteCount = tally.differingByteCount;
        disagreement.leftByte = tally.leftByte;
        disagreement.rightByte = tally.rightByte;
        disagreement.comparableRounds = tally.comparableRounds;
        disagreement.disagreeingRounds = tally.disagreeingRounds;
        finding.disagreements.push_back(disagreement);
    }

    // 重定向的判据不是"存在某一对 CPU↔DMA 分歧"，而是"**没有任何**一条 CPU 路径
    // 与 DMA 一致"。这两者不等价，差别正好是用户态钩子那个形状：R3 被钩住读到
    // 干净字节，而 R0 与 DMA 都读到真实的补丁——此时 R3↔DMA 确实持续分歧，但
    // R0↔DMA 一致，说明 CPU→内存这条路本身是好的，问题出在 R3 那一层。
    // 按"存在任一对分歧"判会把它误报成 SLAT 重定向，把排查方向从用户态钩子
    // 引到 hypervisor 上去。反过来说：只要还有一条 CPU 路径与 DMA 对得上，
    // 就不存在整体重定向。
    const bool cpuVsDma =
        AnyPersistentDisagreementAcross(
            tallies, TamperPathGroup::CpuMediated, TamperPathGroup::DmaMediated)
        && !AgreementAcross(
            tallies, TamperPathGroup::CpuMediated, TamperPathGroup::DmaMediated);
    const bool liveVsReference =
        AnyPersistentDisagreementAcross(
            tallies, TamperPathGroup::CpuMediated, TamperPathGroup::StaticReference)
        || AnyPersistentDisagreementAcross(
            tallies, TamperPathGroup::DmaMediated, TamperPathGroup::StaticReference);

    // 判定顺序本身是判据：CPU 与 DMA 的分歧必须排在"内存与参考不同"前面。
    // SLAT 隐藏的典型形状恰恰是 CPU 侧与参考完全一致（隐藏者给你看原始字节），
    // 只有 DMA 侧能看到真实的改动。若先判"内存与参考不同"，这种情况会因为
    // CPU 侧与参考一致而被判成"一致"，也就是被隐藏者骗过去。
    if (cpuVsDma) {
        finding.verdict = TamperVerdict::CpuViewRedirected;
        finding.cpuMatchesStaticReference = AgreementAcross(
            tallies, TamperPathGroup::CpuMediated, TamperPathGroup::StaticReference);
        return finding;
    }

    // R3 与 R0 的持续分歧：两者都经过 CPU，差异出在用户态那一段路上。
    const auto userVsKernelVirtual = tallies.find(
        MakePair(TamperReadPath::UserModeVirtual, TamperReadPath::KernelVirtual));
    const auto userVsKernelPhysical = tallies.find(
        MakePair(TamperReadPath::UserModeVirtual, TamperReadPath::KernelPhysical));
    const bool userModeDiffers =
        (userVsKernelVirtual != tallies.end()
         && PersistentDisagreementBetween(userVsKernelVirtual->second))
        || (userVsKernelPhysical != tallies.end()
            && PersistentDisagreementBetween(userVsKernelPhysical->second));
    if (userModeDiffers) {
        finding.verdict = TamperVerdict::UserModeViewDiffers;
        return finding;
    }

    if (liveVsReference) {
        finding.verdict = TamperVerdict::LiveDiffersFromReference;
        return finding;
    }

    // 还有持续分歧却不落在上面任何模式上（例如两条 CPU 路径之间持续不同）。
    for (const auto& [pair, tally] : tallies) {
        (void)pair;
        if (PersistentDisagreementBetween(tally)) {
            finding.verdict = TamperVerdict::UnexplainedDisagreement;
            return finding;
        }
    }

    // 出现过分歧但不是每轮都出现：按 T-02 不升为结论，但也不能报"一致"——
    // 它同样可能是一次成功的篡改恰好只在其中几轮可见。
    if (!finding.disagreements.empty()) {
        finding.verdict = TamperVerdict::Inconclusive;
        finding.inconclusiveReason =
            "存在分歧但并非每一轮都出现，无法与采样窗口内的正常写入区分，请增加轮次重试。";
        return finding;
    }

    finding.verdict = TamperVerdict::Consistent;
    return finding;
}

}  // namespace Ksword::Evidence
