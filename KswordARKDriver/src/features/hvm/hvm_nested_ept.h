/*++

Module Name:

    hvm_nested_ept.h

Abstract:

    Defines the shadow EPT that composes L1's EPT12 with our own EPT01 so the
    processor, which only walks one hierarchy, can run L2.

Environment:

    Kernel-mode Driver Framework.

--*/

#pragma once

#include "hvm_internal.h"
#include "hvm_phys_window.h"

/*
 * Bound the table pages one processor's shadow hierarchy may consume.
 *
 * Filling happens inside a VM exit, where allocation is not available, so
 * every table page has to exist before L2 starts.  The count covers a root
 * plus the interior tables an L2 working set touches; exhaustion is reported
 * as a denied violation rather than a crash, because the alternative to
 * "L2 cannot run this page" must not be "the host stops".
 */
#define KSW_HVM_NEPT_TABLE_PAGES 192UL
/*
 * How many composed leaves can have their A/D bits folded back into EPT12.
 *
 * One 4 KiB table page holds 512 leaves, so this is a little over a page's
 * worth - enough for the working set an L2 touches between exits, and small
 * enough that walking it on every exit stays cheap.  Overflow is handled by
 * turning A/D off rather than propagating some of it; see the record arrays.
 */
#define KSW_HVM_NEPT_AD_RECORDS 640UL

/*
 * How many EPT12 table pages the shadow keeps a private copy of.
 *
 * These are the pages L1's own hierarchy is built out of - its PML4, its
 * PDPTs, its page directories and whatever page tables it uses - not the pages
 * it maps.  A guest of a few hundred megabytes needs a handful: one PML4, one
 * or two PDPTs, a page directory per gigabyte, plus a page table wherever L1
 * declines to use a large leaf.  Thirty-two covers that with room, and an
 * overflow is counted rather than hidden, because the consequence of
 * overflowing is losing the right to keep the shadow across an invalidation.
 *
 * Copies rather than checksums, deliberately.  The question these answer is
 * "did L1 edit its tables", and the answer decides whether L2 keeps running on
 * translations we composed earlier.  A checksum answers it with a probability;
 * a copy answers it.  Thirty-two pages is 128 KiB per processor, which is less
 * than this module already reserves for the shadow tables themselves.
 */
#define KSW_HVM_NEPT_TRACKED_PAGES 32UL

