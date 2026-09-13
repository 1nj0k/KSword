#pragma once

#include "KswordArkSafetyIoctl.h"

// ============================================================
// KswordArkInjectionScanIoctl.h
// 作用：
// - 定义注入痕迹检查（issue #196）的 R0 扫描后端协议；
// - 两个只读能力：进程 VAD 枚举、进程用户态可执行 PTE 范围扫描；
// - 两者都是**独立视图**，用来和 R3 的 VirtualQueryEx/QueryWorkingSetEx 交叉核对，
//   不是同一数据换个路径拿。
//
// 为什么需要这两条，而不是复用已有 IOCTL：
//   * IOCTL_KSWORD_ARK_QUERY_VIRTUAL_MEMORY 走的是 ZwQueryVirtualMemory，
//     与 R3 的 VirtualQueryEx 是**同一个来源**，交叉核对它等于自己和自己比。
//     VAD 枚举直接读 EPROCESS.VadRoot 的平衡树，才是第二个来源。
//   * IOCTL_KSWORD_ARK_QUERY_PAGE_TABLE_ENTRY 一次只解析一个 VA。要在整个用户地址
//     空间里找"页表说可执行"的页（PteMalfind 的做法），逐 VA 调用是不可行的。
//
// 硬边界（写在协议里，免得实现或调用方后来忘了）：
//   * 只读。本文件不提供任何写 VAD、写 PTE、改保护的入口。
//   * 内部结构必须按目标 build 验证：VadRoot 偏移来自 DynData，未验证的版本一律
//     返回 DYNDATA_MISSING 并把 profileVerified 置 0，**不允许**用相近版本的偏移继续读。
//   * 扫描期间目标地址空间会变。协议不提供"一致性快照"，只提供游标续扫和
//     不一致计数；调用方必须把它当作跨时点观测，不能当原子快照。
//   * 内核采集依赖内核可信。有内核能力的对手可以改这里读到的元数据；
//     本协议不承诺"有驱动便无法隐藏"。
// ============================================================

#define KSWORD_ARK_INJECTION_SCAN_PROTOCOL_VERSION 1UL

#define KSWORD_ARK_IOCTL_FUNCTION_ENUMERATE_PROCESS_VAD 0x912UL
#define KSWORD_ARK_IOCTL_FUNCTION_SCAN_PROCESS_EXECUTABLE_PTE 0x913UL

// FILE_WRITE_ACCESS 与内存类 IOCTL 一致：这两条暴露内核内部结构，
// 不能走 FILE_ANY_ACCESS 那一档。
#define IOCTL_KSWORD_ARK_ENUMERATE_PROCESS_VAD \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_ENUMERATE_PROCESS_VAD, \
        METHOD_BUFFERED, \
        FILE_WRITE_ACCESS)

#define IOCTL_KSWORD_ARK_SCAN_PROCESS_EXECUTABLE_PTE \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_SCAN_PROCESS_EXECUTABLE_PTE, \
        METHOD_BUFFERED, \
        FILE_WRITE_ACCESS)

// ---------------------------------------------------------------------------
// 通用状态
// ---------------------------------------------------------------------------
#define KSWORD_ARK_INJECTION_SCAN_STATUS_UNAVAILABLE           0UL
#define KSWORD_ARK_INJECTION_SCAN_STATUS_OK                    1UL
// PARTIAL：走完了请求范围，但中途有读不到的节点/表项。
#define KSWORD_ARK_INJECTION_SCAN_STATUS_PARTIAL               2UL
// TRUNCATED：命中条目或预算上限，**没有**走完请求范围。与 PARTIAL 分开是必须的：
// 前者是"范围走完了但有洞"，后者是"范围根本没走完"，混用会让调用方以为已覆盖全程。
#define KSWORD_ARK_INJECTION_SCAN_STATUS_TRUNCATED             3UL
#define KSWORD_ARK_INJECTION_SCAN_STATUS_DYNDATA_MISSING       4UL
#define KSWORD_ARK_INJECTION_SCAN_STATUS_PROCESS_LOOKUP_FAILED 5UL
#define KSWORD_ARK_INJECTION_SCAN_STATUS_PROCESS_EXITING       6UL
#define KSWORD_ARK_INJECTION_SCAN_STATUS_WALK_FAILED           7UL
#define KSWORD_ARK_INJECTION_SCAN_STATUS_BUFFER_TOO_SMALL      8UL
#define KSWORD_ARK_INJECTION_SCAN_STATUS_IRQL_REJECTED         9UL
#define KSWORD_ARK_INJECTION_SCAN_STATUS_INVALID_RANGE         10UL

