#pragma once

// ============================================================
// DdmaScratchPlan.h
// 作用：
// - 从磁盘分区表算出可以拿来做 DDMA 暂存扇区的候选区间，并给出风险分级。
//
// 为什么要有这个模块：
// - DDMA 必须借一块磁盘扇区当中转站，而"哪块扇区可以被临时覆盖"是一道**算错
//   了就毁数据**的题。手填 LBA 把这道题丢给用户，等于要求每个人自己去读分区表；
//   自动挑一个又会把责任藏起来。这里的定位是**算出候选并附上证据**，最终仍由
//   用户确认——判据看得见，责任不转移。
//
// 关键事实（决定了分级方式）：
// - 分区表报告为"未分配"不等于"那里什么都没有"。磁盘**头部**的间隙正是引导器
//   寄居的地方：MBR 盘上 GRUB 把 core image 直接嵌在 LBA 1..2047，没有任何分区
//   表项；GPT 盘上同一段是对齐间隙，同样可能被工具占用。所以头部间隙必须单独
//   分级，且永远不作为首选。
// - 磁盘**尾部**最后若干扇区是 GPT 备份头与备份分区表，覆盖它们会让分区表失去
//   冗余，必须整段排除。
// - 分区之间与最后一个分区之后的间隙没有结构性住户，是相对安全的候选。
//
// 本文件是纯算术：不碰 Qt、不碰 Win32、不做任何 I/O。真正读分区表与读扇区内容
// 由调用方完成，内容是否全零之类的证据以参数形式传进来。
// ============================================================

#include <cstdint>
#include <vector>

namespace ksword::evidence
{
    // DdmaScratchOccupiedRange：一段已被占用的扇区区间（通常是一个分区）。
    struct DdmaScratchOccupiedRange
    {
        std::uint64_t startSector = 0;
        std::uint64_t sectorCount = 0;
    };

    // DdmaScratchRisk：候选区间的风险分级。
    enum class DdmaScratchRisk : int
    {
        // InteriorGap：两个分区之间的未分配间隙。没有结构性住户，最优先。
        InteriorGap = 0,
        // TailGap：最后一个分区之后、备份分区表之前的未分配空间。同样没有
        // 结构性住户；排在 InteriorGap 之后只是因为某些工具习惯往盘尾塞东西。
        TailGap,
        // HeadReserved：与磁盘头部保留区相交。引导器就嵌在这一段，
        // **永远不会被选为首选**，只有在没有别的候选时才列出并显著告警。
        HeadReserved,
        // TailReserved：与备份分区表相交，直接不可用。
        TailReserved
    };

    // DdmaScratchCandidate：一个候选暂存区间。
    struct DdmaScratchCandidate
    {
        std::uint64_t startSector = 0;  // 建议使用的起始 LBA，已按传输粒度对齐。
        std::uint64_t sectorCount = 0;  // 该间隙可用扇区数（不是本次要用的数量）。
        std::uint64_t gapStartSector = 0; // 所属间隙的原始起点，供界面展示证据。
        std::uint64_t gapSectorCount = 0; // 所属间隙的原始长度。
        DdmaScratchRisk risk = DdmaScratchRisk::HeadReserved;
        bool usable = false;            // 是否放得下一次传输且不落在不可用区。
    };

    // kDdmaScratchHeadReservedSectors：
    // - 磁盘头部保留区长度。取 2048 扇区（1 MiB）：Windows 正是把第一个分区对齐
    //   到 2048 来留出这一段，而 MBR 盘上 GRUB 的嵌入区就在里面。
    inline constexpr std::uint64_t kDdmaScratchHeadReservedSectors = 2048ULL;

    // kDdmaScratchTailReservedSectors：
    // - 磁盘尾部保留区长度。GPT 备份头占最后 1 扇区、备份分区表占其前 32 扇区，
    //   共 33；这里取 64 留出余量，避免贴着边界。
    inline constexpr std::uint64_t kDdmaScratchTailReservedSectors = 64ULL;

    // planDdmaScratchCandidates：
    // - 输入：磁盘总扇区数、已占用区间（顺序任意、允许重叠）、一次传输所需扇区数；
    // - 处理：合并已占用区间、求补集得到间隙、按传输粒度对齐起点、分级并排序；
    // - 返回：按"风险由低到高、同级按间隙由大到小、再按 LBA 由小到大"排序的候选；
    //   放不下一次传输的间隙会以 usable=false 保留，让界面能解释"为什么它没被选"，
    //   而不是无声消失。
    std::vector<DdmaScratchCandidate> planDdmaScratchCandidates(
        std::uint64_t diskSectorCount,
        const std::vector<DdmaScratchOccupiedRange>& occupied,
        std::uint32_t requiredSectors);

    // ddmaScratchRiskIsSelectable：
    // - 输入：风险分级；
    // - 返回：该分级是否允许被自动选为首选。HeadReserved 与 TailReserved 一律为假：
    //   前者是引导器所在，后者会破坏分区表冗余，两者都只能由用户显式挑选。
    bool ddmaScratchRiskIsSelectable(DdmaScratchRisk risk);
}
