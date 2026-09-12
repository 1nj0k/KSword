/*++

Module Name:

    hvm_nested_bitmap.h

Abstract:

    Builds the MSR and I/O bitmaps vmcs02 runs L2 under, and answers which of
    the two hypervisors asked for a given MSR or port exit.

Environment:

    Kernel-mode Driver Framework.

--*/

#pragma once

#include "hvm_internal.h"
#include "hvm_nested.h"

/* Forward-declare the per-processor resident context. */
struct _KSW_HVM_RESIDENT_VCPU;

/* Name the primary controls this module reads out of vmcs12. */
#define KSW_HVM_NESTED_PRIMARY_UNCOND_IO_EXITING (1UL << 24)
#define KSW_HVM_NESTED_PRIMARY_USE_IO_BITMAPS (1UL << 25)
#define KSW_HVM_NESTED_PRIMARY_USE_MSR_BITMAPS (1UL << 28)

/*
 * Describe what one merge produced.
 *
 * Returned rather than written straight into vmcs02 so the caller keeps the
 * single place that touches VMCS fields, and so a merge that could not read
 * L1's pages is distinguishable from one that read them and found nothing set.
 */
typedef struct _KSW_HVM_NESTED_BITMAP_MERGE
{
    /* Publish whether every page the merge needed was actually read. */
    BOOLEAN Complete;
    /* Publish whether L1 itself asked for MSR-bitmap filtering. */
    BOOLEAN L1UsesMsrBitmap;
    /* Publish whether L1 itself asked for I/O-bitmap filtering. */
    BOOLEAN L1UsesIoBitmap;
    /* Publish whether L1 asked for unconditional I/O exiting. */
    BOOLEAN L1UncondIo;
    /* Hand back the physical address vmcs02's MSR-bitmap field must carry. */
    ULONGLONG MsrBitmapPhysical;
    /* Hand back the two I/O-bitmap physical addresses for vmcs02. */
    ULONGLONG IoBitmapAPhysical;
    ULONGLONG IoBitmapBPhysical;
} KSW_HVM_NESTED_BITMAP_MERGE;

EXTERN_C_START

/*
 * Build this processor's L2 bitmaps from vmcs12 and our own.
 *
 * VM-exit safe: reads L1's pages through the per-processor physical window and
 * allocates nothing.  Call once per L2 entry, before vmcs02 is written.
 *
 * An MSR exits if either hypervisor wants it, so the MSR page is a union.  The
 * I/O pages are plain copies of L1's, because we request no I/O exiting and
 * therefore have nothing to add.
 *
 * When L1 did not ask for MSR filtering at all, the merged page is left at
 * all-ones: architecturally every L2 MSR access then belongs to L1, and
 * all-ones plus reflection reproduces exactly that.  The same conservative
 * direction covers a page we could not read - `Complete` goes false and the
 * bitmap keeps intercepting everything rather than silently letting L2 through.
 */
VOID
KswordARKHvmNestedBitmapMerge(
    _Inout_ struct _KSW_HVM_RESIDENT_VCPU* Context,
    _In_ ULONG Vmcs12Primary,
    _Out_ KSW_HVM_NESTED_BITMAP_MERGE* Result
    );

/*
 * Answer whether L1 armed an exit for this MSR access.
 *
 * Consults the unmerged copy of L1's bitmap taken at the last merge, because
 * the merged page cannot distinguish "L1 wanted it" from "we wanted it".
 * MSRs outside the two architectural ranges always exit and are always L1's.
 */
BOOLEAN
KswordARKHvmNestedBitmapL1WantsMsr(
    _In_ struct _KSW_HVM_RESIDENT_VCPU* Context,
    _In_ ULONG MsrIndex,
    _In_ BOOLEAN IsWrite
    );

/*
 * Answer whether L1 armed an exit for this port access.
 *
 * An access spanning several ports exits when any one of its port bits is set,
 * which is what the processor itself does.
 */
BOOLEAN
KswordARKHvmNestedBitmapL1WantsPort(
    _In_ struct _KSW_HVM_RESIDENT_VCPU* Context,
    _In_ ULONG Port,
    _In_ ULONG AccessBytes
    );

EXTERN_C_END