// 响应字段可用性。
#define KSWORD_ARK_INJECTION_FIELD_ENTRIES_PRESENT     0x00000001UL
#define KSWORD_ARK_INJECTION_FIELD_CURSOR_PRESENT      0x00000002UL
#define KSWORD_ARK_INJECTION_FIELD_ROOT_PRESENT        0x00000004UL
#define KSWORD_ARK_INJECTION_FIELD_BUDGET_EXHAUSTED    0x00000008UL
#define KSWORD_ARK_INJECTION_FIELD_INCONSISTENT_WALK   0x00000010UL
#define KSWORD_ARK_INJECTION_FIELD_CR3_PRESENT         0x00000020UL
// 本次遍历完整走完了整棵树（没有范围过滤、没有游标、没有截断），因此
// visitedCount / parentMismatchNodes / vadHintVisited 可以拿来和 vadCount 比。
// **没有这一位时那几个字段一律不得参与判定** —— 部分遍历下 visitedCount 本来就小。
#define KSWORD_ARK_INJECTION_FIELD_INTEGRITY_VALID     0x00000040UL
// EPROCESS.VadCount 的偏移可用且已读到。
#define KSWORD_ARK_INJECTION_FIELD_VAD_COUNT_PRESENT   0x00000080UL
// EPROCESS.VadHint 的偏移可用且已读到。
#define KSWORD_ARK_INJECTION_FIELD_VAD_HINT_PRESENT    0x00000100UL

// ---------------------------------------------------------------------------
// VAD 枚举
// ---------------------------------------------------------------------------
#define KSWORD_ARK_INJECTION_VAD_LIMIT_DEFAULT 2048UL
#define KSWORD_ARK_INJECTION_VAD_LIMIT_MAX     16384UL
// 平衡树最大深度。真实 VAD 树深度远小于此；设死上限是为了让中序遍历的显式栈
// 有固定大小，且畸形/被改写的树不会让遍历跑飞。
#define KSWORD_ARK_INJECTION_VAD_MAX_DEPTH     64UL

// entryFlags
#define KSWORD_ARK_INJECTION_VAD_FLAG_PRIVATE_MEMORY 0x00000001UL
#define KSWORD_ARK_INJECTION_VAD_FLAG_HAS_SUBSECTION 0x00000002UL
#define KSWORD_ARK_INJECTION_VAD_FLAG_LONG_VAD       0x00000004UL
#define KSWORD_ARK_INJECTION_VAD_FLAG_NODE_UNREADABLE 0x00000008UL
#define KSWORD_ARK_INJECTION_VAD_FLAG_RANGE_MALFORMED 0x00000010UL
// protection / vadType / PRIVATE_MEMORY 是按**假定的**位域布局从 LongFlags 解出来的。
// DynData 只验证了 EPROCESS.VadRoot 的偏移，没有验证 MMVAD_FLAGS 的位位置，
// 所以这三项只能当展示用，**不得**用来和 R3 的保护属性做"矛盾"判定 ——
// 位布局猜错时那会变成整片假矛盾。原始 vadFlagsRaw 始终一并返回，便于事后复核。
#define KSWORD_ARK_INJECTION_VAD_FLAG_FLAGS_LAYOUT_ASSUMED 0x00000020UL

// MMVAD_FLAGS.VadType（winnt 未公开，取值随版本稳定）。解析不出来时用 UNKNOWN，
// 不猜。
#define KSWORD_ARK_INJECTION_VAD_TYPE_UNKNOWN 0xFFFFFFFFUL

