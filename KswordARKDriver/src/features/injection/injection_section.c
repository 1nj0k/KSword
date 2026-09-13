/*++

Module Name:

    injection_section.c

Abstract:

    映像节对象参考页（issue #196 §五 第三层）。

    拿"这个映像本来该是什么样"的**第二个来源**：不是磁盘上的文件，而是内存管理器
    自己持有的节对象。磁盘文件被锁住、读不到、或者已经被攻击者一并改掉时，这一份
    仍然是映射建立时的内容。

    设计上刻意把版本风险压到最小，三条：

    1. **不走 Subsection 链。** 一个 ControlArea 可以挂多个 Subsection（PE 每节一个），
       按 StartingSector/PtesInSubsection 去定位要依赖四个偏移。而 MMVAD 自己就有
       FirstPrototypePte 与 LastContiguousPte —— 这两者之间原型 PTE 是**连续**的，
       索引就是 (va - vadStart) / PAGE_SIZE。超出这段的页一律报"解析不到"，
       不去猜第二段在哪儿。这两个字段本模块早就在读了，不需要新的 DynData 偏移。

    2. **不解码软件 PTE。** 原型 PTE 有 valid / transition / 页面文件 / demand-zero
       四种形态，只有 valid 那一种的位布局是**架构定义**的（bit 0 = Present、
       bits 12..51 = PFN）。transition 与页面文件的编码是 Windows 自定义且随版本变，
       正是 kLimitKernelVadFlagsUnverified 划下的线。其余形态一律报
       NOT_RESIDENT，由上层记成覆盖缺口。

    3. **绝不把页面调进来。** 把不在内存的参考页读进来需要触发缺页，那会改变目标
       状态，而且在这个调用路径上会死锁。"这一页当前拿不到参考"是一个诚实的答案。

--*/

#include "ark/ark_driver.h"
#include "ark/ark_injection_scan.h"
#include "ark/ark_dyndata.h"
#include "../../platform/kernel_object_probe.h"

// PsLookupProcessByProcessId 声明在 ntifs.h 里，本驱动只 include ntddk.h。
// 与 injection_vad.c / memory_pagetable.c 同一做法：手工声明。
NTSYSAPI
NTSTATUS
NTAPI
PsLookupProcessByProcessId(
    _In_ HANDLE ProcessId,
    _Outptr_ PEPROCESS* Process
    );

// MMVAD 的布局与 injection_vad.c 保持一致；那边已有完整说明。
// 这里只需要 Core 的范围字段与长 VAD 的两个原型 PTE 指针。
typedef struct _KSW_SEC_MMVAD_SHORT
{
    union
    {
        struct
        {
            PVOID NextVad;
            PVOID ExtraCreateInfo;
        } NodeFields;
        RTL_BALANCED_NODE VadNode;
    } NodeUnion;
    ULONG StartingVpn;
    ULONG EndingVpn;
    UCHAR StartingVpnHigh;
    UCHAR EndingVpnHigh;
    UCHAR CommitChargeHigh;
    union
    {
        UCHAR SpareNT64VadUChar;
        struct
        {
            UCHAR EndingVpnHigher : 4;
            UCHAR CommitChargeHigher : 4;
        } HigherFields;
    } HigherUnion;
    LONG ReferenceCount;
    EX_PUSH_LOCK PushLock;
    ULONG LongFlags;
    ULONG LongFlags1;
    union
    {
        ULONG_PTR EventListULongPtr;
        UCHAR StartingVpnHigher : 4;
    } u5;
} KSW_SEC_MMVAD_SHORT, *PKSW_SEC_MMVAD_SHORT;

typedef struct _KSW_SEC_MMVAD
{
    KSW_SEC_MMVAD_SHORT Core;
    ULONG LongFlags2;
    PVOID Subsection;
    PVOID FirstPrototypePte;
    PVOID LastContiguousPte;
} KSW_SEC_MMVAD, *PKSW_SEC_MMVAD;

#define KSW_SEC_VAD_FLAGS_PRIVATE_MEMORY_BIT (1UL << 20)

// x64 硬件 PTE。只有这两项是架构定义的，别的一律不碰。
#define KSW_SEC_PTE_VALID_BIT   0x1ULL
#define KSW_SEC_PTE_PFN_SHIFT   12U
#define KSW_SEC_PTE_PFN_MASK    0xFFFFFFFFFULL   // bits 12..51

#define KSW_SEC_PAGE_BYTES 4096U
#define KSW_SEC_POOL_TAG   'cSsK'