/* Preserve one processor's shadow-EPT composition state. */
typedef struct _KSW_HVM_SHADOW_EPT_STATE
{
    /* Record whether a composed hierarchy is armed for the current EPT12. */
    BOOLEAN Active;
    /* Record whether L1 supplied an EPT pointer at all. */
    BOOLEAN L1PointerValid;
    /*
     * Record that L1 asked for accessed/dirty flags.
     *
     * Kept as its own bit because when this cannot be honoured the refusal
     * reaches L1 only as a generic control-field error, which is
     * architecturally right but says nothing about which control.  Without it,
     * "L2 will not start on this hypervisor" has no readout distinguishing an
     * unsupported feature from a malformed EPT pointer.
     */
    BOOLEAN L1RequestedAccessedDirty;
    /*
     * Record that A/D is actually being maintained and propagated.
     *
     * Distinct from the request above: the request is what L1 asked for, this
     * is what we are doing about it.  They differ exactly when the processor
     * cannot maintain the bits or the record table overflowed, and that is the
     * case a reader most needs to tell apart.
     */
    BOOLEAN AccessedDirtyActive;
    /* Preserve the shadow-EPT generation. */
    ULONG Generation;
    /* Preserve the last invalidated generation. */
    ULONG InvalidationGeneration;
    /* Preserve the L1-provided EPT pointer exactly as L1 wrote it. */
    ULONGLONG L1EptPointer;
    /* Preserve KSword's own EPT pointer. */
    ULONGLONG L0EptPointer;
    /* Preserve the composed EPT pointer the processor loads for L2. */
    ULONGLONG ComposedEptPointer;
    /* Preserve the last composition status. */
    NTSTATUS LastStatus;
    /* Count leaves this hierarchy composed successfully. */
    ULONG FillCount;
    /* Count violations refused because L1's own mapping refused them. */
    ULONG DenyCount;
    /* Count violations refused because no table page remained. */
    ULONG ExhaustionCount;
    /*
     * Why the last refusal happened, in enough detail to act on.
     *
     * A refusal count alone cannot be acted on: "EPT12 maps nothing at this
     * address" and "EPT12 maps it read-only" are the same number and opposite
     * defects.  Measured need - L2 looped on a write to a guest-physical
     * address inside RAM that this code refused thirty-five thousand times,
     * and the reading said only "refused".
     *
     *   Site  1 the EPT12 walk found no mapping (Level and Entry say where)
     *         2 EPT12 maps it, but grants less than the access needs
     *         3 our own EPT01 leaf narrows it below the access
     *         4 no table page left to compose with
     *         5 an interior entry named a page we never handed out
     */
    /*
     * Whether a composed mapping still says what EPT12 says.
     *
     * This is the one failure that can end in a triple fault while leaving no
     * exit behind.  A shadow leaf naming a frame EPT12 did not means the
     * guest's own page-table walk reads another page's bytes, and every fault
     * after that - #PF, #DF, the shutdown - happens inside the guest, where
     * nothing here can see it.  Measured symptom it exists to explain: L1's
     * processor triple-faults while delivering its own local-timer vector into
     * an interruptible 64-bit guest, with no exception exit anywhere near.
     *
     * Sampled rather than checked on every fill, because the check is a second
     * EPT12 walk through the physical window and fills run into six figures per
     * boot.  One GPA is held back from each sample and verified at the next
     * one, so what is being tested is a mapping that has had time to go stale -
     * checking a leaf against the walk that just produced it would prove only
     * that the assignment worked.
     *
     * Unresolved is kept apart from mismatched: a hierarchy that no longer
     * describes the address was dropped by an invalidation, which is correct
     * behaviour and not a defect.
     */
    ULONGLONG VerifyPendingGuestPhysical;
    /*
     * The hierarchy generation when the address was held back.
     *
     * Without it the check has a false positive it cannot distinguish from the
     * defect: an invalidation between the sample and the verify drops and
     * rebuilds the hierarchy, and comparing a leaf composed from one EPT12
     * against a walk of a later one proves nothing. Generation changed means
     * skip, not mismatch.
     */
    ULONGLONG VerifyPendingGeneration;
    ULONG VerifySampleCount;
    ULONG VerifyMismatchCount;
    ULONG VerifyUnresolvedCount;
    ULONG VerifySkippedGenerationCount;
    /* The scene of a mismatch, kept because the last *sample* is usually fine. */
    ULONGLONG VerifyLastGuestPhysical;
    ULONGLONG VerifyLastShadowFrame;
    ULONGLONG VerifyLastL1Frame;
    /* And whether the leaf assignment itself landed where it was aimed. */
    ULONG LeafWriteMismatchCount;
    ULONG LastDenySite;
    ULONG LastDenyLevel;
    ULONG LastDenyAccess;
    ULONGLONG LastDenyGuestPhysical;
    ULONGLONG LastDenyEntry;
    ULONGLONG LastDenyPermissions;
    /* Retain the one nonpaged block every table page is carved from. */
    PVOID PageBlock;
    /* Retain how many pages the block holds. */
    ULONG PageTotal;
    /* Retain how many pages have been handed out. */
    ULONG PageUsed;
    /*
     * Retain each handed-out page's frame so an interior entry can be
     * navigated back to its table.
     *
     * MmGetVirtualForPhysical would answer the same question and is not
     * callable at the IRQL a VM exit runs at, so the answer is recorded when
     * it is cheap - at hand-out time - and looked up by a scan bounded to the
     * pages actually issued.
     */
    ULONGLONG PagePhysical[KSW_HVM_NEPT_TABLE_PAGES];
    /* Retain the composed hierarchy root. */
    PVOID RootVirtual;
    /* Retain the composed hierarchy root's physical address. */
    ULONGLONG RootPhysical;
    /*
     * Pair every composed leaf with the EPT12 entry it came from.
     *
     * Accessed/dirty only mean anything to L1 if they end up in L1's own
     * tables, and the processor sets them in ours.  Folding them back needs
     * the address of the EPT12 leaf, which is known during the walk and
     * nowhere afterwards - recomputing it later would mean walking EPT12 again
     * from a VM exit, for every page, every time.
     *
     * Bounded and allowed to fill up.  On overflow A/D maintenance is turned
     * off and said so, because a partial propagation is worse than none: L1
     * would read back "these pages were written and those were not" and the
     * second half would be a lie.
     */
    ULONG AdRecordCount;
    ULONGLONG AdLeafGuestPhysical[KSW_HVM_NEPT_AD_RECORDS];
    ULONGLONG AdL1EntryAddress[KSW_HVM_NEPT_AD_RECORDS];
    /* Count A/D bits actually folded back into EPT12. */
    ULONG AdPropagatedCount;
    /* Count records dropped because the table was full. */
    ULONG AdOverflowCount;
    /*
     * Every EPT12 table page this hierarchy was composed out of, with a copy.
     *
     * The shadow is a cache of L1's tables, so the only thing that can make it
     * wrong is L1 editing them.  INVEPT is L1 saying "translations for this
     * context may be stale" - which it issues as routine hygiene, not only
     * after an edit.  Measured: VMware issues one per world switch, 319 times
     * a second, and dropping the whole hierarchy each time left its guest able
     * to fault in about 125 pages before losing them all again.  A BIOS
     * loading a kernel needs thousands, so it never finished.
     *
     * With these, an invalidation compares each table page against its copy.
     * Unchanged means the cache is still exactly what L1's tables say, and only
     * the processor's own translation caches need flushing.  Changed - or
     * overflowed, or never snapshotted - means the drop still happens.
     */
    ULONG TrackedCount;
    ULONG TrackedOverflowCount;
    ULONGLONG TrackedFrame[KSW_HVM_NEPT_TRACKED_PAGES];
    /* One page of private copy per tracked frame, in walk order. */
    PVOID TrackedCopyBlock;
    /* Count invalidations that kept the hierarchy, and that dropped it. */
    ULONG InvalidateKeptCount;
    ULONG InvalidateDroppedCount;
    /* Count invalidations that named a context that was not ours. */
    ULONG InvalidateForeignCount;
} KSW_HVM_SHADOW_EPT_STATE;