typedef struct _KSWORD_ARK_ENUMERATE_PROCESS_VAD_REQUEST
{
    unsigned long flags;
    unsigned long processId;
    unsigned long maxEntries;
    unsigned long reserved0;
    unsigned long long startAddress;   // 0 表示从用户空间起点
    unsigned long long endAddress;     // 0 表示到用户空间终点
    // 续扫游标：上一次响应的 nextCursorVpn。0 表示从头开始。
    unsigned long long cursorVpn;
    unsigned long long reserved1;
} KSWORD_ARK_ENUMERATE_PROCESS_VAD_REQUEST;

// 一条 VAD 记录。**刻意不含文件名**：从 VAD 反查文件名要经
// Subsection -> ControlArea -> FilePointer 三次不可信解引用，风险远大于收益 ——
// 路径这一维 R3 的 GetMappedFileNameW 已经给了，交叉核对需要的是范围与保护属性。
// subsection 非 0 只说明"这是一段有 section 支撑的映射"，够用于分类。
// **它是 Subsection 指针，不是 ControlArea。** 两者差一次解引用（_SUBSECTION.ControlArea
// 在偏移 0），实测地址差 0x80 = sizeof(_CONTROL_AREA)。真正的 ControlArea 由
// IOCTL_KSWORD_ARK_READ_IMAGE_SECTION_PAGES 给出。早先这里叫 controlArea，
// 任何拿它去和别处的 ControlArea 地址对账的人都会被误导。
typedef struct _KSWORD_ARK_PROCESS_VAD_ENTRY
{
    unsigned long long startVa;
    unsigned long long endVaExclusive;
    unsigned long long vadNodeAddress;
    unsigned long long subsection;       // 0 = 私有内存或读不到
    unsigned long long firstPrototypePte; // 仅诊断
    unsigned long vadFlagsRaw;           // MMVAD_SHORT.LongFlags 原值
    unsigned long protection;            // MMVAD_FLAGS.Protection（MM_ 编码）
    unsigned long vadType;               // MMVAD_FLAGS.VadType
    unsigned long entryFlags;
} KSWORD_ARK_PROCESS_VAD_ENTRY;

typedef struct _KSWORD_ARK_ENUMERATE_PROCESS_VAD_RESPONSE
{
    unsigned long version;
    unsigned long size;
    unsigned long processId;
    unsigned long fieldFlags;
    unsigned long status;
    long lastStatus;
    unsigned long entrySize;
    unsigned long returnedCount;
    // 本次遍历真正访问到的节点数（含被范围过滤掉的）。它与 returnedCount 分开，
    // 否则"过滤掉很多"和"只走了很少"在账目上长得一样。
    unsigned long visitedCount;
    unsigned long unreadableNodeCount;
    // DynData 里的 EPROCESS.VadRoot 偏移是否已针对当前 build 验证。为 0 时
    // 结果不可用于"缺项推断"，调用方必须降级。
    unsigned long profileVerified;
    unsigned long vadRootOffset;
    unsigned long long vadRootAddress;
    unsigned long long nextCursorVpn;    // 0 = 已走完

    // --- 树结构完整性（断链检查）---------------------------------------------
    // 这几项只在**完整走完整棵树**时才有意义：带范围过滤、带游标、或者命中条目
    // 上限时，visitedCount 本来就不该等于 VadCount。所以它们由
    // KSWORD_ARK_INJECTION_FIELD_INTEGRITY_VALID 单独把门，调用方必须先看那一位。
    unsigned long long vadHintAddress;   // EPROCESS.VadHint 的值；0 = 偏移不可用
    // EPROCESS.VadCount。内核自己维护的计数，摘链的人通常不会同步减它，
    // 所以 visitedCount < vadCount 是"有节点不在树上"的直接读数。
    unsigned long vadCount;
    // 父指针回指不一致的节点数：ParentValue & ~3 指向的节点，其左右孩子里
    // 没有一个是本节点。干净的摘链（把父的孩子指针接到自己的子树上）不会留下
    // 这个痕迹，但粗暴改写会。
    unsigned long parentMismatchNodes;
    // EPROCESS.VadHint 指向的节点在本次遍历中被访问到了。VadHint 是内核的
    // "最近用过的 VAD"缓存，它指向一个树上找不到的节点，说明树被动过。
    unsigned long vadHintVisited;
    unsigned long integrityReserved;

    KSWORD_ARK_PROCESS_VAD_ENTRY entries[1];
} KSWORD_ARK_ENUMERATE_PROCESS_VAD_RESPONSE;