static BOOLEAN
KswordARKSectionReadKernel(
    _In_ const VOID* Address,
    _Out_writes_bytes_(Size) VOID* Buffer,
    _In_ SIZE_T Size
    )
{
    MM_COPY_ADDRESS copyAddress;
    SIZE_T copied = 0U;
    NTSTATUS status = STATUS_SUCCESS;

    if (Address == NULL || Buffer == NULL || Size == 0U) {
        return FALSE;
    }
    if (!KswordARKKernelProbeRangeIsResident(Address, Size)) {
        return FALSE;
    }
    RtlZeroMemory(&copyAddress, sizeof(copyAddress));
    copyAddress.VirtualAddress = (PVOID)(ULONG_PTR)Address;
    __try {
        status = MmCopyMemory(Buffer, copyAddress, Size, MM_COPY_MEMORY_VIRTUAL, &copied);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
        copied = 0U;
    }
    return NT_SUCCESS(status) && copied == Size;
}

static BOOLEAN
KswordARKSectionReadPhysicalPage(
    _In_ ULONG64 PhysicalAddress,
    _Out_writes_bytes_(KSW_SEC_PAGE_BYTES) VOID* Buffer
    )
{
    MM_COPY_ADDRESS copyAddress;
    SIZE_T copied = 0U;
    NTSTATUS status = STATUS_SUCCESS;

    RtlZeroMemory(&copyAddress, sizeof(copyAddress));
    copyAddress.PhysicalAddress.QuadPart = (LONGLONG)PhysicalAddress;
    __try {
        status = MmCopyMemory(
            Buffer, copyAddress, KSW_SEC_PAGE_BYTES, MM_COPY_MEMORY_PHYSICAL, &copied);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
        copied = 0U;
    }
    return NT_SUCCESS(status) && copied == KSW_SEC_PAGE_BYTES;
}

static ULONG64
KswordARKSectionVadStartVa(
    _In_ const KSW_SEC_MMVAD_SHORT* Core
    )
{
    ULONG_PTR higher = Core->u5.StartingVpnHigher;
    ULONG_PTR high = Core->StartingVpnHigh;
    ULONG_PTR low = Core->StartingVpn;
    return (ULONG64)((low | ((high | (higher << 8)) << 32)) << PAGE_SHIFT);
}

static ULONG64
KswordARKSectionVadEndVaExclusive(
    _In_ const KSW_SEC_MMVAD_SHORT* Core
    )
{
    ULONG_PTR higher = Core->HigherUnion.HigherFields.EndingVpnHigher;
    ULONG_PTR high = Core->EndingVpnHigh;
    ULONG_PTR low = Core->EndingVpn;
    return (ULONG64)(((low + 1U) | ((high | (higher << 8)) << 32)) << PAGE_SHIFT);
}

static NTSTATUS
KswordARKSectionFindVad(
    _In_ PEPROCESS ProcessObject,
    _In_ const KSW_DYN_STATE* DynState,
    _In_ ULONG64 TargetVa,
    _Out_ KSW_SEC_MMVAD* VadOut,
    _Out_ ULONG64* VadStartOut,
    _Out_ ULONG64* VadEndOut
    )
