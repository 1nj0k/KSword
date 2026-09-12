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
