// 内存内容交叉视图（shared/evidence/MemoryTamperCrossView.h）的离线测试。
//
// 这一层属于**判错了不会报错、只会安静地说"干净"**的那一类：它的输出会被当成
// "这段内存有没有被隐蔽修改"的答案，而一次错误的 Consistent 与一次成功的隐藏
// 在界面上完全同形。所以判定矩阵必须被穷举，尤其是三条硬规则：
//
//   T-01 读失败 / 通道不可用 / 未覆盖，都不是"没被篡改"；
//   T-02 一次性的不一致不升为结论（内存随时可能正在被合法写入）；
//   T-03 读不到的字节不补 00 参与比较。
//
// 另外单测一条**判定顺序**：SLAT 隐藏的典型形状是 CPU 侧与磁盘参考完全一致、
// 只有 DMA 侧看得到改动。若先判"内存与参考不同"，这种情况会因为 CPU 侧与参考
// 一致而被判成"一致"——也就是被隐藏者骗过去。顺序错了不会有任何编译或运行报错。

#include "TestSupport.h"

#include "../shared/evidence/MemoryTamperCrossView.h"

#include <cstdint>
#include <vector>

namespace {

using Ksword::Evidence::AnalyzeTamperRounds;
using Ksword::Evidence::GroupOf;
using Ksword::Evidence::TamperPathGroup;
using Ksword::Evidence::TamperReadPath;
using Ksword::Evidence::TamperRound;
using Ksword::Evidence::TamperSampleStatus;
using Ksword::Evidence::TamperVerdict;
using Ksword::Evidence::TamperViewSample;

// 三段有区分度的字节：全零和全 FF 在这类比对里最容易掩盖错误，一律不用。
const std::vector<std::uint8_t> kClean{0x48, 0x89, 0x5C, 0x24, 0x08, 0x57};
const std::vector<std::uint8_t> kPatched{0xE9, 0x11, 0x22, 0x33, 0x44, 0x57};
const std::vector<std::uint8_t> kOther{0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};

TamperViewSample ReadSample(const TamperReadPath path, const std::vector<std::uint8_t>& bytes) {
    TamperViewSample sample;
    sample.path = path;
    sample.status = TamperSampleStatus::Read;
    sample.bytes = bytes;
    return sample;
}

TamperViewSample FailedSample(const TamperReadPath path, const TamperSampleStatus status) {
    TamperViewSample sample;
    sample.path = path;
    sample.status = status;
    return sample;
}

// RepeatRound：把同一轮内容重复 n 次，用来构造"每轮都一样"的持续分歧。
std::vector<TamperRound> RepeatRound(const TamperRound& round, const int times) {
    return std::vector<TamperRound>(static_cast<std::size_t>(times), round);
}

// ------------------------------------------------------------
// 一、分组：分组错了，后面所有判定都跟着错，而且不会报错。
// ------------------------------------------------------------
void TestPathGrouping(KswordTests::Suite& suite) {
    suite.expect(GroupOf(TamperReadPath::UserModeVirtual) == TamperPathGroup::CpuMediated,
        L"tamper: R3 read is CPU-mediated");
    suite.expect(GroupOf(TamperReadPath::KernelVirtual) == TamperPathGroup::CpuMediated,
        L"tamper: R0 virtual read is CPU-mediated");
    // 这一条最容易被想当然地归到 DMA 那一组：按物理地址读仍然是 CPU 在访存，
    // 照样会被 SLAT 骗过。归错组会让 EPT 隐藏变得检测不到。
    suite.expect(GroupOf(TamperReadPath::KernelPhysical) == TamperPathGroup::CpuMediated,
        L"tamper: R0 physical read is still CPU-mediated, not DMA");
    // HVM 更容易归错：它的接口叫 "ring -1 memory access"，听上去就在 VMX root。
    // 实现（hvm_memory.c）跑在 PASSIVE_LEVEL 的驱动上下文里、不进 VMX root，
    // 照样吃 SLAT。归进 DMA 组会让「CPU 视图被重定向」凭空多出一条不成立的证据，
    // 而且不会有任何报错。它独立的是别的：不调用文档化的内存管理器例程。
    suite.expect(GroupOf(TamperReadPath::HvmPrivateWindow) == TamperPathGroup::CpuMediated,
        L"tamper: the HVM private window is CPU-mediated despite its ring -1 name");
    suite.expect(GroupOf(TamperReadPath::DmaPhysical) == TamperPathGroup::DmaMediated,
        L"tamper: DDMA read is the only DMA-mediated path");
    suite.expect(GroupOf(TamperReadPath::ImageSectionClean) == TamperPathGroup::StaticReference,
        L"tamper: the section object's clean pages are a static reference");
    suite.expect(GroupOf(TamperReadPath::OnDiskImage) == TamperPathGroup::StaticReference,
        L"tamper: the on-disk image is a static reference");
}

// ------------------------------------------------------------
// 二、T-01：读不到不是"干净"。
// ------------------------------------------------------------
void TestFailuresAreNeverClean(KswordTests::Suite& suite) {
    suite.expect(AnalyzeTamperRounds({}).verdict == TamperVerdict::Inconclusive,
        L"tamper: no rounds at all is inconclusive, never consistent");

    // 只有一条路径读成功：无从互比。
    TamperRound singleView;
    singleView.views.push_back(ReadSample(TamperReadPath::KernelVirtual, kClean));
    singleView.views.push_back(FailedSample(TamperReadPath::DmaPhysical, TamperSampleStatus::Unavailable));
    singleView.views.push_back(FailedSample(TamperReadPath::OnDiskImage, TamperSampleStatus::OutOfCoverage));
    const auto singleFinding = AnalyzeTamperRounds(RepeatRound(singleView, 3));
    suite.expect(singleFinding.verdict == TamperVerdict::Inconclusive,
        L"tamper: one readable path across every round is inconclusive");
    suite.expect(singleFinding.comparableRoundCount == 0,
        L"tamper: a round with one readable path counts as zero comparable rounds");
    suite.expect(!singleFinding.inconclusiveReason.empty(),
        L"tamper: an inconclusive verdict always states which condition blocked it");

    // 四种非 Read 状态都不得被当成可比对的数据。
    for (const TamperSampleStatus status : {
             TamperSampleStatus::NotAttempted,
             TamperSampleStatus::Unavailable,
             TamperSampleStatus::Failed,
             TamperSampleStatus::OutOfCoverage}) {
        TamperRound round;
        round.views.push_back(FailedSample(TamperReadPath::KernelVirtual, status));
        round.views.push_back(FailedSample(TamperReadPath::DmaPhysical, status));
        const auto finding = AnalyzeTamperRounds(RepeatRound(round, 3));
        suite.expect(finding.verdict == TamperVerdict::Inconclusive,
            L"tamper: non-Read statuses never become comparable data");
    }

    // 最关键的一条：DMA 不可用时，两条 CPU 路径一致**不足以**说明没有 SLAT 隐藏，
    // 因为两条都会被同一个隐藏者骗过。这里的 Consistent 只描述"已比对的路径一致"，
    // 所以结论文本里必须能看出 DMA 缺席——由 lastRoundStatus 承载。
    TamperRound noDma;
    noDma.views.push_back(ReadSample(TamperReadPath::UserModeVirtual, kClean));
    noDma.views.push_back(ReadSample(TamperReadPath::KernelVirtual, kClean));
    noDma.views.push_back(FailedSample(TamperReadPath::DmaPhysical, TamperSampleStatus::Unavailable));
    const auto noDmaFinding = AnalyzeTamperRounds(RepeatRound(noDma, 3));
    suite.expect(noDmaFinding.verdict == TamperVerdict::Consistent,
        L"tamper: two agreeing CPU paths are reported consistent");
    suite.expect(noDmaFinding.lastRoundStatus.size() == 3,
        L"tamper: every configured path is carried in lastRoundStatus, including the missing one");
    bool dmaStatusVisible = false;
    for (const TamperViewSample& sample : noDmaFinding.lastRoundStatus) {
        if (sample.path == TamperReadPath::DmaPhysical
            && sample.status == TamperSampleStatus::Unavailable) {
            dmaStatusVisible = true;
        }
    }
    suite.expect(dmaStatusVisible,
        L"tamper: the DMA path's unavailability stays visible next to a consistent verdict");
}

// ------------------------------------------------------------
// 三、T-02：一次性的不一致不升为结论。
// ------------------------------------------------------------
void TestTransientDisagreementIsNotAVerdict(KswordTests::Suite& suite) {
    TamperRound agreeing;
    agreeing.views.push_back(ReadSample(TamperReadPath::KernelVirtual, kClean));
    agreeing.views.push_back(ReadSample(TamperReadPath::DmaPhysical, kClean));

    TamperRound disagreeing;
    disagreeing.views.push_back(ReadSample(TamperReadPath::KernelVirtual, kClean));
    disagreeing.views.push_back(ReadSample(TamperReadPath::DmaPhysical, kPatched));

    // 三轮里只有一轮不一致：与"采样窗口里正好有一次正常写入"无法区分。
    const std::vector<TamperRound> mixed{agreeing, disagreeing, agreeing};
    const auto mixedFinding = AnalyzeTamperRounds(mixed);
    suite.expect(mixedFinding.verdict == TamperVerdict::Inconclusive,
        L"tamper: a disagreement seen in only some rounds does not become a verdict");
    suite.expect(!mixedFinding.inconclusiveReason.empty(),
        L"tamper: the transient case explains itself rather than looking clean");
    // 但它必须**出现在清单里**：界面上看不见的话，用户会以为这一轮什么都没发生。
    suite.expect(mixedFinding.disagreements.size() == 1,
        L"tamper: a transient disagreement is still listed as an observation");
    suite.expect(mixedFinding.disagreements.front().comparableRounds == 3
        && mixedFinding.disagreements.front().disagreeingRounds == 1,
        L"tamper: the listing records how many rounds disagreed out of how many comparable");

    // 每一轮都不一致才升为结论。
    const auto persistentFinding = AnalyzeTamperRounds(RepeatRound(disagreeing, 3));
    suite.expect(persistentFinding.verdict == TamperVerdict::CpuViewRedirected,
        L"tamper: a disagreement present in every round becomes a verdict");

    // 只有一轮时不成立：一轮无法把篡改与竞态分开，哪怕这一轮确实不一致。
    const auto singleRoundFinding = AnalyzeTamperRounds(RepeatRound(disagreeing, 1));
    suite.expect(singleRoundFinding.verdict == TamperVerdict::Inconclusive,
        L"tamper: a single round is never enough to separate tampering from a write race");
}

// ------------------------------------------------------------
// 四、判定顺序：SLAT 隐藏必须排在"内存与参考不同"之前。
// ------------------------------------------------------------
void TestRedirectionOutranksReferenceDiff(KswordTests::Suite& suite) {
    // 隐藏者想要的形状：CPU 侧读到的正是磁盘上那份原始字节，DMA 侧才是真的。
    TamperRound hidden;
    hidden.views.push_back(ReadSample(TamperReadPath::UserModeVirtual, kClean));
    hidden.views.push_back(ReadSample(TamperReadPath::KernelVirtual, kClean));
    hidden.views.push_back(ReadSample(TamperReadPath::DmaPhysical, kPatched));
    hidden.views.push_back(ReadSample(TamperReadPath::OnDiskImage, kClean));

    const auto hiddenFinding = AnalyzeTamperRounds(RepeatRound(hidden, 3));
    suite.expect(hiddenFinding.verdict == TamperVerdict::CpuViewRedirected,
        L"tamper: CPU-vs-DMA disagreement outranks every other pattern");
    suite.expect(hiddenFinding.cpuMatchesStaticReference,
        L"tamper: the finding records that the CPU side matched the clean reference");

    // 普通补丁：所有活体路径都看到改动，只与静态参考不同。这种才是
    // LiveDiffersFromReference，它不代表有人在隐藏。
    TamperRound plainPatch;
    plainPatch.views.push_back(ReadSample(TamperReadPath::KernelVirtual, kPatched));
    plainPatch.views.push_back(ReadSample(TamperReadPath::DmaPhysical, kPatched));
    plainPatch.views.push_back(ReadSample(TamperReadPath::OnDiskImage, kClean));
    const auto plainFinding = AnalyzeTamperRounds(RepeatRound(plainPatch, 3));
    suite.expect(plainFinding.verdict == TamperVerdict::LiveDiffersFromReference,
        L"tamper: a patch every live path can see is not a redirection");

    // 两种形状必须给出不同结论：混同就等于分不清"被改了"和"被改了还被藏起来"。
    suite.expect(hiddenFinding.verdict != plainFinding.verdict,
        L"tamper: hidden and plain patches never collapse into the same verdict");
}

// ------------------------------------------------------------
// 五、用户态视图分歧。
// ------------------------------------------------------------
void TestUserModeHookPattern(KswordTests::Suite& suite) {
    TamperRound hooked;
    hooked.views.push_back(ReadSample(TamperReadPath::UserModeVirtual, kClean));
    hooked.views.push_back(ReadSample(TamperReadPath::KernelVirtual, kPatched));
    hooked.views.push_back(ReadSample(TamperReadPath::DmaPhysical, kPatched));

    const auto finding = AnalyzeTamperRounds(RepeatRound(hooked, 3));
    // R0 与 DMA 一致，所以不是重定向；差异只出在 R3 这条路上。
    suite.expect(finding.verdict == TamperVerdict::UserModeViewDiffers,
        L"tamper: R3 disagreeing while R0 and DMA agree is a user-mode hook, not a redirection");

    // 反过来：R3 与 R0 一致而 DMA 不同，仍然是重定向，不能因为"R3 也参与了"
    // 就降级成用户态钩子。
    TamperRound redirected;
    redirected.views.push_back(ReadSample(TamperReadPath::UserModeVirtual, kClean));
    redirected.views.push_back(ReadSample(TamperReadPath::KernelVirtual, kClean));
    redirected.views.push_back(ReadSample(TamperReadPath::DmaPhysical, kPatched));
    suite.expect(AnalyzeTamperRounds(RepeatRound(redirected, 3)).verdict
            == TamperVerdict::CpuViewRedirected,
        L"tamper: R3 agreeing with R0 does not downgrade a CPU-vs-DMA disagreement");
}

// ------------------------------------------------------------
// 六、T-03 与比较本身的边界。
// ------------------------------------------------------------
void TestComparisonEdges(KswordTests::Suite& suite) {
    // 长度不等：短的那一截差额必须计入差异，不能只比公共前缀然后报"一致"。
    TamperRound shortRead;
    shortRead.views.push_back(ReadSample(TamperReadPath::KernelVirtual, kClean));
    shortRead.views.push_back(ReadSample(
        TamperReadPath::DmaPhysical,
        std::vector<std::uint8_t>(kClean.begin(), kClean.begin() + 3)));
    const auto shortFinding = AnalyzeTamperRounds(RepeatRound(shortRead, 3));
    suite.expect(shortFinding.verdict == TamperVerdict::CpuViewRedirected,
        L"tamper: a path that read fewer bytes counts as disagreeing, not as agreeing");
    suite.expect(!shortFinding.disagreements.empty()
        && shortFinding.disagreements.front().differingByteCount == 3,
        L"tamper: the length gap is counted into the differing byte count");

    // 第一处不同的偏移必须精确：界面靠它定位，报错了用户会去看错的地方。
    TamperRound offsetRound;
    std::vector<std::uint8_t> tweaked = kClean;
    tweaked[4] = 0x90;
    offsetRound.views.push_back(ReadSample(TamperReadPath::KernelVirtual, kClean));
    offsetRound.views.push_back(ReadSample(TamperReadPath::DmaPhysical, tweaked));
    const auto offsetFinding = AnalyzeTamperRounds(RepeatRound(offsetRound, 2));
    suite.expect(!offsetFinding.disagreements.empty(),
        L"tamper: a single differing byte is still reported");
    suite.expect(offsetFinding.disagreements.front().firstDifferingOffset == 4,
        L"tamper: the first differing offset is exact");
    suite.expect(offsetFinding.disagreements.front().differingByteCount == 1,
        L"tamper: a single differing byte counts as exactly one");
    suite.expect(offsetFinding.disagreements.front().leftByte == kClean[4]
        && offsetFinding.disagreements.front().rightByte == 0x90,
        L"tamper: both sides' bytes at the first difference are carried out");

    // 完全一致的多轮：结论是 Consistent，且分歧清单为空。
    TamperRound clean;
    clean.views.push_back(ReadSample(TamperReadPath::KernelVirtual, kClean));
    clean.views.push_back(ReadSample(TamperReadPath::DmaPhysical, kClean));
    clean.views.push_back(ReadSample(TamperReadPath::OnDiskImage, kClean));
    const auto cleanFinding = AnalyzeTamperRounds(RepeatRound(clean, 3));
    suite.expect(cleanFinding.verdict == TamperVerdict::Consistent,
        L"tamper: every path agreeing every round is consistent");
    suite.expect(cleanFinding.disagreements.empty(),
        L"tamper: a consistent verdict lists no disagreements");
    suite.expect(cleanFinding.comparableRoundCount == 3,
        L"tamper: every round with two readable paths counts as comparable");

    // 两条 CPU 路径持续互相不同、且没有 DMA 和参考：落到"未归类"，不许报一致。
    TamperRound cpuOnly;
    cpuOnly.views.push_back(ReadSample(TamperReadPath::KernelVirtual, kClean));
    cpuOnly.views.push_back(ReadSample(TamperReadPath::KernelPhysical, kOther));
    const auto cpuOnlyFinding = AnalyzeTamperRounds(RepeatRound(cpuOnly, 3));
    suite.expect(cpuOnlyFinding.verdict == TamperVerdict::UnexplainedDisagreement,
        L"tamper: two CPU paths persistently disagreeing is reported, not swallowed");
}

}  // namespace

int RunMemoryTamperCrossViewTests() {
    KswordTests::Suite suite(L"T memory tamper cross-view");
    TestPathGrouping(suite);
    TestFailuresAreNeverClean(suite);
    TestTransientDisagreementIsNotAVerdict(suite);
    TestRedirectionOutranksReferenceDiff(suite);
    TestUserModeHookPattern(suite);
    TestComparisonEdges(suite);
    suite.report();
    return suite.failures();
}