/*++

Routine Description:

    在 VAD 树里找包含 TargetVa 的那个节点。中文说明：这是一次**按值下降**的查找，
    不是中序遍历 —— 只要能走到目标节点，不需要访问整棵树。每一步都容错读，
    读不到就当查不到，不跟野指针。

    深度上限与 injection_vad.c 同一个常量：被改写的"树"不能让这里转不出来。

--*/
{
    ULONG offset = 0U;
    PVOID node = NULL;
    ULONG depth = 0U;

    RtlZeroMemory(VadOut, sizeof(*VadOut));
    *VadStartOut = 0ULL;
    *VadEndOut = 0ULL;

    offset = DynState->Kernel.EpVadRoot;
    if (offset == KSW_DYN_OFFSET_UNAVAILABLE || offset == 0U) {
        return STATUS_NOT_SUPPORTED;
    }
    if (!KswordARKSectionReadKernel(
            (const UCHAR*)ProcessObject + offset, &node, sizeof(node))) {
        return STATUS_ACCESS_VIOLATION;
    }

    while (node != NULL && depth < KSWORD_ARK_INJECTION_VAD_MAX_DEPTH) {
        KSW_SEC_MMVAD_SHORT core;
        ULONG64 startVa = 0ULL;
        ULONG64 endVa = 0ULL;

        ++depth;
        if (!KswordARKSectionReadKernel(node, &core, sizeof(core))) {
            return STATUS_ACCESS_VIOLATION;
        }
        startVa = KswordARKSectionVadStartVa(&core);
        endVa = KswordARKSectionVadEndVaExclusive(&core);
        if (endVa <= startVa) {
            return STATUS_DATA_ERROR;
        }

        if (TargetVa < startVa) {
            node = core.NodeUnion.VadNode.Children[0];
            continue;
        }
        if (TargetVa >= endVa) {
            node = core.NodeUnion.VadNode.Children[1];
            continue;
        }

        // 命中。私有内存是 MMVAD_SHORT，后面的 Subsection/FirstPrototypePte 不存在 ——
        // 按长 VAD 整读会跨出这次分配。所以先用 Core 判，确认不是私有再读长字段。
        VadOut->Core = core;
        *VadStartOut = startVa;
        *VadEndOut = endVa;
        if ((core.LongFlags & KSW_SEC_VAD_FLAGS_PRIVATE_MEMORY_BIT) != 0UL) {
            return STATUS_NOT_FOUND;   // 私有内存没有节对象参考，这不是错误
        }
        if (!KswordARKSectionReadKernel(
                (const UCHAR*)node + FIELD_OFFSET(KSW_SEC_MMVAD, Subsection),
                &VadOut->Subsection,
                sizeof(PVOID) * 3U)) {
            return STATUS_ACCESS_VIOLATION;
        }
        return STATUS_SUCCESS;
    }
    return (depth >= KSWORD_ARK_INJECTION_VAD_MAX_DEPTH) ? STATUS_DATA_ERROR : STATUS_NOT_FOUND;
}