EXTERN_C_START

/* Initialize explicit inactive shadow-EPT state. */
VOID
KswordARKHvmNestedEptInitialize(
    _Out_ KSW_HVM_SHADOW_EPT_STATE* Shadow,
    _In_ ULONGLONG L0EptPointer
    );

/* Reserve the table pages one processor's shadow needs.  PASSIVE_LEVEL. */
NTSTATUS
KswordARKHvmNestedEptPrepare(
    _Inout_ KSW_HVM_SHADOW_EPT_STATE* Shadow
    );

/*
 * Fold the accessed/dirty bits the processor set in our leaves into EPT12.
 *
 * VM-exit safe: reads and writes L1's tables through the per-processor window
 * and allocates nothing.  Call while the shadow is still coherent - before
 * invalidating it, and before returning control to L1 - because the records it
 * walks name leaves that invalidation destroys.
 *
 * Does nothing when L1 did not ask for A/D, when the processor cannot maintain
 * it, or when the record table overflowed.  That last one is deliberate: a
 * partial fold would have L1 read back "these pages were written and those
 * were not" with the second half untrue and undetectable.
 *
 * Returns how many entries were updated, which is the only evidence that any
 * of this happened - L1 cannot tell a propagated bit from one it set itself.
 */
ULONG
KswordARKHvmNestedEptPropagateAccessedDirty(
    _Inout_ KSW_HVM_SHADOW_EPT_STATE* Shadow,
    _Inout_ KSW_HVM_PHYS_WINDOW* Window
    );

/* Release the reserved block.  Legal at DISPATCH_LEVEL. */
VOID
KswordARKHvmNestedEptRelease(
    _Inout_ KSW_HVM_SHADOW_EPT_STATE* Shadow
    );

/*
 * Record the EPT pointer L1 wrote into vmcs12 and arm composition.
 *
 * Changing the pointer drops every composed mapping: they described a
 * different EPT12 entirely, and keeping them would let L2 run on translations
 * the incoming hierarchy never authorized.
 */
NTSTATUS
KswordARKHvmNestedEptSetL1Pointer(
    _Inout_ KSW_HVM_SHADOW_EPT_STATE* Shadow,
    _In_ ULONGLONG L1EptPointer
    );

/* Drop every composed mapping for one L1 invalidation request. */
VOID
KswordARKHvmNestedEptInvalidate(
    _Inout_ KSW_HVM_SHADOW_EPT_STATE* Shadow
    );

/*
 * Serve one INVEPT from L1 without destroying a hierarchy that is still right.
 *
 * VM-exit safe.  Compares every EPT12 table page the hierarchy was composed
 * out of against the copy taken when it was read.  All equal means L1 has not
 * edited its tables since, so the composed mappings still say exactly what
 * EPT12 says and only the processor's translation caches need flushing.  Any
 * difference - or a hierarchy composed before tracking could keep up - falls
 * back to dropping everything, which is what this used to do unconditionally.
 *
 * Returns TRUE when the hierarchy was kept.
 */
BOOLEAN
KswordARKHvmNestedEptInvalidateChecked(
    _Inout_ KSW_HVM_SHADOW_EPT_STATE* Shadow,
    _Inout_ KSW_HVM_PHYS_WINDOW* Window
    );

/*
 * Compose one leaf for an L2 guest physical address.  VM-exit safe.
 *
 * Returns TRUE when a mapping now exists and the faulting instruction may be
 * retried; FALSE when the violation belongs to L1 - either because EPT12 does
 * not permit the access or because no table page remained.
 */
BOOLEAN
KswordARKHvmNestedEptFill(
    _Inout_ struct _KSW_HVM_RUNTIME* Runtime,
    _Inout_ KSW_HVM_SHADOW_EPT_STATE* Shadow,
    _Inout_ KSW_HVM_PHYS_WINDOW* Window,
    _In_ ULONGLONG GuestPhysicalAddress,
    _In_ ULONG Access
    );

EXTERN_C_END
