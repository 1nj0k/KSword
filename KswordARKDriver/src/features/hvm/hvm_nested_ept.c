/*++

Module Name:

    hvm_nested_ept.c

Abstract:

    Implements shadow-EPT composition for L2 execution.

Environment:

    Kernel-mode Driver Framework.

--*/

#include "hvm_nested_ept.h"
#include "hvm_ept.h"

#if defined(_M_AMD64)

#include "../../platform/pool_compat.h"

/* Tag the single per-processor block backing every shadow table. */
#define KSW_HVM_NEPT_POOL_TAG 'NvHK'

/* Name the read, write and execute permission bits of an EPT entry. */
#define KSW_HVM_NEPT_READ 0x1ULL
#define KSW_HVM_NEPT_WRITE 0x2ULL
#define KSW_HVM_NEPT_EXECUTE 0x4ULL
#define KSW_HVM_NEPT_PERMISSIONS \
    (KSW_HVM_NEPT_READ | KSW_HVM_NEPT_WRITE | KSW_HVM_NEPT_EXECUTE)
/* Name the bit that makes an interior entry a leaf. */
#define KSW_HVM_NEPT_LARGE 0x80ULL
/* Name the frame field of an EPT entry. */
#define KSW_HVM_NEPT_FRAME_MASK 0x000FFFFFFFFFF000ULL
/* Name write-back in the memory-type field of a leaf. */
#define KSW_HVM_NEPT_MEMORY_TYPE_WB 0x30ULL
/* Name the four-level page-walk length an EPT pointer encodes. */
#define KSW_HVM_NEPT_EPTP_WALK_4 0x18ULL
/* Name the EPT-pointer bit that asks the processor to maintain A/D flags. */
#define KSW_HVM_NEPT_EPTP_ENABLE_AD (1ULL << 6)
/* Name the two leaf bits the processor maintains: accessed (8), dirty (9). */
#define KSW_HVM_NEPT_AD_BITS ((1ULL << 8) | (1ULL << 9))

/*
 * Answer whether this processor can maintain EPT accessed/dirty flags.
 *
 * Asked of the capability MSR rather than assumed from the fact that L1 asked:
 * L1 reads the same MSR, but it reads it through us, and nothing guarantees
 * the two views agree on a machine where an outer hypervisor filters it.
 * Setting EPTP bit 6 on a processor that cannot honour it fails VM entry with
 * an error L1 has no way to act on.
 */
static BOOLEAN
KswordARKHvmNestedEptProcessorSupportsAccessedDirty(
    VOID
    )
{
    /* IA32_VMX_EPT_VPID_CAP bit 21 reports EPT A/D support. */
    const ULONGLONG capability = __readmsr(0x48CUL);

    /* Report exactly what the processor claims. */
    return ((capability & (1ULL << 21)) != 0ULL) ? TRUE : FALSE;
}
/* Name write-back in the memory-type field of an EPT pointer. */
#define KSW_HVM_NEPT_EPTP_MEMORY_TYPE_WB 0x6ULL

/* Hand out one zero-initialized page from the processor's block. */
static PVOID
KswordARKHvmNestedEptTakePage(
    _Inout_ KSW_HVM_SHADOW_EPT_STATE* Shadow,
    _Out_ ULONGLONG* PhysicalAddress
    )
{
    PVOID page = NULL;
    PHYSICAL_ADDRESS physical = { 0 };

    *PhysicalAddress = 0ULL;
    /* Report exhaustion rather than running past the block. */
    if (Shadow->PageBlock == NULL ||
        Shadow->PageUsed >= Shadow->PageTotal) {
        /* Return no page for an exhausted block. */
        return NULL;
    }
    page = (PVOID)((PUCHAR)Shadow->PageBlock +
        ((SIZE_T)Shadow->PageUsed * (SIZE_T)PAGE_SIZE));
    RtlZeroMemory(page, PAGE_SIZE);
    physical = MmGetPhysicalAddress(page);
    *PhysicalAddress = (ULONGLONG)physical.QuadPart;
    Shadow->PagePhysical[Shadow->PageUsed] =
        (ULONGLONG)physical.QuadPart & KSW_HVM_NEPT_FRAME_MASK;
    Shadow->PageUsed += 1UL;
    /* Return one page whose physical identity is already resolved. */
    return page;
}

/*
 * Navigate one interior entry back to the table it names.
 *
 * Only pages this record handed out are accepted.  A frame we never issued
 * means the entry was not written by us - either an invariant is broken or
 * something outside edited the hierarchy - and the honest response is to stop
 * rather than follow a pointer into memory of unknown ownership.
 */
static volatile ULONGLONG*
KswordARKHvmNestedEptPageVirtual(
    _In_ const KSW_HVM_SHADOW_EPT_STATE* Shadow,
    _In_ ULONGLONG Entry
    )
{
    const ULONGLONG frame = Entry & KSW_HVM_NEPT_FRAME_MASK;
    ULONG index = 0UL;

    for (index = 0UL; index < Shadow->PageUsed; ++index) {
        if (Shadow->PagePhysical[index] == frame) {
            /* Return the table this record issued for that frame. */
            return (volatile ULONGLONG*)((PUCHAR)Shadow->PageBlock +
                ((SIZE_T)index * (SIZE_T)PAGE_SIZE));
        }
    }
    /* Return nothing for a frame this record never issued. */
    return NULL;
}

