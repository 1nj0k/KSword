// 基于 DMA 的进程操作计划层（shared/evidence/DmaProcessOpPlan.h）的离线测试。
//
// 这一层算错的后果与别处不同：它**直接把字节写进别人的进程**。空隙找错就是把真
// 代码覆盖掉、目标当场死掉而且是我们弄死的；范围算错会越过页边界，而 DMA 的粒度
// 就是一页，越界那一页可能是任何东西。这些都不会有任何错误码。
//
// 所以三类断言必须穷举：
//   * 空隙判据（只认 0x00/0xCC、不跨填充字节、长度不足必须拒绝而不是凑合给）；
//   * 备份（备份缺了就没法还原，而 DMA 改的是真页，不是影子页）；
//   * 读回校验（DMA 写入没有任何自证，驱动报 OK 不等于物理页真的变了）。

#include "TestSupport.h"

#include "../shared/evidence/DmaProcessOpPlan.h"

#include <cstdint>
#include <vector>

namespace {

using Ksword::Evidence::DmaOpPlanStatus;
using Ksword::Evidence::FindLargestCodeCave;
using Ksword::Evidence::kDmaOpMinCaveBytes;
using Ksword::Evidence::kDmaOpPageBytes;
using Ksword::Evidence::PlanBytesAtOffset;
using Ksword::Evidence::PlanPayloadIntoCave;
using Ksword::Evidence::UndefinedInstructionBytes;
using Ksword::Evidence::VerifyWriteReadback;

constexpr std::size_t kPage = static_cast<std::size_t>(kDmaOpPageBytes);

// MakePage：一页"真代码"，用一个不重复的模式填，确保不会被误认成填充。
std::vector<std::uint8_t> MakePage() {
    std::vector<std::uint8_t> page(kPage, 0U);
    for (std::size_t i = 0U; i < kPage; ++i) {
        // 刻意避开 0x00 与 0xCC，否则整页都会被当成空隙。
        const std::uint8_t v = static_cast<std::uint8_t>((i * 7U + 1U) & 0xFFU);
        page[i] = (v == 0x00U || v == 0xCCU) ? 0x55U : v;
    }
    return page;
}

// 在页里刻一段填充。
void CarveCave(std::vector<std::uint8_t>& page, std::size_t offset,
               std::size_t length, std::uint8_t fill) {
    for (std::size_t i = 0U; i < length; ++i) {
        page[offset + i] = fill;
    }
}

// ------------------------------------------------------------
// 一、空隙判据。
// ------------------------------------------------------------
void TestCaveDiscovery(KswordTests::Suite& suite) {
    {
        std::vector<std::uint8_t> page = MakePage();
        CarveCave(page, 1000U, 200U, 0x00U);
        const auto cave = FindLargestCodeCave(page, kDmaOpMinCaveBytes);
        suite.expect(cave.found && cave.offset == 1000U && cave.length == 200U,
            L"dma op: the zero-filled cave is found at its exact offset and length");
        suite.expect(cave.fillByte == 0x00U,
            L"dma op: the cave reports which byte fills it");
    }
    {
        std::vector<std::uint8_t> page = MakePage();
        CarveCave(page, 2048U, 128U, 0xCCU);
        const auto cave = FindLargestCodeCave(page, kDmaOpMinCaveBytes);
        suite.expect(cave.found && cave.offset == 2048U && cave.length == 128U,
            L"dma op: an int3-filled cave is found too");
    }
    {
        // 两段，取最长的那段。
        std::vector<std::uint8_t> page = MakePage();
        CarveCave(page, 100U, 80U, 0x00U);
        CarveCave(page, 500U, 300U, 0x00U);
        const auto cave = FindLargestCodeCave(page, kDmaOpMinCaveBytes);
        suite.expect(cave.found && cave.offset == 500U && cave.length == 300U,
            L"dma op: the longest cave wins when several qualify");
    }
    {
        // 0x00 与 0xCC 相邻**不能**并成一段：中间那个边界上的字节属于另一种
        // 填充，合并等于把两段不同来源的区域当成一段连续空隙。
        std::vector<std::uint8_t> page = MakePage();
        CarveCave(page, 300U, 100U, 0x00U);
        CarveCave(page, 400U, 100U, 0xCCU);
        const auto cave = FindLargestCodeCave(page, kDmaOpMinCaveBytes);
        suite.expect(cave.found && cave.length == 100U,
            L"dma op: runs of different padding bytes are not merged into one cave");
    }
    {
        // 长度不足时必须拒绝，而不是把那段最长的交出去。
        std::vector<std::uint8_t> page = MakePage();
        CarveCave(page, 800U, kDmaOpMinCaveBytes - 1U, 0x00U);
        const auto cave = FindLargestCodeCave(page, kDmaOpMinCaveBytes);
        suite.expect(!cave.found,
            L"dma op: a cave one byte short of the minimum is refused, not returned anyway");
        suite.expect(cave.length == 0U,
            L"dma op: a refused cave carries no length a caller could mistake for usable");
    }
    {
        // 恰好等于下限：边界的另一侧必须通过。
        std::vector<std::uint8_t> page = MakePage();
        CarveCave(page, 800U, kDmaOpMinCaveBytes, 0x00U);
        const auto cave = FindLargestCodeCave(page, kDmaOpMinCaveBytes);
        suite.expect(cave.found && cave.length == kDmaOpMinCaveBytes,
            L"dma op: a cave exactly at the minimum is accepted");
    }
    {
        // 整页真代码，没有填充。
        const std::vector<std::uint8_t> page = MakePage();
        suite.expect(!FindLargestCodeCave(page, kDmaOpMinCaveBytes).found,
            L"dma op: a page of real code yields no cave");
    }
    {
        // 其它重复字节**不算**填充。一段全 0x41（'A'）很可能是字符串数据，
        // 认成空隙会把数据覆盖掉。
        std::vector<std::uint8_t> page = MakePage();
        CarveCave(page, 600U, 400U, 0x41U);
        suite.expect(!FindLargestCodeCave(page, kDmaOpMinCaveBytes).found,
            L"dma op: a long run of some other repeated byte is not treated as padding");
    }
    {
        // 空隙贴着页尾。
        std::vector<std::uint8_t> page = MakePage();
        CarveCave(page, kPage - 128U, 128U, 0x00U);
        const auto cave = FindLargestCodeCave(page, kDmaOpMinCaveBytes);
        suite.expect(cave.found && cave.offset == kPage - 128U && cave.length == 128U,
            L"dma op: a cave that ends exactly at the page end is found with the right length");
    }
}

// ------------------------------------------------------------
// 二、排入空隙 + 备份。
// ------------------------------------------------------------
void TestPlanIntoCave(KswordTests::Suite& suite) {
    std::vector<std::uint8_t> page = MakePage();
    CarveCave(page, 1500U, 256U, 0x00U);
    const std::vector<std::uint8_t> payload{0x90U, 0x48U, 0x31U, 0xC0U, 0xC3U};

    const auto plan = PlanPayloadIntoCave(page, payload, kDmaOpMinCaveBytes);
    suite.expect(plan.status == DmaOpPlanStatus::Ok,
        L"dma op: a payload that fits the cave plans successfully");
    suite.expect(plan.offsetInPage == 1500U,
        L"dma op: the payload is placed at the cave's offset");
    suite.expect(plan.bytesToWrite == payload,
        L"dma op: the planned bytes are exactly the payload");
    // 备份必须与写入等长，且必须是**那一段**原始字节。
    suite.expect(plan.originalBytes.size() == payload.size(),
        L"dma op: the backup is the same length as the write");
    bool backupCorrect = true;
    for (std::size_t i = 0U; i < payload.size(); ++i) {
        if (plan.originalBytes[i] != page[1500U + i]) {
            backupCorrect = false;
        }
    }
    suite.expect(backupCorrect,
        L"dma op: the backup holds the bytes actually about to be overwritten");

    // 载荷比空隙长：必须拒绝。挤不下却硬写就会盖到空隙后面的真代码上。
    std::vector<std::uint8_t> page2 = MakePage();
    CarveCave(page2, 2000U, 100U, 0x00U);
    const std::vector<std::uint8_t> big(150U, 0x90U);
    suite.expect(PlanPayloadIntoCave(page2, big, kDmaOpMinCaveBytes).status
            == DmaOpPlanStatus::NoCave,
        L"dma op: a payload longer than every cave is refused");

    // 空载荷、错误页长。
    suite.expect(PlanPayloadIntoCave(page, {}, kDmaOpMinCaveBytes).status
            == DmaOpPlanStatus::EmptyPayload,
        L"dma op: an empty payload is refused");
    suite.expect(PlanPayloadIntoCave({}, payload, kDmaOpMinCaveBytes).status
            == DmaOpPlanStatus::PageBytesUnavailable,
        L"dma op: planning without the page contents is refused, since there is no backup");
    suite.expect(PlanPayloadIntoCave(std::vector<std::uint8_t>(100U, 0U), payload,
            kDmaOpMinCaveBytes).status == DmaOpPlanStatus::PageBytesWrongSize,
        L"dma op: page contents of the wrong length are refused");
}

// ------------------------------------------------------------
// 三、定点写入与页边界。
// ------------------------------------------------------------
void TestPlanAtOffset(KswordTests::Suite& suite) {
    const std::vector<std::uint8_t> page = MakePage();
    const std::vector<std::uint8_t> ud2 = UndefinedInstructionBytes();

    suite.expect(ud2.size() == 2U && ud2[0] == 0x0FU && ud2[1] == 0x0BU,
        L"dma op: the undefined instruction is exactly 0F 0B (UD2)");
    // 刻意不是 0xCC：int3 会让附了调试器的目标停下而不是退出，而"停住了"和
    // "退出了"在界面上会被读成同一个结果。
    suite.expect(ud2[0] != 0xCCU,
        L"dma op: int3 is deliberately not used, since a debugger would swallow it");

    const auto plan = PlanBytesAtOffset(page, 1234U, ud2);
    suite.expect(plan.status == DmaOpPlanStatus::Ok && plan.offsetInPage == 1234U,
        L"dma op: a targeted write plans at the requested offset");
    suite.expect(plan.originalBytes.size() == 2U
        && plan.originalBytes[0] == page[1234U]
        && plan.originalBytes[1] == page[1235U],
        L"dma op: a targeted write backs up exactly what it overwrites");

    // 页边界两侧都要测。
    suite.expect(PlanBytesAtOffset(page, kPage - 2U, ud2).status == DmaOpPlanStatus::Ok,
        L"dma op: a write ending exactly at the page end is allowed");
    suite.expect(PlanBytesAtOffset(page, kPage - 1U, ud2).status
            == DmaOpPlanStatus::WouldCrossPage,
        L"dma op: a write that would cross the page boundary by one byte is refused");
    suite.expect(PlanBytesAtOffset(page, kPage, ud2).status
            == DmaOpPlanStatus::OffsetOutOfPage,
        L"dma op: an offset at the page end is out of the page");

    // 极大长度：越界判据必须写成"剩余空间不够"，写成"起点加长度大于页长"会
    // 在这里整型回绕，回绕后的比较恰好会通过。
    const std::vector<std::uint8_t> huge(kPage, 0x90U);
    suite.expect(PlanBytesAtOffset(page, 4000U, huge).status
            == DmaOpPlanStatus::WouldCrossPage,
        L"dma op: a length that would wrap the offset arithmetic is still refused");
}

// ------------------------------------------------------------
// 四、读回校验 —— DMA 写入没有任何自证。
// ------------------------------------------------------------
void TestReadbackVerification(KswordTests::Suite& suite) {
    const std::vector<std::uint8_t> intended{0x0FU, 0x0BU, 0x90U, 0x90U};

    const auto good = VerifyWriteReadback(intended, intended);
    suite.expect(good.matched && good.comparedBytes == 4U,
        L"dma op: an identical readback verifies");

    std::vector<std::uint8_t> wrong = intended;
    wrong[2] = 0xCCU;
    const auto bad = VerifyWriteReadback(intended, wrong);
    suite.expect(!bad.matched,
        L"dma op: a differing readback does not verify");
    suite.expect(bad.firstMismatchOffset == 2U && bad.expectedByte == 0x90U
        && bad.actualByte == 0xCCU,
        L"dma op: the first mismatch reports its offset and both bytes");

    // 读回太短与内容不同是两件事：前者说明读回通路有问题，后者说明写入没落地。
    const auto shortRead = VerifyWriteReadback(intended, {0x0FU, 0x0BU});
    suite.expect(!shortRead.matched && shortRead.readbackTooShort,
        L"dma op: a short readback is reported as short, not merely as different");
    suite.expect(shortRead.comparedBytes == 2U,
        L"dma op: a short readback reports how much could be compared");

    // 前缀就不对时，即使读少了也要指出那处不同——两个问题可能同时存在。
    const auto shortAndWrong = VerifyWriteReadback(intended, {0x0FU, 0xFFU});
    suite.expect(shortAndWrong.readbackTooShort && shortAndWrong.firstMismatchOffset == 1U,
        L"dma op: a readback that is both short and wrong reports both facts");

    // 什么都没写却报"验证通过"是最糟的：一次空操作会看起来成功了。
    suite.expect(!VerifyWriteReadback({}, {}).matched,
        L"dma op: verifying an empty write never reports success");
}

}  // namespace

int RunDmaProcessOpPlanTests() {
    KswordTests::Suite suite(L"DMA process op plan");
    TestCaveDiscovery(suite);
    TestPlanIntoCave(suite);
    TestPlanAtOffset(suite);
    TestReadbackVerification(suite);
    suite.report();
    return suite.failures();
}
