#include "DmaProcessOpPlan.h"

#include <algorithm>

namespace Ksword::Evidence {

const char* DmaOpPlanStatusName(const DmaOpPlanStatus status) noexcept {
    switch (status) {
        case DmaOpPlanStatus::Ok: return "计划成立";
        case DmaOpPlanStatus::EmptyPayload: return "载荷为空";
        case DmaOpPlanStatus::PayloadTooLarge: return "载荷放不下";
        case DmaOpPlanStatus::PageBytesUnavailable: return "没有目标页的当前内容";
        case DmaOpPlanStatus::PageBytesWrongSize: return "目标页内容长度不是一页";
        case DmaOpPlanStatus::OffsetOutOfPage: return "页内偏移超出页范围";
        case DmaOpPlanStatus::WouldCrossPage: return "写入范围会越过页边界";
        case DmaOpPlanStatus::NoCave: return "页内没有足够长的空隙";
    }
    return "未知状态";
}

namespace {

// 只认这两种填充。理由见头文件：其它重复字节可能是真数据。
bool IsPaddingByte(const std::uint8_t value) noexcept {
    return value == 0x00U || value == 0xCCU;
}

}  // namespace

DmaCodeCave FindLargestCodeCave(
    const std::vector<std::uint8_t>& pageBytes,
    const std::size_t minLength) {
    DmaCodeCave best;
    if (pageBytes.empty() || minLength == 0U) {
        return best;
    }

    std::size_t runStart = 0U;
    std::size_t runLength = 0U;
    std::uint8_t runByte = 0U;

    // 逐字节扫。只有"同一个填充字节连续出现"才算一段空隙：0x00 与 0xCC 交替
    // 出现的区域不是填充，把它们并成一段会把中间的真内容一起覆盖掉。
    for (std::size_t index = 0U; index <= pageBytes.size(); ++index) {
        const bool continues =
            (index < pageBytes.size())
            && IsPaddingByte(pageBytes[index])
            && (runLength == 0U || pageBytes[index] == runByte);

        if (continues) {
            if (runLength == 0U) {
                runStart = index;
                runByte = pageBytes[index];
            }
            ++runLength;
            continue;
        }

        if (runLength > best.length) {
            best.found = true;
            best.offset = runStart;
            best.length = runLength;
            best.fillByte = runByte;
        }
        // 当前字节本身可能是另一段空隙的开头。
        if (index < pageBytes.size() && IsPaddingByte(pageBytes[index])) {
            runStart = index;
            runByte = pageBytes[index];
            runLength = 1U;
        } else {
            runLength = 0U;
        }
    }

    // 长度不足时**不返回**那段最长的。返回它等于让调用方拿着一段不够长的空隙
    // 去写，与直接覆盖真代码没有区别，而调用方从返回值里看不出这个区别。
    if (best.length < minLength) {
        return DmaCodeCave{};
    }
    return best;
}

DmaWritePlan PlanPayloadIntoCave(
    const std::vector<std::uint8_t>& pageBytes,
    const std::vector<std::uint8_t>& payload,
    const std::size_t minCaveBytes) {
    DmaWritePlan plan;

    if (pageBytes.empty()) {
        plan.status = DmaOpPlanStatus::PageBytesUnavailable;
        return plan;
    }
    if (pageBytes.size() != static_cast<std::size_t>(kDmaOpPageBytes)) {
        plan.status = DmaOpPlanStatus::PageBytesWrongSize;
        return plan;
    }
    if (payload.empty()) {
        plan.status = DmaOpPlanStatus::EmptyPayload;
        return plan;
    }
    if (payload.size() > static_cast<std::size_t>(kDmaOpPageBytes)) {
        plan.status = DmaOpPlanStatus::PayloadTooLarge;
        return plan;
    }

    const std::size_t required = (std::max)(minCaveBytes, payload.size());
    const DmaCodeCave cave = FindLargestCodeCave(pageBytes, required);
    if (!cave.found) {
        plan.status = DmaOpPlanStatus::NoCave;
        return plan;
    }
    if (payload.size() > cave.length) {
        plan.status = DmaOpPlanStatus::PayloadTooLarge;
        return plan;
    }

    plan.status = DmaOpPlanStatus::Ok;
    plan.offsetInPage = cave.offset;
    plan.bytesToWrite = payload;
    plan.originalBytes.assign(
        pageBytes.begin() + static_cast<std::ptrdiff_t>(cave.offset),
        pageBytes.begin() + static_cast<std::ptrdiff_t>(cave.offset + payload.size()));
    return plan;
}

DmaWritePlan PlanBytesAtOffset(
    const std::vector<std::uint8_t>& pageBytes,
    const std::size_t offsetInPage,
    const std::vector<std::uint8_t>& bytes) {
    DmaWritePlan plan;

    if (pageBytes.empty()) {
        plan.status = DmaOpPlanStatus::PageBytesUnavailable;
        return plan;
    }
    if (pageBytes.size() != static_cast<std::size_t>(kDmaOpPageBytes)) {
        plan.status = DmaOpPlanStatus::PageBytesWrongSize;
        return plan;
    }
    if (bytes.empty()) {
        plan.status = DmaOpPlanStatus::EmptyPayload;
        return plan;
    }
    if (offsetInPage >= static_cast<std::size_t>(kDmaOpPageBytes)) {
        plan.status = DmaOpPlanStatus::OffsetOutOfPage;
        return plan;
    }
    // 越界判据写成"剩余空间不够"而不是"起点加长度大于页长"：后者在长度极大时
    // 会整型回绕，回绕之后的比较恰好会通过。
    if (bytes.size() > static_cast<std::size_t>(kDmaOpPageBytes) - offsetInPage) {
        plan.status = DmaOpPlanStatus::WouldCrossPage;
        return plan;
    }

    plan.status = DmaOpPlanStatus::Ok;
    plan.offsetInPage = offsetInPage;
    plan.bytesToWrite = bytes;
    plan.originalBytes.assign(
        pageBytes.begin() + static_cast<std::ptrdiff_t>(offsetInPage),
        pageBytes.begin() + static_cast<std::ptrdiff_t>(offsetInPage + bytes.size()));
    return plan;
}

const char* DmaTargetSharingName(const DmaTargetSharing sharing) noexcept {
    switch (sharing) {
        case DmaTargetSharing::PrivateConfirmed: return "已确认为进程私有";
        case DmaTargetSharing::SharedConfirmed: return "已确认被其它进程共享";
        case DmaTargetSharing::SharingUnknown: return "无法确认是否共享";
    }
    return "未知";
}

DmaTargetSharing EvaluateTargetSharing(
    const bool regionIsPrivate,
    const bool comparisonPerformed,
    const bool comparisonMatched) noexcept {
    // MEM_PRIVATE 的页不由节对象支撑，谈不上跨进程共享，不需要再比对。
    if (regionIsPrivate) {
        return DmaTargetSharing::PrivateConfirmed;
    }
    // 没比对过就**不能**说私有。此刻没有别的进程映射它，也不代表下一刻没有；
    // 而把"没找到"读成"确认私有"正是这条判据存在要防的那个错误。
    if (!comparisonPerformed) {
        return DmaTargetSharing::SharingUnknown;
    }
    return comparisonMatched
        ? DmaTargetSharing::SharedConfirmed
        : DmaTargetSharing::PrivateConfirmed;
}

std::vector<std::uint8_t> UndefinedInstructionBytes() {
    return std::vector<std::uint8_t>{0x0FU, 0x0BU};
}

DmaWriteVerification VerifyWriteReadback(
    const std::vector<std::uint8_t>& intendedBytes,
    const std::vector<std::uint8_t>& readbackBytes) {
    DmaWriteVerification verification;

    // 两边都为空不算"验证通过"：没有写入就没有可验证的东西，报 matched = true
    // 会让一次什么都没做的操作看起来成功了。
    if (intendedBytes.empty()) {
        verification.matched = false;
        return verification;
    }
    if (readbackBytes.size() < intendedBytes.size()) {
        verification.readbackTooShort = true;
        verification.comparedBytes = readbackBytes.size();
        // 公共前缀仍然要比：前缀就不对的话，说明不只是读少了。
        for (std::size_t index = 0U; index < readbackBytes.size(); ++index) {
            if (readbackBytes[index] != intendedBytes[index]) {
                verification.firstMismatchOffset = index;
                verification.expectedByte = intendedBytes[index];
                verification.actualByte = readbackBytes[index];
                return verification;
            }
        }
        verification.firstMismatchOffset = readbackBytes.size();
        return verification;
    }

    verification.comparedBytes = intendedBytes.size();
    for (std::size_t index = 0U; index < intendedBytes.size(); ++index) {
        if (readbackBytes[index] != intendedBytes[index]) {
            verification.firstMismatchOffset = index;
            verification.expectedByte = intendedBytes[index];
            verification.actualByte = readbackBytes[index];
            return verification;
        }
    }
    verification.matched = true;
    return verification;
}

}  // namespace Ksword::Evidence
