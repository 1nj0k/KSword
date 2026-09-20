#include "DdmaScratchPlan.h"

#include <algorithm>

// ============================================================
// DdmaScratchPlan.cpp
// 作用：从分区表求出未分配间隙，并按"谁会住在那里"给候选分级。
// ============================================================

namespace ksword::evidence
{
    namespace
    {
        // mergeOccupied：把已占用区间排序并合并重叠/相邻部分。
        // 不合并就会在重叠分区（或重复上报）时算出一堆长度为负的假间隙。
        std::vector<DdmaScratchOccupiedRange> mergeOccupied(
            const std::vector<DdmaScratchOccupiedRange>& occupied,
            const std::uint64_t diskSectorCount)
        {
            std::vector<DdmaScratchOccupiedRange> ranges;
            ranges.reserve(occupied.size());
            for (const DdmaScratchOccupiedRange& range : occupied)
            {
                if (range.sectorCount == 0ULL || range.startSector >= diskSectorCount)
                {
                    continue;
                }
                DdmaScratchOccupiedRange clamped = range;
                // 区间末端超出磁盘时按磁盘末端截断；溢出的加法要先判再算。
                const std::uint64_t available = diskSectorCount - clamped.startSector;
                if (clamped.sectorCount > available)
                {
                    clamped.sectorCount = available;
                }
                ranges.push_back(clamped);
            }

            std::sort(
                ranges.begin(),
                ranges.end(),
                [](const DdmaScratchOccupiedRange& left, const DdmaScratchOccupiedRange& right) {
                    if (left.startSector != right.startSector)
                    {
                        return left.startSector < right.startSector;
                    }
                    return left.sectorCount < right.sectorCount;
                });

            std::vector<DdmaScratchOccupiedRange> merged;
            for (const DdmaScratchOccupiedRange& range : ranges)
            {
                if (merged.empty())
                {
                    merged.push_back(range);
                    continue;
                }
                DdmaScratchOccupiedRange& last = merged.back();
                const std::uint64_t lastEnd = last.startSector + last.sectorCount;
                if (range.startSector <= lastEnd)
                {
                    const std::uint64_t rangeEnd = range.startSector + range.sectorCount;
                    if (rangeEnd > lastEnd)
                    {
                        last.sectorCount = rangeEnd - last.startSector;
                    }
                    continue;
                }
                merged.push_back(range);
            }
            return merged;
        }

        // alignUp：把起点向上对齐到传输粒度，让一次 DMA 落在对齐边界上。
        std::uint64_t alignUp(const std::uint64_t value, const std::uint64_t alignment)
        {
            if (alignment <= 1ULL)
            {
                return value;
            }
            const std::uint64_t remainder = value % alignment;
            if (remainder == 0ULL)
            {
                return value;
            }
            // 溢出保护：对齐后越过 64 位上限时原样返回，交给后续可用性判定拒掉。
            const std::uint64_t delta = alignment - remainder;
            if (value > (UINT64_MAX - delta))
            {
                return value;
            }
            return value + delta;
        }

        // classifyGap：按间隙与头尾保留区的关系定级。
        DdmaScratchRisk classifyGap(
            const std::uint64_t gapStart,
            const std::uint64_t gapEnd,          // 半开区间末端
            const std::uint64_t headReservedEnd,
            const std::uint64_t tailReservedStart,
            const bool isTailGap)
        {
            if (gapStart < headReservedEnd)
            {
                // 与头部保留区相交：引导器就嵌在这一段。
                return DdmaScratchRisk::HeadReserved;
            }
            if (gapEnd > tailReservedStart)
            {
                // 与备份分区表相交：覆盖它会让分区表失去冗余。
                return DdmaScratchRisk::TailReserved;
            }
            return isTailGap ? DdmaScratchRisk::TailGap : DdmaScratchRisk::InteriorGap;
        }
    }

    bool ddmaScratchRiskIsSelectable(const DdmaScratchRisk risk)
    {
        return risk == DdmaScratchRisk::InteriorGap || risk == DdmaScratchRisk::TailGap;
    }