VOID
KswordARKHvmNestedEptInitialize(
    _Out_ KSW_HVM_SHADOW_EPT_STATE* Shadow,
    _In_ ULONGLONG L0EptPointer
    )
{
    /* Ignore a missing record rather than fault on initialization. */
    if (Shadow == NULL) {
        /* Return without touching absent state. */
        return;
    }
    RtlZeroMemory(Shadow, sizeof(*Shadow));
    Shadow->L0EptPointer = L0EptPointer;
    Shadow->LastStatus = STATUS_SUCCESS;
}

NTSTATUS
KswordARKHvmNestedEptPrepare(
    _Inout_ KSW_HVM_SHADOW_EPT_STATE* Shadow
    )
{
    ULONGLONG rootPhysical = 0ULL;

    /* Reject an incomplete caller contract before reserving anything. */
    if (Shadow == NULL) {
        /* Return the exact caller-contract failure. */
        return STATUS_INVALID_PARAMETER;
    }
    /* Keep an existing reservation rather than leaking a second one. */
    if (Shadow->PageBlock != NULL) {
        /* Return the already-complete reservation. */
        return STATUS_SUCCESS;
    }
    /*
     * One allocation for the whole processor, from the pool rather than
     * contiguous memory.  Releasing happens on the residency teardown path,
     * which a power callback can reach and where PASSIVE_LEVEL is not
     * guaranteed - the same constraint that already shapes the private EPT
     * hierarchies and the host stacks.
     */
    Shadow->PageBlock = KswordARKAllocateNonPagedPool(
        (SIZE_T)KSW_HVM_NEPT_TABLE_PAGES * (SIZE_T)PAGE_SIZE,
        KSW_HVM_NEPT_POOL_TAG);
    /* Leave composition unavailable when the reservation fails. */
    if (Shadow->PageBlock == NULL) {
        Shadow->LastStatus = STATUS_INSUFFICIENT_RESOURCES;
        /* Return the exact nonpaged-resource failure. */
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(
        Shadow->PageBlock,
        (SIZE_T)KSW_HVM_NEPT_TABLE_PAGES * (SIZE_T)PAGE_SIZE);
    Shadow->PageTotal = KSW_HVM_NEPT_TABLE_PAGES;
    Shadow->PageUsed = 0UL;
    /*
     * Room for a copy of every EPT12 table page the hierarchy depends on.
     *
     * A second allocation rather than a bigger first one: this block is read
     * and compared, never handed out as a table, and keeping the two apart
     * means a bug in one cannot hand the processor a page from the other.
     * Failure is not fatal - it costs the right to keep the shadow across an
     * invalidation, which is exactly what the code did before this existed.
     */
    Shadow->TrackedCopyBlock = KswordARKAllocateNonPagedPool(
        (SIZE_T)KSW_HVM_NEPT_TRACKED_PAGES * (SIZE_T)PAGE_SIZE,
        KSW_HVM_NEPT_POOL_TAG);
    if (Shadow->TrackedCopyBlock != NULL) {
        RtlZeroMemory(
            Shadow->TrackedCopyBlock,
            (SIZE_T)KSW_HVM_NEPT_TRACKED_PAGES * (SIZE_T)PAGE_SIZE);
    }
    Shadow->TrackedCount = 0UL;
    Shadow->TrackedOverflowCount = 0UL;
    /* Take the root first so every fill below has somewhere to publish. */
    Shadow->RootVirtual = KswordARKHvmNestedEptTakePage(
        Shadow,
        &rootPhysical);
    if (Shadow->RootVirtual == NULL) {
        ExFreePool(Shadow->PageBlock);
        Shadow->PageBlock = NULL;
        Shadow->PageTotal = 0UL;
        Shadow->LastStatus = STATUS_INSUFFICIENT_RESOURCES;
        /* Return the exact bounded-resource failure. */
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Shadow->RootPhysical = rootPhysical;
    /*
     * The composed pointer names our root, never L1's.
     *
     * Walk length and memory type come from what the processor supports, not
     * from what L1 asked for: L1's EPT12 is data we interpret, and copying its
     * pointer format would let a malformed one reach the hardware EPTP.
     */
    Shadow->ComposedEptPointer =
        (rootPhysical & KSW_HVM_NEPT_FRAME_MASK) |
        KSW_HVM_NEPT_EPTP_WALK_4 |
        KSW_HVM_NEPT_EPTP_MEMORY_TYPE_WB;
    Shadow->LastStatus = STATUS_SUCCESS;
    /* Return a complete reservation. */
    return STATUS_SUCCESS;
}

VOID
KswordARKHvmNestedEptRelease(
    _Inout_ KSW_HVM_SHADOW_EPT_STATE* Shadow
    )
{
    /* Ignore a record whose reservation never completed. */
    if (Shadow == NULL || Shadow->PageBlock == NULL) {
        /* Return without touching an absent reservation. */
        return;
    }
    ExFreePool(Shadow->PageBlock);
    Shadow->PageBlock = NULL;
    if (Shadow->TrackedCopyBlock != NULL) {
        ExFreePool(Shadow->TrackedCopyBlock);
        Shadow->TrackedCopyBlock = NULL;
    }
    Shadow->TrackedCount = 0UL;
    Shadow->TrackedOverflowCount = 0UL;
    Shadow->PageTotal = 0UL;
    Shadow->PageUsed = 0UL;
    Shadow->RootVirtual = NULL;
    Shadow->RootPhysical = 0ULL;
    Shadow->ComposedEptPointer = 0ULL;
    Shadow->Active = FALSE;
}

ULONG
KswordARKHvmNestedEptPropagateAccessedDirty(
    _Inout_ KSW_HVM_SHADOW_EPT_STATE* Shadow,
    _Inout_ KSW_HVM_PHYS_WINDOW* Window
    )
{
    /* Index shifts for PML4, PDPT, PD and PT in walk order. */
    static const ULONG shifts[4] = { 39UL, 30UL, 21UL, 12UL };
    ULONG updated = 0UL;
    ULONG record = 0UL;

    if (Shadow == NULL || Window == NULL ||
        !Shadow->AccessedDirtyActive || Shadow->RootVirtual == NULL) {
        /* Report that nothing was folded. */
        return 0UL;
    }
    for (record = 0UL; record < Shadow->AdRecordCount; ++record) {
        const ULONGLONG guestPhysical = Shadow->AdLeafGuestPhysical[record];
        const ULONGLONG l1Entry = Shadow->AdL1EntryAddress[record];
        volatile ULONGLONG* table = (volatile ULONGLONG*)Shadow->RootVirtual;
        ULONGLONG shadowLeaf = 0ULL;
        ULONGLONG l1Value = 0ULL;
        ULONG level = 0UL;

        if (l1Entry == 0ULL) {
            /* Skip a record whose EPT12 entry was never resolved. */
            continue;
        }
        /* Navigate our own hierarchy to the leaf this record names. */
        for (level = 0UL; level < 3UL; ++level) {
            const ULONGLONG entry =
                table[(guestPhysical >> shifts[level]) & 0x1FFULL];

            if ((entry & KSW_HVM_NEPT_PERMISSIONS) == 0ULL) {
                table = NULL;
                break;
            }
            table = KswordARKHvmNestedEptPageVirtual(Shadow, entry);
            if (table == NULL) { break; }
        }
        if (table == NULL) {
            /* Skip a leaf the hierarchy no longer describes. */
            continue;
        }
        shadowLeaf = table[(guestPhysical >> shifts[3]) & 0x1FFULL];
        /*
         * Only the two bits, and only ever setting them.
         *
         * L1 owns everything else in that entry, including whether the bits
         * were already set and whether it has since cleared them to start a
         * new round of tracking.  Writing the whole value back would undo any
         * change L1 made while L2 was running; clearing a bit would lose a
         * write L1 has not yet accounted for.  OR of just these two is the
         * only operation that cannot lose information either way.
         */
        if ((shadowLeaf & KSW_HVM_NEPT_AD_BITS) == 0ULL) {
            /* Skip a leaf the processor never touched. */
            continue;
        }
        if (!NT_SUCCESS(KswordARKHvmPhysWindowReadQword(
                Window,
                l1Entry,
                &l1Value))) {
            /* Skip an entry that could not be read back. */
            continue;
        }
        if ((l1Value & (shadowLeaf & KSW_HVM_NEPT_AD_BITS)) ==
                (shadowLeaf & KSW_HVM_NEPT_AD_BITS)) {
            /* Skip an entry that already carries these bits. */
            continue;
        }
        l1Value |= (shadowLeaf & KSW_HVM_NEPT_AD_BITS);
        if (!NT_SUCCESS(KswordARKHvmPhysWindowWriteQword(
                Window,
                l1Entry,
                l1Value))) {
            /* Skip an entry that could not be written. */
            continue;
        }
        updated += 1UL;
    }
    Shadow->AdPropagatedCount += updated;
    /* Report how many EPT12 entries actually changed. */
    return updated;
}

VOID
KswordARKHvmNestedEptInvalidate(
    _Inout_ KSW_HVM_SHADOW_EPT_STATE* Shadow
    )
{
    /* Ignore a record with nothing composed. */
    if (Shadow == NULL || Shadow->RootVirtual == NULL) {
        /* Return without dropping absent mappings. */
        return;
    }
    /*
     * Drop everything rather than the one entry L1 named.
     *
     * Precise invalidation needs a reverse map from L1 physical back to every
     * L2 page that composed through it, and nothing here maintains one.
     * Dropping the whole hierarchy costs refills; dropping the wrong subset
     * costs an L2 running on a translation L1 already retired, with no symptom
     * until the memory underneath it is reused.  Coarse and certain beats
     * precise and unproven.
     */
    RtlZeroMemory(Shadow->RootVirtual, PAGE_SIZE);
    Shadow->PageUsed = 1UL;
    /*
     * The copies describe a hierarchy that no longer exists.
     *
     * What gets composed next may walk a different set of EPT12 pages, and
     * comparing the next invalidation against copies taken for the old one
     * would vouch for pages the new mappings never read.  Forgetting them
     * costs one snapshot per table page on the way back up.
     */
    Shadow->TrackedCount = 0UL;
    Shadow->TrackedOverflowCount = 0UL;
    /*
     * The A/D records describe leaves that no longer exist.
     *
     * Keeping them would have the next propagation read bits out of table
     * pages that have since been handed to a different guest-physical address,
     * and write them into EPT12 entries for pages L2 never touched.  The
     * caller is expected to have propagated before invalidating; anything not
     * folded by then is lost, which is the same thing INVEPT means for the
     * translations themselves.
     */
    Shadow->AdRecordCount = 0UL;
    /*
     * Zeroing the tables is not the whole job.
     *
     * The processor caches translations derived from them, and those survive
     * an edit to the memory they came from - that is what INVEPT exists for.
     * Dropping the tables without invalidating leaves L2 running on exactly
     * the mappings this call was made to retire, and the tables now say
     * nothing, so nothing later will contradict the stale entry either.
     */
    if (Shadow->ComposedEptPointer != 0ULL) {
        (void)KswordARKHvmAsmInveptSingle(Shadow->ComposedEptPointer);
    }
    Shadow->InvalidationGeneration = Shadow->Generation;
    Shadow->Generation += 1UL;
}

NTSTATUS
KswordARKHvmNestedEptSetL1Pointer(
    _Inout_ KSW_HVM_SHADOW_EPT_STATE* Shadow,
    _In_ ULONGLONG L1EptPointer
    )
{
    /* Reject an incomplete caller contract before recording anything. */
    if (Shadow == NULL) {
        /* Return the exact caller-contract failure. */
        return STATUS_INVALID_PARAMETER;
    }
    /* Refuse to arm composition without a reserved hierarchy. */
    if (Shadow->RootVirtual == NULL) {
        Shadow->LastStatus = STATUS_INSUFFICIENT_RESOURCES;
        /* Return the exact unavailable-reservation failure. */
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    /* Reject an EPT pointer whose frame the architecture cannot encode. */
    if ((L1EptPointer & KSW_HVM_NEPT_FRAME_MASK) == 0ULL) {
        Shadow->LastStatus = STATUS_INVALID_PARAMETER;
        /* Return the exact encoding failure. */
        return STATUS_INVALID_PARAMETER;
    }
    /*
     * Accessed/dirty is maintained and folded back, not refused.
     *
     * L1 asking for A/D is L1 saying it intends to read those bits back out of
     * its own EPT12 - that is the only thing they are for, and every use of
     * them (live migration, snapshots, copy-on-write) decides which pages to
     * copy from exactly that readback.
     *
     * L2 runs on the composed hierarchy, so the processor sets A/D in *our*
     * shadow leaves.  Leaving it there means L1 reads its own tables back,
     * finds every bit clear, and skips exactly the pages its guest modified -
     * silently, with nothing anywhere reporting it.  So the bits are folded
     * into EPT12 when L2 stops, using the leaf addresses recorded during
     * composition.
     *
     * Two things still have to be true, and both are checked rather than
     * assumed: the processor must be able to maintain the bits at all, and the
     * record table must not overflow.  Either failing turns the feature off
     * and refuses the entry, because half-propagated A/D is worse than none.
     */
    if ((L1EptPointer & KSW_HVM_NEPT_EPTP_ENABLE_AD) != 0ULL) {
        Shadow->L1RequestedAccessedDirty = TRUE;
        if (!KswordARKHvmNestedEptProcessorSupportsAccessedDirty()) {
            Shadow->AccessedDirtyActive = FALSE;
            Shadow->LastStatus = STATUS_NOT_SUPPORTED;
            /* Return the exact unsupported-control failure. */
            return STATUS_NOT_SUPPORTED;
        }
        Shadow->AccessedDirtyActive = TRUE;
    } else {
        Shadow->L1RequestedAccessedDirty = FALSE;
        Shadow->AccessedDirtyActive = FALSE;
    }
    /* Drop every mapping composed against a different EPT12. */
    if (Shadow->L1EptPointer != L1EptPointer) {
        KswordARKHvmNestedEptInvalidate(Shadow);
        Shadow->L1EptPointer = L1EptPointer;
    }
    /*
     * Put A/D into the pointer the processor actually loads.
     *
     * The composed pointer is built once at reservation time, before anything
     * knows what L1 will ask for, so this bit can only be decided here.  It is
     * assigned in both directions: a stale set bit from a previous L1 would
     * have the processor maintaining bits nobody is folding back.
     */
    if (Shadow->AccessedDirtyActive) {
        Shadow->ComposedEptPointer |= KSW_HVM_NEPT_EPTP_ENABLE_AD;
    } else {
        Shadow->ComposedEptPointer &= ~KSW_HVM_NEPT_EPTP_ENABLE_AD;
    }
    Shadow->L1PointerValid = TRUE;
    Shadow->Active = TRUE;
    Shadow->LastStatus = STATUS_SUCCESS;
    /* Return the armed composition. */
    return STATUS_SUCCESS;
}

/*
 * Take a private copy of one EPT12 table page, once.
 *
 * Called from the walk, so it runs in VMX root and must map through the
 * per-processor window like everything else here.  A frame already tracked is
 * left alone: the copy has to be of what the hierarchy was *composed from*,
 * and re-snapshotting on a later walk would quietly absorb an edit L1 made in
 * between - which is exactly the edit this exists to catch.
 */
static VOID
KswordARKHvmNestedEptTrackTablePage(
    _Inout_ KSW_HVM_SHADOW_EPT_STATE* Shadow,
    _Inout_ KSW_HVM_PHYS_WINDOW* Window,
    _In_ ULONGLONG TableFrame
    )
{
    volatile VOID* mapped = NULL;
    ULONG index = 0UL;

    if (Shadow->TrackedCopyBlock == NULL || TableFrame == 0ULL) {
        /* Report nothing; the invalidation path treats absent as unknown. */
        return;
    }
    for (index = 0UL; index < Shadow->TrackedCount; ++index) {
        if (Shadow->TrackedFrame[index] == TableFrame) {
            /* Return; the copy that matters is the first one. */
            return;
        }
    }
    if (Shadow->TrackedCount >= KSW_HVM_NEPT_TRACKED_PAGES) {
        Shadow->TrackedOverflowCount += 1UL;
        /* Return; the count is what forfeits the right to keep the shadow. */
        return;
    }
    if (KswordARKHvmPhysWindowMap(
            Window,
            TableFrame,
            PAGE_SIZE,
            &mapped) != KSW_HVM_PHYS_WINDOW_OK ||
        mapped == NULL) {
        Shadow->TrackedOverflowCount += 1UL;
        /* Return; an untaken copy is counted the same as no room for one. */
        return;
    }
    RtlCopyMemory(
        (UCHAR*)Shadow->TrackedCopyBlock +
            ((SIZE_T)Shadow->TrackedCount * (SIZE_T)PAGE_SIZE),
        (const VOID*)mapped,
        PAGE_SIZE);
    KswordARKHvmPhysWindowUnmap(Window);
    Shadow->TrackedFrame[Shadow->TrackedCount] = TableFrame;
    Shadow->TrackedCount += 1UL;
}

BOOLEAN
KswordARKHvmNestedEptInvalidateChecked(
    _Inout_ KSW_HVM_SHADOW_EPT_STATE* Shadow,
    _Inout_ KSW_HVM_PHYS_WINDOW* Window
    )
{
    ULONG index = 0UL;

    if (Shadow == NULL || Window == NULL ||
        Shadow->RootVirtual == NULL) {
        /* Report that nothing was kept, because nothing was composed. */
        return FALSE;
    }
    /*
     * A hierarchy we could not fully snapshot has to be dropped.
     *
     * Overflow means at least one table page the mappings depend on has no
     * copy, so "unchanged" cannot be established for it.  Keeping the shadow
     * on the strength of the pages we did copy would be asserting something
     * about the ones we did not.
     */
    if (Shadow->TrackedCopyBlock == NULL ||
        Shadow->TrackedCount == 0UL ||
        Shadow->TrackedOverflowCount != 0UL) {
        KswordARKHvmNestedEptInvalidate(Shadow);
        Shadow->InvalidateDroppedCount += 1UL;
        /* Report the drop. */
        return FALSE;
    }
    for (index = 0UL; index < Shadow->TrackedCount; ++index) {
        volatile VOID* mapped = NULL;
        BOOLEAN same = FALSE;

        if (KswordARKHvmPhysWindowMap(
                Window,
                Shadow->TrackedFrame[index],
                PAGE_SIZE,
                &mapped) != KSW_HVM_PHYS_WINDOW_OK ||
            mapped == NULL) {
            /* A page we cannot re-read is a page we cannot vouch for. */
            KswordARKHvmNestedEptInvalidate(Shadow);
            Shadow->InvalidateDroppedCount += 1UL;
            /* Report the drop. */
            return FALSE;
        }
        same = (RtlCompareMemory(
            (const VOID*)mapped,
            (const UCHAR*)Shadow->TrackedCopyBlock +
                ((SIZE_T)index * (SIZE_T)PAGE_SIZE),
            PAGE_SIZE) == PAGE_SIZE) ? TRUE : FALSE;
        KswordARKHvmPhysWindowUnmap(Window);
        if (!same) {
            KswordARKHvmNestedEptInvalidate(Shadow);
            Shadow->InvalidateDroppedCount += 1UL;
            /* Report the drop; L1 really did edit its tables. */
            return FALSE;
        }
    }
    /*
     * Every table page is byte-identical, so the composed mappings still say
     * exactly what EPT12 says.  What remains is the processor's own caches,
     * which is the part INVEPT genuinely always means.
     */
    if (Shadow->ComposedEptPointer != 0ULL) {
        (void)KswordARKHvmAsmInveptSingle(Shadow->ComposedEptPointer);
    }
    Shadow->InvalidationGeneration = Shadow->Generation;
    Shadow->InvalidateKeptCount += 1UL;
    /* Report that the hierarchy was kept. */
    return TRUE;
}

/*
 * Walk EPT12 for one L2 guest physical address.
 *
 * Every level is read through the window because EPT12's tables live at L1
 * physical addresses, which under our identity EPT01 are host physical
 * addresses - readable only through a mapping we create.
 */
static BOOLEAN
KswordARKHvmNestedEptWalkL1(
    _Inout_ KSW_HVM_SHADOW_EPT_STATE* Shadow,
    _Inout_ KSW_HVM_PHYS_WINDOW* Window,
    _In_ ULONGLONG GuestPhysicalAddress,
    _Out_ ULONGLONG* L1Physical,
    _Out_ ULONGLONG* Permissions,
    _Out_opt_ ULONGLONG* L1EntryAddress
    )
{
    /* Index shifts for PML4, PDPT, PD and PT in walk order. */
    static const ULONG shifts[4] = { 39UL, 30UL, 21UL, 12UL };
    ULONGLONG table = Shadow->L1EptPointer & KSW_HVM_NEPT_FRAME_MASK;
    ULONGLONG permissions = KSW_HVM_NEPT_PERMISSIONS;
    ULONG level = 0UL;

    *L1Physical = 0ULL;
    *Permissions = 0ULL;
    if (L1EntryAddress != NULL) { *L1EntryAddress = 0ULL; }
    for (level = 0UL; level < 4UL; ++level) {
        const ULONGLONG index =
            (GuestPhysicalAddress >> shifts[level]) & 0x1FFULL;
        const ULONGLONG entryAddress = table + (index << 3);
        ULONGLONG entry = 0ULL;

        /*
         * Copy this table page before reading anything out of it.
         *
         * Before, not after: the copy has to be of the bytes the mapping is
         * about to be composed from, so that a later comparison answers "is
         * the hierarchy still what L1's tables say" rather than "did anything
         * change since some arbitrary moment".
         */
        KswordARKHvmNestedEptTrackTablePage(Shadow, Window, table);
        if (!NT_SUCCESS(KswordARKHvmPhysWindowReadQword(
                Window,
                entryAddress,
                &entry))) {
            /* Report that EPT12 could not be walked at all. */
            return FALSE;
        }
        /*
         * Remember where the entry that decides this page lives.
         *
         * Only meaningful at the last level, and only known here - after the
         * walk returns, `table` is gone and recovering this address would mean
         * walking EPT12 again, from a VM exit, for every page.
         */
        if (L1EntryAddress != NULL) { *L1EntryAddress = entryAddress; }
        /*
         * Accumulate permissions down the walk, never widen them.
         *
         * An interior entry that denies write denies it for everything
         * beneath, so the effective permission is the intersection - taking
         * only the leaf's bits would grant access L1 revoked one level up.
         */
        permissions &= entry;
        /* A wholly unreadable entry terminates the walk with no mapping. */
        if ((entry & KSW_HVM_NEPT_PERMISSIONS) == 0ULL) {
            /*
             * Keep which level stopped and what it read.
             *
             * "The walk found nothing" is not actionable on its own: stopping
             * at the PML4 means L1 has not built this half of the address
             * space at all, stopping at the PT means one page is absent, and
             * an entry that is nonzero but permissionless means L1 deliberately
             * revoked it.  Those are three different defects and one count.
             */
            Shadow->LastDenySite = 1UL;
            Shadow->LastDenyLevel = level;
            Shadow->LastDenyEntry = entry;
            Shadow->LastDenyGuestPhysical = GuestPhysicalAddress;
            /* Report that EPT12 maps nothing here. */
            return FALSE;
        }
        /* Resolve large leaves at the levels that may terminate a walk. */
        if ((level >= 1UL && level <= 2UL &&
                (entry & KSW_HVM_NEPT_LARGE) != 0ULL) ||
            level == 3UL) {
            const ULONGLONG offsetMask =
                (level == 3UL)
                    ? (PAGE_SIZE - 1ULL)
                    : ((1ULL << shifts[level]) - 1ULL);

            *L1Physical =
                ((entry & KSW_HVM_NEPT_FRAME_MASK) & ~offsetMask) |
                (GuestPhysicalAddress & offsetMask);
            *Permissions = permissions & KSW_HVM_NEPT_PERMISSIONS;
            /* Report a complete EPT12 translation. */
            return TRUE;
        }
        table = entry & KSW_HVM_NEPT_FRAME_MASK;
    }
    /* Report that the walk ran out of levels without a leaf. */
    return FALSE;
}

BOOLEAN
KswordARKHvmNestedEptFill(
    _Inout_ struct _KSW_HVM_RUNTIME* Runtime,
    _Inout_ KSW_HVM_SHADOW_EPT_STATE* Shadow,
    _Inout_ KSW_HVM_PHYS_WINDOW* Window,
    _In_ ULONGLONG GuestPhysicalAddress,
    _In_ ULONG Access
    )
{
    /* Index shifts for PML4, PDPT, PD and PT in walk order. */
    static const ULONG shifts[4] = { 39UL, 30UL, 21UL, 12UL };
    ULONGLONG l1Physical = 0ULL;
    ULONGLONG permissions = 0ULL;
    /* Where EPT12's own leaf for this page lives, for folding A/D back. */
    ULONGLONG l1EntryAddress = 0ULL;
    volatile ULONGLONG* table = NULL;
    const volatile ULONGLONG* hostLeaf = NULL;
    ULONG level = 0UL;

    /* Refuse composition without an armed hierarchy or a usable window. */
    if (Shadow == NULL || Window == NULL ||
        !Shadow->Active || Shadow->RootVirtual == NULL) {
        /* Report that this violation is not ours to satisfy. */
        return FALSE;
    }
    /* Translate through L1's own hierarchy first. */
    if (!KswordARKHvmNestedEptWalkL1(
            Shadow,
            Window,
            GuestPhysicalAddress,
            &l1Physical,
            &permissions,
            &l1EntryAddress)) {
        Shadow->DenyCount += 1UL;
        /* Report that EPT12 itself refuses this access. */
        return FALSE;
    }
    /*
     * Refuse when EPT12 grants less than the access needs.
     *
     * This is the violation L1 installed its EPT to receive, so it must reach
     * L1 rather than be satisfied here.  Composing a leaf that permits it
     * would silently defeat whatever L1 was protecting.
     */
    if ((Access & KSW_HVM_NEPT_PERMISSIONS) != 0UL &&
        ((ULONGLONG)Access & permissions) !=
            ((ULONGLONG)Access & KSW_HVM_NEPT_PERMISSIONS)) {
        Shadow->DenyCount += 1UL;
        Shadow->LastDenySite = 2UL;
        Shadow->LastDenyAccess = Access;
        Shadow->LastDenyPermissions = permissions;
        Shadow->LastDenyGuestPhysical = GuestPhysicalAddress;
        /* Report the violation as L1's to handle. */
        return FALSE;
    }
    /*
     * Intersect with our own leaf for the same page.
     *
     * EPT01 is an identity map, so the frame is unchanged - but a view or a
     * rule may have narrowed the permissions of that exact page, and L2 must
     * not be handed more than L1-as-a-guest already has.  A page we never
     * split has no four-KiB leaf and keeps the identity default.
     */
    hostLeaf = KswordARKHvmEptFindLeafEntry(Runtime, l1Physical);
    if (hostLeaf != NULL) {
        permissions &= (*hostLeaf & KSW_HVM_NEPT_PERMISSIONS);
        if (((ULONGLONG)Access & permissions) !=
                ((ULONGLONG)Access & KSW_HVM_NEPT_PERMISSIONS)) {
            Shadow->DenyCount += 1UL;
            Shadow->LastDenySite = 3UL;
            Shadow->LastDenyAccess = Access;
            Shadow->LastDenyPermissions = permissions;
            Shadow->LastDenyGuestPhysical = GuestPhysicalAddress;
            Shadow->LastDenyEntry = *hostLeaf;
            /* Report a violation our own hierarchy refuses. */
            return FALSE;
        }
    }
    /* Build the shadow path down to the four-KiB leaf. */
    table = (volatile ULONGLONG*)Shadow->RootVirtual;
    for (level = 0UL; level < 3UL; ++level) {
        const ULONGLONG index =
            (GuestPhysicalAddress >> shifts[level]) & 0x1FFULL;
        ULONGLONG entry = table[index];

        if ((entry & KSW_HVM_NEPT_PERMISSIONS) == 0ULL) {
            ULONGLONG childPhysical = 0ULL;
            PVOID child = KswordARKHvmNestedEptTakePage(
                Shadow,
                &childPhysical);

            /* Report exhaustion as a refusal, never as a crash. */
            if (child == NULL) {
                Shadow->ExhaustionCount += 1UL;
                Shadow->LastStatus = STATUS_INSUFFICIENT_RESOURCES;
                Shadow->LastDenySite = 4UL;
                Shadow->LastDenyLevel = level;
                Shadow->LastDenyGuestPhysical = GuestPhysicalAddress;
                /* Report that no mapping could be composed. */
                return FALSE;
            }
            /*
             * Interior entries carry full permissions.
             *
             * The leaf is where the intersection is expressed; narrowing an
             * interior entry would apply it to every page under that entry,
             * including ones composed later from different EPT12 leaves.
             */
            entry = (childPhysical & KSW_HVM_NEPT_FRAME_MASK) |
                KSW_HVM_NEPT_PERMISSIONS;
            table[index] = entry;
        }
        table = KswordARKHvmNestedEptPageVirtual(Shadow, entry);
        /* Refuse when an interior entry names a page we did not hand out. */
        if (table == NULL) {
            Shadow->LastStatus = STATUS_DATA_ERROR;
            Shadow->LastDenySite = 5UL;
            Shadow->LastDenyLevel = level;
            Shadow->LastDenyEntry = entry;
            Shadow->LastDenyGuestPhysical = GuestPhysicalAddress;
            /* Report that the hierarchy could not be navigated. */
            return FALSE;
        }
    }
    table[(GuestPhysicalAddress >> shifts[3]) & 0x1FFULL] =
        (l1Physical & KSW_HVM_NEPT_FRAME_MASK) |
        (permissions & KSW_HVM_NEPT_PERMISSIONS) |
        KSW_HVM_NEPT_MEMORY_TYPE_WB;
    /*
     * Pair this leaf with EPT12's, so the bits the processor is about to set
     * here can be folded back into L1's table when L2 stops.
     *
     * Recorded at composition rather than looked up later: after this returns,
     * the EPT12 leaf address is only recoverable by walking EPT12 again, from
     * a VM exit, for every page.
     */
    if (Shadow->AccessedDirtyActive) {
        if (Shadow->AdRecordCount < KSW_HVM_NEPT_AD_RECORDS) {
            Shadow->AdLeafGuestPhysical[Shadow->AdRecordCount] =
                GuestPhysicalAddress & KSW_HVM_NEPT_FRAME_MASK;
            Shadow->AdL1EntryAddress[Shadow->AdRecordCount] = l1EntryAddress;
            Shadow->AdRecordCount += 1UL;
        } else {
            /*
             * Out of records.  Fold what we have, empty the table, and keep
             * going.
             *
             * This used to set AccessedDirtyActive to FALSE - "a partial fold
             * is worse than none", which is true, but turning the feature off
             * mid-run *is* the partial fold: every page composed before the
             * overflow keeps its bits, every page after silently loses them,
             * and L1 has no way to tell the difference.  Six hundred and forty
             * records against a guest with a hundred and ninety thousand pages
             * means the overflow is not an edge case; measured 127 of them in
             * a single boot.
             *
             * What L1 loses by dirty tracking that stops is not an abstraction:
             * VMware write-protects the guest's text framebuffer only until
             * the writes get frequent, then maps it writable and finds the
             * changed pages from the dirty bits.  With the bits gone the
             * screen simply stops being repainted while the guest runs on -
             * which is exactly the reading that sent this investigation after
             * three different wrong devices, because "the console froze at
             * line N" was taken for "the guest stopped at line N".
             *
             * Folding here is safe: the fold only ever *sets* bits in EPT12,
             * and it reads them from shadow leaves this same hierarchy still
             * describes.  Records that no longer resolve are skipped by the
             * propagation itself.
             */
            (void)KswordARKHvmNestedEptPropagateAccessedDirty(Shadow, Window);
            Shadow->AdRecordCount = 0UL;
            Shadow->AdOverflowCount += 1UL;
            Shadow->AdLeafGuestPhysical[0] =
                GuestPhysicalAddress & KSW_HVM_NEPT_FRAME_MASK;
            Shadow->AdL1EntryAddress[0] = l1EntryAddress;
            Shadow->AdRecordCount = 1UL;
        }
    }
    Shadow->FillCount += 1UL;
    Shadow->LastStatus = STATUS_SUCCESS;
    /* Report that the faulting access may now be retried. */
    return TRUE;
}

#else

VOID
KswordARKHvmNestedEptInitialize(
    _Out_ KSW_HVM_SHADOW_EPT_STATE* Shadow,
    _In_ ULONGLONG L0EptPointer
    )
{
    UNREFERENCED_PARAMETER(L0EptPointer);
    /* Zero the record so no caller reads uninitialized shadow state. */
    if (Shadow != NULL) {
        RtlZeroMemory(Shadow, sizeof(*Shadow));
    }
}

NTSTATUS
KswordARKHvmNestedEptPrepare(
    _Inout_ KSW_HVM_SHADOW_EPT_STATE* Shadow
    )
{
    UNREFERENCED_PARAMETER(Shadow);
    /* Return the explicit unsupported-architecture boundary. */
    return STATUS_NOT_SUPPORTED;
}

VOID
KswordARKHvmNestedEptRelease(
    _Inout_ KSW_HVM_SHADOW_EPT_STATE* Shadow
    )
{
    UNREFERENCED_PARAMETER(Shadow);
}

NTSTATUS
KswordARKHvmNestedEptSetL1Pointer(
    _Inout_ KSW_HVM_SHADOW_EPT_STATE* Shadow,
    _In_ ULONGLONG L1EptPointer
    )
{
    UNREFERENCED_PARAMETER(Shadow);
    UNREFERENCED_PARAMETER(L1EptPointer);
    /* Return the explicit unsupported-architecture boundary. */
    return STATUS_NOT_SUPPORTED;
}

VOID
KswordARKHvmNestedEptInvalidate(
    _Inout_ KSW_HVM_SHADOW_EPT_STATE* Shadow
    )
{
    UNREFERENCED_PARAMETER(Shadow);
}

BOOLEAN
KswordARKHvmNestedEptInvalidateChecked(
    _Inout_ KSW_HVM_SHADOW_EPT_STATE* Shadow,
    _Inout_ KSW_HVM_PHYS_WINDOW* Window
    )
{
    UNREFERENCED_PARAMETER(Shadow);
    UNREFERENCED_PARAMETER(Window);
    /* Report the explicit unsupported-architecture boundary as "not kept". */
    return FALSE;
}

BOOLEAN
KswordARKHvmNestedEptFill(
    _Inout_ struct _KSW_HVM_RUNTIME* Runtime,
    _Inout_ KSW_HVM_SHADOW_EPT_STATE* Shadow,
    _Inout_ KSW_HVM_PHYS_WINDOW* Window,
    _In_ ULONGLONG GuestPhysicalAddress,
    _In_ ULONG Access
    )
{
    UNREFERENCED_PARAMETER(Runtime);
    UNREFERENCED_PARAMETER(Shadow);
    UNREFERENCED_PARAMETER(Window);
    UNREFERENCED_PARAMETER(GuestPhysicalAddress);
    UNREFERENCED_PARAMETER(Access);
    /* Report that no mapping could be composed. */
    return FALSE;
}

#endif