#define KSWORD_ARK_INJECTION_VAD_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_ENUMERATE_PROCESS_VAD_RESPONSE) - sizeof(KSWORD_ARK_PROCESS_VAD_ENTRY))

// ---------------------------------------------------------------------------
// 用户态可执行 PTE 扫描
// ---------------------------------------------------------------------------
// 默认条目上限按实测定，不是拍的：2026-09-12 在 Win11 22621.4317 上，
// explorer.exe 的整个用户地址空间产出 7266 段（443 次表读、22779 个可执行 4 KiB 页），
// lsass 产出 1044 段。4096 曾是默认值，explorer 上直接截断在半路。16384 留了一倍余量。
#define KSWORD_ARK_INJECTION_PTE_LIMIT_DEFAULT 16384UL
#define KSWORD_ARK_INJECTION_PTE_LIMIT_MAX     32768UL
// 读表预算：一次请求最多读多少张页表页。4 级页表每张 4 KiB。
// 默认值按"覆盖一个普通进程的全部已映射区域"量级给，超过即 TRUNCATED + 游标续扫。
#define KSWORD_ARK_INJECTION_PTE_TABLE_READS_DEFAULT 8192UL
#define KSWORD_ARK_INJECTION_PTE_TABLE_READS_MAX     262144UL

// flags
// 默认只报"页表说可执行"的叶子。置位后连不可执行的已映射叶子也报 —— 量极大，
// 只用于诊断，不要在常规扫描里打开。
#define KSWORD_ARK_INJECTION_PTE_FLAG_INCLUDE_NON_EXECUTABLE 0x00000001UL
// 默认跳过 supervisor 页（用户地址空间里不该有）。置位后一并报出来。
#define KSWORD_ARK_INJECTION_PTE_FLAG_INCLUDE_SUPERVISOR 0x00000002UL

// entryFlags
#define KSWORD_ARK_INJECTION_PTE_ENTRY_FLAG_EXECUTABLE 0x00000001UL
#define KSWORD_ARK_INJECTION_PTE_ENTRY_FLAG_WRITABLE   0x00000002UL
#define KSWORD_ARK_INJECTION_PTE_ENTRY_FLAG_USER       0x00000004UL
#define KSWORD_ARK_INJECTION_PTE_ENTRY_FLAG_LARGE_PAGE 0x00000008UL

typedef struct _KSWORD_ARK_SCAN_PROCESS_EXECUTABLE_PTE_REQUEST
{
    unsigned long flags;
    unsigned long processId;
    unsigned long maxEntries;
    unsigned long maxTableReads;
    unsigned long long startAddress;
    unsigned long long endAddress;      // 0 表示到用户空间终点
    unsigned long long cursorAddress;   // 续扫游标；0 表示从 startAddress 开始
    unsigned long long reserved0;
} KSWORD_ARK_SCAN_PROCESS_EXECUTABLE_PTE_REQUEST;

// 一段属性相同、地址连续的叶子页。按"段"而不是"页"返回：一个 2 MiB 的可执行
// 私有分配是 512 个 4 KiB 叶子，逐页返回会把缓冲撑爆且没有额外信息。
typedef struct _KSWORD_ARK_PROCESS_EXECUTABLE_PTE_ENTRY
{
    unsigned long long startVa;
    unsigned long long byteLength;
    unsigned long long firstPhysicalAddress;
    unsigned long long firstEntryValue;   // 该段第一个叶子项的原始值
    unsigned long pageSize;
    unsigned long pageCount;
    unsigned long effectiveFlags;         // KSWORD_ARK_PAGE_TABLE_FLAG_*（逐级合并后）
    unsigned long entryFlags;             // KSWORD_ARK_INJECTION_PTE_ENTRY_FLAG_*
} KSWORD_ARK_PROCESS_EXECUTABLE_PTE_ENTRY;