NTSTATUS
KswordARKDriverReadImageSectionPages(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_READ_IMAGE_SECTION_PAGES_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
{
    KSWORD_ARK_READ_IMAGE_SECTION_PAGES_RESPONSE* response = NULL;
    KSW_DYN_STATE dynState;
    KSW_SEC_MMVAD vad;
    PEPROCESS processObject = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    NTSTATUS findStatus = STATUS_SUCCESS;
    ULONG64 vadStart = 0ULL;
    ULONG64 vadEnd = 0ULL;
    ULONG64 rangeStart = 0ULL;
    ULONG64 rangeEnd = 0ULL;
    ULONG64 va = 0ULL;
    ULONG maxPages = 0UL;
    size_t entryCapacity = 0U;
    size_t headerSize = KSWORD_ARK_INJECTION_SECTION_RESPONSE_HEADER_SIZE;
    size_t perPageBytes = 0U;
    UCHAR* byteArea = NULL;
    BOOLEAN includeBytes = FALSE;
    BOOLEAN truncated = FALSE;

    if (BytesWrittenOut == NULL || OutputBuffer == NULL || Request == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < headerSize) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (KeGetCurrentIrql() > PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    response = (KSWORD_ARK_READ_IMAGE_SECTION_PAGES_RESPONSE*)OutputBuffer;
    RtlZeroMemory(response, headerSize);
    response->version = KSWORD_ARK_INJECTION_SCAN_PROTOCOL_VERSION;
    response->processId = Request->processId;
    response->entrySize = (ULONG)sizeof(KSWORD_ARK_IMAGE_SECTION_PAGE_ENTRY);
    response->status = KSWORD_ARK_INJECTION_SCAN_STATUS_UNAVAILABLE;

    includeBytes =
        (Request->flags & KSWORD_ARK_INJECTION_SECTION_FLAG_INCLUDE_BYTES) != 0UL;
    perPageBytes = includeBytes ? KSW_SEC_PAGE_BYTES : 0U;
    response->bytesPerPage = (ULONG)perPageBytes;

    maxPages = Request->maxPages != 0UL
        ? Request->maxPages
        : KSWORD_ARK_INJECTION_SECTION_PAGES_DEFAULT;
    if (includeBytes && maxPages > KSWORD_ARK_INJECTION_SECTION_BYTES_PAGES_MAX) {
        maxPages = KSWORD_ARK_INJECTION_SECTION_BYTES_PAGES_MAX;
    }
    if (maxPages > KSWORD_ARK_INJECTION_SECTION_PAGES_MAX) {
        maxPages = KSWORD_ARK_INJECTION_SECTION_PAGES_MAX;
    }

    // 每条目占 sizeof(entry) + 可选 4096 字节。缓冲能放几条就是几条。
    entryCapacity = (OutputBufferLength - headerSize) /
                    (sizeof(KSWORD_ARK_IMAGE_SECTION_PAGE_ENTRY) + perPageBytes);
    if (entryCapacity == 0U) {
        response->status = KSWORD_ARK_INJECTION_SCAN_STATUS_BUFFER_TOO_SMALL;
        *BytesWrittenOut = headerSize;
        response->size = (ULONG)headerSize;
        return STATUS_SUCCESS;
    }
    if (entryCapacity > maxPages) {
        entryCapacity = maxPages;
    }

    rangeStart = Request->cursorVa != 0ULL ? Request->cursorVa : Request->rangeStart;
    rangeEnd = Request->rangeEnd;
    rangeStart &= ~((ULONG64)PAGE_SIZE - 1ULL);
    if (rangeEnd <= rangeStart) {
        response->status = KSWORD_ARK_INJECTION_SCAN_STATUS_INVALID_RANGE;
        response->lastStatus = (LONG)STATUS_INVALID_PARAMETER;
        *BytesWrittenOut = headerSize;
        response->size = (ULONG)headerSize;
        return STATUS_SUCCESS;
    }

    RtlZeroMemory(&dynState, sizeof(dynState));
    KswordARKDynDataSnapshot(&dynState);

    status = PsLookupProcessByProcessId(ULongToHandle(Request->processId), &processObject);
    if (!NT_SUCCESS(status)) {
        response->status = KSWORD_ARK_INJECTION_SCAN_STATUS_PROCESS_LOOKUP_FAILED;
        response->lastStatus = (LONG)status;
        *BytesWrittenOut = headerSize;
        response->size = (ULONG)headerSize;
        return STATUS_SUCCESS;
    }

    findStatus = KswordARKSectionFindVad(
        processObject, &dynState, rangeStart, &vad, &vadStart, &vadEnd);
    if (findStatus == STATUS_NOT_SUPPORTED) {
        response->status = KSWORD_ARK_INJECTION_SCAN_STATUS_DYNDATA_MISSING;
        response->lastStatus = (LONG)findStatus;
        ObDereferenceObject(processObject);
        *BytesWrittenOut = headerSize;
        response->size = (ULONG)headerSize;
        return STATUS_SUCCESS;
    }
    if (!NT_SUCCESS(findStatus) || vad.FirstPrototypePte == NULL) {
        // 私有内存 / 找不到 VAD / 读不到长字段：都不是"参考是空的"，
        // 而是"这段范围没有节对象参考"。状态分开报，调用方才能区分。
        response->status = (findStatus == STATUS_NOT_FOUND)
            ? KSWORD_ARK_INJECTION_SCAN_STATUS_OK
            : KSWORD_ARK_INJECTION_SCAN_STATUS_WALK_FAILED;
        response->lastStatus = (LONG)findStatus;
        ObDereferenceObject(processObject);
        *BytesWrittenOut = headerSize;
        response->size = (ULONG)headerSize;
        return STATUS_SUCCESS;
    }

    // ControlArea = *(PVOID*)Subsection，Segment = *(PVOID*)ControlArea。
    // 两者都在偏移 0，是这条链上最稳的两跳。解引用失败只丢诊断，不影响读参考页 ——
    // 索引用的是 FirstPrototypePte，不经过它们。
    {
        PVOID controlArea = NULL;
        PVOID segment = NULL;
        if (vad.Subsection != NULL &&
            KswordARKSectionReadKernel(vad.Subsection, &controlArea, sizeof(controlArea))) {
            response->controlArea = (ULONG64)(ULONG_PTR)controlArea;
            if (controlArea != NULL &&
                KswordARKSectionReadKernel(controlArea, &segment, sizeof(segment))) {
                response->segment = (ULONG64)(ULONG_PTR)segment;
            }
        }
        response->prototypePteArray = (ULONG64)(ULONG_PTR)vad.FirstPrototypePte;
    }

    if (rangeEnd > vadEnd) {
        rangeEnd = vadEnd;   // 一次只处理一个 VAD；跨 VAD 由调用方按游标再来
    }
    if (includeBytes) {
        // 字节区的位置必须告诉调用方：它取决于 entryCapacity，
        // 而 entryCapacity 同时受缓冲大小与 maxPages 约束，调用方算不出来。
        response->byteAreaOffset =
            (ULONG64)(headerSize +
                      (entryCapacity * sizeof(KSWORD_ARK_IMAGE_SECTION_PAGE_ENTRY)));
        byteArea = (UCHAR*)OutputBuffer + response->byteAreaOffset;
    } else {
        byteArea = NULL;
    }

    for (va = rangeStart; va < rangeEnd; va += PAGE_SIZE) {
        KSWORD_ARK_IMAGE_SECTION_PAGE_ENTRY* entry = NULL;
        ULONG64 index = 0ULL;
        const UCHAR* ptePointer = NULL;
        ULONG64 pteValue = 0ULL;

        if ((size_t)response->returnedCount >= entryCapacity) {
            truncated = TRUE;
            response->nextCursorVa = va;
            response->fieldFlags |= KSWORD_ARK_INJECTION_FIELD_CURSOR_PRESENT;
            break;
        }

        entry = &response->entries[response->returnedCount];
        RtlZeroMemory(entry, sizeof(*entry));
        entry->va = va;

        index = (va - vadStart) / PAGE_SIZE;
        ptePointer = (const UCHAR*)vad.FirstPrototypePte + (index * sizeof(ULONG64));
        // 连续段的硬边界。超出 LastContiguousPte 的页要走 Subsection 链才找得到，
        // 本版本不做 —— 报"解析不到"而不是接着往后算，算过去就是在读别人的内存。
        if (vad.LastContiguousPte != NULL &&
            (const UCHAR*)ptePointer > (const UCHAR*)vad.LastContiguousPte) {
            entry->entryFlags |= KSWORD_ARK_INJECTION_SECTION_ENTRY_FLAG_PTE_UNREADABLE;
            ++response->unreadablePteCount;
            ++response->returnedCount;
            continue;
        }
        entry->prototypePteAddress = (ULONG64)(ULONG_PTR)ptePointer;

        if (!KswordARKSectionReadKernel(ptePointer, &pteValue, sizeof(pteValue))) {
            entry->entryFlags |= KSWORD_ARK_INJECTION_SECTION_ENTRY_FLAG_PTE_UNREADABLE;
            ++response->unreadablePteCount;
            ++response->returnedCount;
            continue;
        }
        entry->prototypePteValue = pteValue;

        if ((pteValue & KSW_SEC_PTE_VALID_BIT) == 0ULL) {
            // transition / 页面文件 / 零页。位布局随版本变，不解码；
            // 也**不把它调进来**。这一页当前拿不到参考，如实报。
            entry->entryFlags |= KSWORD_ARK_INJECTION_SECTION_ENTRY_FLAG_NOT_RESIDENT;
            ++response->notResidentPageCount;
            ++response->returnedCount;
            continue;
        }

        entry->physicalAddress =
            ((pteValue >> KSW_SEC_PTE_PFN_SHIFT) & KSW_SEC_PTE_PFN_MASK) << KSW_SEC_PTE_PFN_SHIFT;
        entry->entryFlags |= KSWORD_ARK_INJECTION_SECTION_ENTRY_FLAG_VALID;
        ++response->validPageCount;

        if (includeBytes && byteArea != NULL) {
            UCHAR* target = byteArea + ((size_t)response->validPageCount - 1U) * KSW_SEC_PAGE_BYTES;
            if (KswordARKSectionReadPhysicalPage(entry->physicalAddress, target)) {
                entry->entryFlags |= KSWORD_ARK_INJECTION_SECTION_ENTRY_FLAG_BYTES_PRESENT;
            }
        }
        ++response->returnedCount;
    }

    if (response->returnedCount != 0U) {
        response->fieldFlags |= KSWORD_ARK_INJECTION_FIELD_ENTRIES_PRESENT;
    }
    if (truncated) {
        response->status = KSWORD_ARK_INJECTION_SCAN_STATUS_TRUNCATED;
    } else if (response->unreadablePteCount != 0U) {
        response->status = KSWORD_ARK_INJECTION_SCAN_STATUS_PARTIAL;
    } else {
        response->status = KSWORD_ARK_INJECTION_SCAN_STATUS_OK;
    }

    *BytesWrittenOut = headerSize +
                       ((size_t)response->returnedCount *
                        sizeof(KSWORD_ARK_IMAGE_SECTION_PAGE_ENTRY));
    if (includeBytes) {
        // 字节区按 entryCapacity 对齐排布，返回长度要覆盖到最后一页。
        *BytesWrittenOut = headerSize +
                           (entryCapacity * sizeof(KSWORD_ARK_IMAGE_SECTION_PAGE_ENTRY)) +
                           ((size_t)response->validPageCount * KSW_SEC_PAGE_BYTES);
    }
    response->size = (ULONG)*BytesWrittenOut;
    ObDereferenceObject(processObject);
    return STATUS_SUCCESS;
}