    std::vector<DdmaScratchCandidate> planDdmaScratchCandidates(
        const std::uint64_t diskSectorCount,
        const std::vector<DdmaScratchOccupiedRange>& occupied,
        const std::uint32_t requiredSectors)
    {
        std::vector<DdmaScratchCandidate> candidates;
        if (diskSectorCount == 0ULL || requiredSectors == 0UL)
        {
            return candidates;
        }

        const std::uint64_t required = static_cast<std::uint64_t>(requiredSectors);
        const std::uint64_t headReservedEnd =
            std::min<std::uint64_t>(kDdmaScratchHeadReservedSectors, diskSectorCount);
        const std::uint64_t tailReservedStart =
            (diskSectorCount > kDdmaScratchTailReservedSectors)
                ? (diskSectorCount - kDdmaScratchTailReservedSectors)
                : 0ULL;

        const std::vector<DdmaScratchOccupiedRange> merged =
            mergeOccupied(occupied, diskSectorCount);

        // 求补集：逐个已占用区间之间的空档，外加最后一个之后到磁盘末端。
        std::uint64_t cursor = 0ULL;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> gaps; // (start, end) 半开
        for (const DdmaScratchOccupiedRange& range : merged)
        {
            if (range.startSector > cursor)
            {
                gaps.emplace_back(cursor, range.startSector);
            }
            const std::uint64_t end = range.startSector + range.sectorCount;
            if (end > cursor)
            {
                cursor = end;
            }
        }
        const bool hasTailGap = (cursor < diskSectorCount);
        if (hasTailGap)
        {
            gaps.emplace_back(cursor, diskSectorCount);
        }

        for (std::size_t index = 0U; index < gaps.size(); ++index)
        {
            const std::uint64_t gapStart = gaps[index].first;
            const std::uint64_t gapEnd = gaps[index].second;
            const bool isTailGap = hasTailGap && (index + 1U == gaps.size());

            DdmaScratchCandidate candidate;
            candidate.gapStartSector = gapStart;
            candidate.gapSectorCount = gapEnd - gapStart;
            candidate.risk =
                classifyGap(gapStart, gapEnd, headReservedEnd, tailReservedStart, isTailGap);

            // 起点向上对齐到传输粒度；对齐会吃掉间隙开头的几个扇区，所以可用性
            // 必须在对齐之后再判一次，不能拿原始间隙长度去比。
            const std::uint64_t alignedStart = alignUp(gapStart, required);
            candidate.startSector = alignedStart;
            candidate.sectorCount =
                (alignedStart < gapEnd) ? (gapEnd - alignedStart) : 0ULL;

            // 可用的条件：对齐后放得下一次完整传输，且整段不与尾部保留区相交。
            // 头部相交的候选仍然标为可用——它确实能用，只是危险；把它判成不可用
            // 会让"只有这一个候选"的机器上界面说不出任何原因。
            const bool fits = (candidate.sectorCount >= required);
            const bool clearsTail =
                (alignedStart + required) <= tailReservedStart || tailReservedStart == 0ULL;
            candidate.usable =
                fits && clearsTail && candidate.risk != DdmaScratchRisk::TailReserved;

            candidates.push_back(candidate);
        }

        std::sort(
            candidates.begin(),
            candidates.end(),
            [](const DdmaScratchCandidate& left, const DdmaScratchCandidate& right) {
                // 可用的排在不可用之前；其次风险由低到高；再次间隙由大到小；
                // 最后按 LBA 由小到大，保证同一块盘每次给出同一个首选。
                if (left.usable != right.usable)
                {
                    return left.usable;
                }
                if (left.risk != right.risk)
                {
                    return static_cast<int>(left.risk) < static_cast<int>(right.risk);
                }
                if (left.sectorCount != right.sectorCount)
                {
                    return left.sectorCount > right.sectorCount;
                }
                return left.startSector < right.startSector;
            });

        return candidates;
    }
}