typedef struct _KSWORD_ARK_SCAN_PROCESS_EXECUTABLE_PTE_RESPONSE
{
    unsigned long version;
    unsigned long size;
    unsigned long processId;
    unsigned long fieldFlags;
    unsigned long status;
    long lastStatus;
    unsigned long entrySize;
    unsigned long returnedCount;
    unsigned long tableReads;          // 实际读了多少张表页
    unsigned long failedTableReads;    // 读失败的表页数；>0 即 PARTIAL
    unsigned long executablePageCount; // 命中的可执行叶子页总数（按 4 KiB 计）
    unsigned long reserved0;
    unsigned long long scannedBegin;   // 实际扫描到的范围
    unsigned long long scannedEnd;
    unsigned long long nextCursorAddress;  // 0 = 已走完
    unsigned long long cr3PhysicalAddress;
    KSWORD_ARK_PROCESS_EXECUTABLE_PTE_ENTRY entries[1];
} KSWORD_ARK_SCAN_PROCESS_EXECUTABLE_PTE_RESPONSE;

#define KSWORD_ARK_INJECTION_PTE_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_SCAN_PROCESS_EXECUTABLE_PTE_RESPONSE) - \
     sizeof(KSWORD_ARK_PROCESS_EXECUTABLE_PTE_ENTRY))

// ---------------------------------------------------------------------------
// 映像节对象参考页（issue #196 §五 第三层）
// ---------------------------------------------------------------------------
// 拿"这个映像本来该是什么样"的**第二个来源**：不是磁盘上的文件，而是内存管理器
// 自己持有的节对象。链路是 VAD → Subsection → ControlArea → Segment → PrototypePte[]。
//
// 它比"和磁盘文件比"多买到两件事：
//   * 磁盘文件被锁住 / 读不到 / 已被替换时，仍然有参考；
//   * 攻击者把磁盘文件也改成和内存一致时，仍然能发现 —— 节对象里的那份是
//     映射建立时的内容，改磁盘文件不会改它。
//
// **刻意不解码软件 PTE。** 原型 PTE 有多种形态：valid（硬件格式）、transition、
// 在页面文件里、demand-zero。只有 valid 那一种的位布局是**架构定义**的
// （bit 0 = Present，bits 12..51 = PFN）；transition/pagefile 的编码是 Windows
// 自定义且随版本变，正是 kLimitKernelVadFlagsUnverified 划下的那条线。
// 所以本接口只对 valid 的原型 PTE 取 PFN 并读物理页，其余一律报成"这一页拿不到
// 参考"，由上层记成覆盖缺口。**绝不把页面调进来** —— 那会改变目标状态，
// 而且在这个 IRQL 上会死锁。
#define KSWORD_ARK_IOCTL_FUNCTION_READ_IMAGE_SECTION_PAGES 0x914UL

#define IOCTL_KSWORD_ARK_READ_IMAGE_SECTION_PAGES \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_READ_IMAGE_SECTION_PAGES, \
        METHOD_BUFFERED, \
        FILE_WRITE_ACCESS)

// 一次请求最多解析多少页。带字节时每页 4 KiB，64 页 = 256 KiB，
// 已经超过 METHOD_BUFFERED 的舒适区，所以默认只给元数据。
#define KSWORD_ARK_INJECTION_SECTION_PAGES_DEFAULT 512UL
#define KSWORD_ARK_INJECTION_SECTION_PAGES_MAX     4096UL
// 带页字节时的上限，单独一档：METHOD_BUFFERED 的缓冲要一次分配出来。
#define KSWORD_ARK_INJECTION_SECTION_BYTES_PAGES_MAX 64UL

// flags
// 置位后每个条目后面跟 4096 字节的干净页内容（只对 valid 的原型 PTE 有效）。
// 不置位时只返回元数据，条目里没有字节。
#define KSWORD_ARK_INJECTION_SECTION_FLAG_INCLUDE_BYTES 0x00000001UL

// 条目标志
// 这一页的原型 PTE 是 valid 硬件格式，physicalAddress 可用。
#define KSWORD_ARK_INJECTION_SECTION_ENTRY_FLAG_VALID        0x00000001UL
// 原型 PTE 读到了，但不是 valid 形态（transition / 页面文件 / 零页）。
// **不是错误**，是"这一页当前不在内存里，本接口按设计不去调它进来"。
#define KSWORD_ARK_INJECTION_SECTION_ENTRY_FLAG_NOT_RESIDENT 0x00000002UL
// 原型 PTE 本身没读到（分页池被换出等）。
#define KSWORD_ARK_INJECTION_SECTION_ENTRY_FLAG_PTE_UNREADABLE 0x00000004UL
// 这一页的字节已随条目返回。
#define KSWORD_ARK_INJECTION_SECTION_ENTRY_FLAG_BYTES_PRESENT 0x00000008UL

typedef struct _KSWORD_ARK_READ_IMAGE_SECTION_PAGES_REQUEST
{
    unsigned long size;
    unsigned long version;
    unsigned long processId;
    unsigned long flags;
    unsigned long long rangeStart;   // 目标进程里的 VA，按页对齐
    unsigned long long rangeEnd;     // 不含
    unsigned long maxPages;
    unsigned long reserved0;
    unsigned long long cursorVa;     // 0 = 从头开始
} KSWORD_ARK_READ_IMAGE_SECTION_PAGES_REQUEST;

typedef struct _KSWORD_ARK_IMAGE_SECTION_PAGE_ENTRY
{
    unsigned long long va;                 // 目标进程里的虚拟地址
    unsigned long long prototypePteAddress;// 原型 PTE 自己的内核地址
    unsigned long long prototypePteValue;  // 原始值，未解码
    unsigned long long physicalAddress;    // 仅 VALID 时有效
    unsigned long entryFlags;
    unsigned long reserved0;
} KSWORD_ARK_IMAGE_SECTION_PAGE_ENTRY;

typedef struct _KSWORD_ARK_READ_IMAGE_SECTION_PAGES_RESPONSE
{
    unsigned long version;
    unsigned long size;
    unsigned long processId;
    unsigned long fieldFlags;
    unsigned long status;
    long lastStatus;
    unsigned long entrySize;
    unsigned long returnedCount;
    unsigned long validPageCount;       // 拿到参考的页数
    unsigned long notResidentPageCount; // 原型 PTE 不是 valid 形态
    unsigned long unreadablePteCount;   // 原型 PTE 本身读不到
    unsigned long bytesPerPage;         // 带字节时为 4096，否则 0
    unsigned long long controlArea;     // 本次解析到的 ControlArea
    unsigned long long segment;         // 本次解析到的 Segment
    unsigned long long prototypePteArray; // Segment.PrototypePte
    unsigned long long nextCursorVa;    // 0 = 已走完
    // 字节区相对响应起始的偏移（带 INCLUDE_BYTES 时非 0）。
    //
    // **这一项必须由响应直接给出，不能让调用方自己算。** 字节区排在
    // 条目**容量**之后，而容量同时受缓冲大小和请求里 maxPages 两个值约束；
    // 调用方只知道前者，重算出来的偏移会偏到别的页上去。
    unsigned long long byteAreaOffset;
    KSWORD_ARK_IMAGE_SECTION_PAGE_ENTRY entries[1];
    // 字节区：validPageCount × bytesPerPage，顺序与条目里 BYTES_PRESENT
    // 置位的顺序一致。
} KSWORD_ARK_READ_IMAGE_SECTION_PAGES_RESPONSE;

#define KSWORD_ARK_INJECTION_SECTION_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_READ_IMAGE_SECTION_PAGES_RESPONSE) - \
     sizeof(KSWORD_ARK_IMAGE_SECTION_PAGE_ENTRY))
