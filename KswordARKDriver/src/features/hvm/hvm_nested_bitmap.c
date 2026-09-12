/*++

Module Name:

    hvm_nested_bitmap.c

Abstract:

    Implements the MSR and I/O bitmap merge for L2, and the ownership tests the
    exit path uses to decide whether L1 armed a given MSR or port exit.

Environment:

    Kernel-mode Driver Framework.

--*/

#include "hvm_nested_bitmap.h"
#include "hvm_nested_vmcs.h"
#include "hvm_phys_window.h"
#include "hvm_resident.h"

#if defined(_M_AMD64)

/* Name the vmcs12 fields holding L1's three bitmap addresses. */
#define KSW_NB_VMCS12_IO_BITMAP_A 0x2000UL
#define KSW_NB_VMCS12_IO_BITMAP_B 0x2002UL
#define KSW_NB_VMCS12_MSR_BITMAP 0x2004UL

/* One bitmap page, in bytes and in qwords. */
#define KSW_NB_PAGE_BYTES 4096UL
#define KSW_NB_PAGE_QWORDS (KSW_NB_PAGE_BYTES / 8UL)

/*
 * Describe the MSR bitmap's four regions.
 *
 * SDM 25.6.9: one page, four 1 KiB regions in this order - read-low,
 * read-high, write-low, write-high.  "Low" covers 0x00000000-0x00001FFF and
 * "high" covers 0xC0000000-0xC0001FFF; an MSR outside both ranges is not
 * described by the bitmap at all and always exits.
 */
#define KSW_NB_MSR_REGION_BYTES 1024UL
#define KSW_NB_MSR_LOW_BASE 0x00000000UL
#define KSW_NB_MSR_LOW_LIMIT 0x00001FFFUL
#define KSW_NB_MSR_HIGH_BASE 0xC0000000UL
#define KSW_NB_MSR_HIGH_LIMIT 0xC0001FFFUL

/*
 * Copy one guest-physical page into a local buffer through the exit window.
 *
 * Returns FALSE when the window is unavailable or refuses the address, which
 * every caller treats as "keep intercepting everything" rather than as a
 * reason to let L2 run unmediated.
 */
static BOOLEAN
KswordARKHvmNestedBitmapReadPage(
    _Inout_ KSW_HVM_PHYS_WINDOW* Window,
    _In_ ULONGLONG GuestPhysical,
    _Out_writes_bytes_(KSW_NB_PAGE_BYTES) VOID* Destination
    )
{
    volatile VOID* mapped = NULL;
    volatile ULONGLONG* source = NULL;
    ULONGLONG* target = (ULONGLONG*)Destination;
    ULONG index = 0UL;

    /* Refuse without a window: there is no other exit-safe way to read this. */
    if (Window == NULL) {
        /* Report the unread page. */
        return FALSE;
    }
    /*
     * A bitmap must be page-aligned by architecture.  Checking here rather
     * than relying on the window's own page-crossing refusal keeps the reason
     * specific: a misaligned address is L1 handing us an illegal VMCS, not a
     * window that could not satisfy a legal request.
     */
    if ((GuestPhysical & (KSW_NB_PAGE_BYTES - 1ULL)) != 0ULL) {
        /* Report the unread page. */
        return FALSE;
    }
    if (KswordARKHvmPhysWindowMap(
            Window,
            GuestPhysical,
            KSW_NB_PAGE_BYTES,
            &mapped) != KSW_HVM_PHYS_WINDOW_OK) {
        /* Report the unread page. */
        return FALSE;
    }
    source = (volatile ULONGLONG*)mapped;
    for (index = 0UL; index < KSW_NB_PAGE_QWORDS; ++index) {
        target[index] = source[index];
    }
    KswordARKHvmPhysWindowUnmap(Window);
    /* Report the page as read. */
    return TRUE;
}

/*
 * Resolve one MSR to its bit position in the bitmap page.
 *
 * Returns FALSE for an MSR the bitmap does not describe, which architecturally
 * always exits.
 */
static BOOLEAN
KswordARKHvmNestedBitmapMsrBit(
    _In_ ULONG MsrIndex,
    _In_ BOOLEAN IsWrite,
    _Out_ ULONG* ByteOffset,
    _Out_ ULONG* BitMask
    )
{
    ULONG regionBase = 0UL;
    ULONG withinRange = 0UL;

    *ByteOffset = 0UL;
    *BitMask = 0UL;
    if (MsrIndex <= KSW_NB_MSR_LOW_LIMIT) {
        /* Read-low is region 0 and write-low is region 2. */
        regionBase = IsWrite
            ? (2UL * KSW_NB_MSR_REGION_BYTES)
            : (0UL * KSW_NB_MSR_REGION_BYTES);
        withinRange = MsrIndex - KSW_NB_MSR_LOW_BASE;
    } else if (MsrIndex >= KSW_NB_MSR_HIGH_BASE &&
               MsrIndex <= KSW_NB_MSR_HIGH_LIMIT) {
        /* Read-high is region 1 and write-high is region 3. */
        regionBase = IsWrite
            ? (3UL * KSW_NB_MSR_REGION_BYTES)
            : (1UL * KSW_NB_MSR_REGION_BYTES);
        withinRange = MsrIndex - KSW_NB_MSR_HIGH_BASE;
    } else {
        /* Report an MSR the bitmap does not describe. */
        return FALSE;
    }
    *ByteOffset = regionBase + (withinRange / 8UL);
    *BitMask = 1UL << (withinRange % 8UL);
    /* Report the resolved position. */
    return TRUE;
}

VOID
KswordARKHvmNestedBitmapMerge(
    _Inout_ struct _KSW_HVM_RESIDENT_VCPU* Context,
    _In_ ULONG Vmcs12Primary,
    _Out_ KSW_HVM_NESTED_BITMAP_MERGE* Result
    )
{
    KSW_HVM_NESTED_VCPU* nested = &Context->Nested;
    KSW_HVM_VMCS12_STATE* vmcs12 = &nested->Vmcs12;
    UCHAR* merged = NULL;
    UCHAR* l1Copy = NULL;
    const UCHAR* ours = NULL;
    ULONGLONG address = 0ULL;
    ULONG index = 0UL;

    RtlZeroMemory(Result, sizeof(*Result));
    Result->L1UsesMsrBitmap =
        ((Vmcs12Primary & KSW_HVM_NESTED_PRIMARY_USE_MSR_BITMAPS) != 0UL);
    Result->L1UsesIoBitmap =
        ((Vmcs12Primary & KSW_HVM_NESTED_PRIMARY_USE_IO_BITMAPS) != 0UL);
    Result->L1UncondIo =
        ((Vmcs12Primary & KSW_HVM_NESTED_PRIMARY_UNCOND_IO_EXITING) != 0UL);
    /* Publish L1's own intent for the exit path before anything can fail. */
    nested->L2MsrFilterFromL1 = Result->L1UsesMsrBitmap;
    nested->L2IoFilterFromL1 = Result->L1UsesIoBitmap;
    nested->L2UncondIoFromL1 = Result->L1UncondIo;
    nested->L2BitmapMergeComplete = FALSE;
    /* Without the pages there is nothing to hand vmcs02. */
    if (Context->Resource == NULL ||
        Context->Resource->L2MsrBitmapVirtual == NULL ||
        Context->Resource->L2IoBitmapAVirtual == NULL ||
        Context->Resource->L2IoBitmapBVirtual == NULL ||
        Context->Resource->L2MsrBitmapL1Copy == NULL) {
        /* Leave every address zero; the caller refuses the entry. */
        return;
    }
    Result->MsrBitmapPhysical =
        (ULONGLONG)Context->Resource->L2MsrBitmapPhysical.QuadPart;
    Result->IoBitmapAPhysical =
        (ULONGLONG)Context->Resource->L2IoBitmapAPhysical.QuadPart;
    Result->IoBitmapBPhysical =
        (ULONGLONG)Context->Resource->L2IoBitmapBPhysical.QuadPart;
    merged = (UCHAR*)Context->Resource->L2MsrBitmapVirtual;
    l1Copy = (UCHAR*)Context->Resource->L2MsrBitmapL1Copy;
    Result->Complete = TRUE;

    /*
     * MSR side.
     *
     * L1 without a bitmap means every L2 MSR access is architecturally L1's.
     * All-ones reproduces that exactly once reflection is in place, and it is
     * also the state both pages must fall back to when a read fails - so the
     * two cases share one branch rather than being spelled out twice.
     */
    if (!Result->L1UsesMsrBitmap) {
        RtlFillMemory(merged, KSW_NB_PAGE_BYTES, 0xFF);
        RtlFillMemory(l1Copy, KSW_NB_PAGE_BYTES, 0xFF);
    } else {
        address = 0ULL;
        (void)KswordARKHvmNestedVmcs12Read(
            vmcs12,
            KSW_NB_VMCS12_MSR_BITMAP,
            &address);
        /*
         * With nothing of our own to add, point vmcs02 at L1's page directly.
         *
         * The merge exists to union two bitmaps.  When our side is empty the
         * union *is* L1's page, and copying it produces a byte-identical
         * duplicate at the cost of a 4 KiB window read plus a 4 KiB OR on
         * every single L2 entry.  Measured at 21-27% of entry cost.
         *
         * The test is the bitmap's own bit count, not a proxy for it.  It
         * asked "are there zero MSR policies" while the only thing that ever
         * set a bit was a policy; once the VMX capability interception began
         * setting bits, that proxy would have shared L1's page and silently
         * dropped our interception for L2 - which is to say, it would have let
         * L2 read the machine's real VMX capabilities out from under the
         * filter that exists precisely to stop that.
         *
         * With capability interception armed our half is never empty, so this
         * path no longer runs during normal residency.  It is kept rather than
         * deleted because emptiness is a property of the bitmap, not a
         * permanent fact: the interception is armed at preparation and a build
         * that does not arm it - or a future narrowing of what we intercept -
         * gets the fast path back without anyone having to rediscover it.
         *
         * Sharing is also strictly more correct than copying here.  A copy is
         * a snapshot: L1 editing its bitmap in place between entries - which
         * is exactly what a hypervisor does as guest state changes - leaves us
         * running L2 against a stale one until the next merge. Pointing at the
         * same page means the processor sees the edit immediately.
         *
         * Valid because EPT01 is an identity map, so an L1 physical address is
         * a host physical address.  The walk in hvm_nested_ept.c already
         * depends on exactly this; if that ever stops holding, both break
         * together rather than one silently.
         *
         * Alignment is checked because the field feeds hardware directly here,
         * not a reader that would have refused a bad address on our behalf.
         */
        if (Context->Runtime->MsrBitmapInterceptCount == 0UL &&
            address != 0ULL &&
            (address & (KSW_NB_PAGE_BYTES - 1ULL)) == 0ULL) {
            Result->MsrBitmapPhysical = address;
            Result->SharedMsrBitmap = TRUE;
            nested->L2MsrBitmapShared = TRUE;
            nested->L2MsrBitmapL1Gpa = address;
            /*
             * Routing still needs to know what L1 wanted, and the shared page
             * is L1's own - so the exit path reads the one byte it needs
             * through the window instead of consulting a copy that no longer
             * exists.  Eight bytes per MSR exit against four kilobytes per
             * entry.
             */
            goto io_side;
        }
        nested->L2MsrBitmapShared = FALSE;
        if (!KswordARKHvmNestedBitmapReadPage(
                Context->PhysWindow,
                address,
                l1Copy)) {
            /*
             * Could not read L1's bitmap.  Intercept everything and say so:
             * the alternative is a page of zeroes, which would hand L2 direct
             * access to every MSR on the machine.
             */
            RtlFillMemory(merged, KSW_NB_PAGE_BYTES, 0xFF);
            RtlFillMemory(l1Copy, KSW_NB_PAGE_BYTES, 0xFF);
            Result->Complete = FALSE;
        } else {
            /*
             * Union with ours.  An MSR exits if either hypervisor wants it;
             * dropping our half would cost us interception we depend on, and
             * dropping L1's would lose its guest's events silently.
             */
            ours = (const UCHAR*)Context->Runtime->MsrBitmapVirtual;
            if (ours == NULL) {
                RtlCopyMemory(merged, l1Copy, KSW_NB_PAGE_BYTES);
            } else {
                for (index = 0UL; index < KSW_NB_PAGE_BYTES; ++index) {
                    merged[index] = (UCHAR)(l1Copy[index] | ours[index]);
                }
            }
        }
    }

io_side:
    /*
     * I/O side.
     *
     * We request no I/O exiting, so there is nothing to union - the pages are
     * L1's, copied because vmcs02 needs host-physical addresses and L1's are
     * guest-physical.  With unconditional I/O exiting the processor ignores
     * the bitmaps entirely, so that case skips the copy; with neither control
     * set no I/O exit happens at all and the pages are never consulted.
     */
    if (Result->L1UsesIoBitmap && !Result->L1UncondIo) {
        ULONGLONG addressA = 0ULL;
        ULONGLONG addressB = 0ULL;

        (void)KswordARKHvmNestedVmcs12Read(
            vmcs12,
            KSW_NB_VMCS12_IO_BITMAP_A,
            &addressA);
        (void)KswordARKHvmNestedVmcs12Read(
            vmcs12,
            KSW_NB_VMCS12_IO_BITMAP_B,
            &addressB);
        /*
         * We contribute nothing to I/O interception, so these are always
         * shareable - there is no union to compute, only L1's own decision.
         *
         * Copying them was two more 4 KiB window reads per entry producing two
         * byte-identical duplicates, and a snapshot besides: an L1 that edits
         * its I/O bitmap between entries would have run L2 against the old one.
         */
        if (addressA != 0ULL && addressB != 0ULL &&
            (addressA & (KSW_NB_PAGE_BYTES - 1ULL)) == 0ULL &&
            (addressB & (KSW_NB_PAGE_BYTES - 1ULL)) == 0ULL) {
            Result->IoBitmapAPhysical = addressA;
            Result->IoBitmapBPhysical = addressB;
            Result->SharedIoBitmaps = TRUE;
            nested->L2IoBitmapsShared = TRUE;
            nested->L2IoBitmapAL1Gpa = addressA;
            nested->L2IoBitmapBL1Gpa = addressB;
            nested->L2BitmapMergeComplete = Result->Complete;
            /* Return with both sides settled. */
            return;
        }
        nested->L2IoBitmapsShared = FALSE;
        if (!KswordARKHvmNestedBitmapReadPage(
                Context->PhysWindow,
                addressA,
                Context->Resource->L2IoBitmapAVirtual)) {
            RtlFillMemory(
                Context->Resource->L2IoBitmapAVirtual,
                KSW_NB_PAGE_BYTES,
                0xFF);
            Result->Complete = FALSE;
        }
        if (!KswordARKHvmNestedBitmapReadPage(
                Context->PhysWindow,
                addressB,
                Context->Resource->L2IoBitmapBVirtual)) {
            RtlFillMemory(
                Context->Resource->L2IoBitmapBVirtual,
                KSW_NB_PAGE_BYTES,
                0xFF);
            Result->Complete = FALSE;
        }
    } else {
        nested->L2IoBitmapsShared = FALSE;
        /*
         * Leave both pages intercepting everything.
         *
         * They are not consulted in either remaining case, but a stale page
         * from a previous L2 with a different owner is exactly the kind of
         * thing that becomes load-bearing after an unrelated change.
         */
        RtlFillMemory(
            Context->Resource->L2IoBitmapAVirtual,
            KSW_NB_PAGE_BYTES,
            0xFF);
        RtlFillMemory(
            Context->Resource->L2IoBitmapBVirtual,
            KSW_NB_PAGE_BYTES,
            0xFF);
    }
    nested->L2BitmapMergeComplete = Result->Complete;
}

BOOLEAN
KswordARKHvmNestedBitmapL1WantsMsr(
    _In_ struct _KSW_HVM_RESIDENT_VCPU* Context,
    _In_ ULONG MsrIndex,
    _In_ BOOLEAN IsWrite
    )
{
    const KSW_HVM_NESTED_VCPU* nested = &Context->Nested;
    const UCHAR* l1Copy = NULL;
    ULONG byteOffset = 0UL;
    ULONG bitMask = 0UL;

    /* Without a bitmap of its own, every MSR access from L2 is L1's. */
    if (!nested->L2MsrFilterFromL1) {
        /* Report the access as L1's. */
        return TRUE;
    }
    /*
     * An incomplete merge means we do not know what L1 wanted.  Reflecting is
     * the direction that costs L1 a spurious exit rather than hiding one it
     * armed.
     */
    if (!nested->L2BitmapMergeComplete) {
        /* Report the access as L1's. */
        return TRUE;
    }
    if (Context->Resource == NULL ||
        Context->Resource->L2MsrBitmapL1Copy == NULL) {
        /* Report the access as L1's. */
        return TRUE;
    }
    /* An MSR outside both ranges always exits, and always to L1. */
    if (!KswordARKHvmNestedBitmapMsrBit(
            MsrIndex,
            IsWrite,
            &byteOffset,
            &bitMask)) {
        /* Report the access as L1's. */
        return TRUE;
    }
    /*
     * A shared page has no local copy: read the byte out of L1's own page.
     *
     * Eight bytes through the window per MSR exit, against four kilobytes per
     * L2 entry for the copy this replaces.  It also cannot go stale, which a
     * snapshot can the moment L1 edits its bitmap.
     */
    if (nested->L2MsrBitmapShared) {
        ULONGLONG chunk = 0ULL;
        const ULONGLONG qwordAddress =
            nested->L2MsrBitmapL1Gpa + (byteOffset & ~7ULL);

        if (Context->PhysWindow == NULL ||
            !NT_SUCCESS(KswordARKHvmPhysWindowReadQword(
                (KSW_HVM_PHYS_WINDOW*)Context->PhysWindow,
                qwordAddress,
                &chunk))) {
            /* Report the access as L1's: unknown resolves toward reflection. */
            return TRUE;
        }
        chunk >>= ((byteOffset & 7ULL) * 8ULL);
        /* Report exactly what L1's own bitmap says. */
        return (((ULONG)(chunk & 0xFFULL) & bitMask) != 0UL) ? TRUE : FALSE;
    }
    l1Copy = (const UCHAR*)Context->Resource->L2MsrBitmapL1Copy;
    /* Report exactly what L1's own bitmap says. */
    return ((l1Copy[byteOffset] & (UCHAR)bitMask) != 0U) ? TRUE : FALSE;
}

BOOLEAN
KswordARKHvmNestedBitmapL1WantsPort(
    _In_ struct _KSW_HVM_RESIDENT_VCPU* Context,
    _In_ ULONG Port,
    _In_ ULONG AccessBytes
    )
{
    const KSW_HVM_NESTED_VCPU* nested = &Context->Nested;
    const UCHAR* bitmap = NULL;
    ULONG offset = 0UL;
    ULONG current = 0UL;
    ULONG span = (AccessBytes == 0UL) ? 1UL : AccessBytes;

    /* Unconditional I/O exiting makes every port access L1's. */
    if (nested->L2UncondIoFromL1) {
        /* Report the access as L1's. */
        return TRUE;
    }
    /* Without either control L1 armed nothing, so the exit is not its. */
    if (!nested->L2IoFilterFromL1) {
        /* Report the access as ours. */
        return FALSE;
    }
    if (!nested->L2BitmapMergeComplete ||
        Context->Resource == NULL ||
        Context->Resource->L2IoBitmapAVirtual == NULL ||
        Context->Resource->L2IoBitmapBVirtual == NULL) {
        /* Report the access as L1's: unknown resolves toward reflection. */
        return TRUE;
    }
    /*
     * The processor exits when any port in the access has its bit set, so the
     * test walks every byte of the access rather than only its first port.
     */
    for (current = 0UL; current < span; ++current) {
        const ULONG port = (Port + current) & 0xFFFFUL;

        offset = (port < 0x8000UL) ? port : (port - 0x8000UL);
        /* A shared page has no local copy: read L1's own byte. */
        if (nested->L2IoBitmapsShared) {
            const ULONGLONG pageGpa = (port < 0x8000UL)
                ? nested->L2IoBitmapAL1Gpa
                : nested->L2IoBitmapBL1Gpa;
            ULONGLONG chunk = 0ULL;

            if (Context->PhysWindow == NULL ||
                !NT_SUCCESS(KswordARKHvmPhysWindowReadQword(
                    (KSW_HVM_PHYS_WINDOW*)Context->PhysWindow,
                    pageGpa + ((ULONGLONG)(offset / 8UL) & ~7ULL),
                    &chunk))) {
                /* Report as L1's: unknown resolves toward reflection. */
                return TRUE;
            }
            chunk >>= (((ULONGLONG)(offset / 8UL) & 7ULL) * 8ULL);
            if ((((ULONG)(chunk & 0xFFULL)) &
                    (1UL << (offset % 8UL))) != 0UL) {
                /* Report the access as L1's. */
                return TRUE;
            }
            continue;
        }
        bitmap = (port < 0x8000UL)
            ? (const UCHAR*)Context->Resource->L2IoBitmapAVirtual
            : (const UCHAR*)Context->Resource->L2IoBitmapBVirtual;
        if ((bitmap[offset / 8UL] & (UCHAR)(1UL << (offset % 8UL))) != 0U) {
            /* Report the access as L1's. */
            return TRUE;
        }
    }
    /* Report the access as ours. */
    return FALSE;
}

#else

VOID
KswordARKHvmNestedBitmapMerge(
    _Inout_ struct _KSW_HVM_RESIDENT_VCPU* Context,
    _In_ ULONG Vmcs12Primary,
    _Out_ KSW_HVM_NESTED_BITMAP_MERGE* Result
    )
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Vmcs12Primary);
    RtlZeroMemory(Result, sizeof(*Result));
}

BOOLEAN
KswordARKHvmNestedBitmapL1WantsMsr(
    _In_ struct _KSW_HVM_RESIDENT_VCPU* Context,
    _In_ ULONG MsrIndex,
    _In_ BOOLEAN IsWrite
    )
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(MsrIndex);
    UNREFERENCED_PARAMETER(IsWrite);
    /* Report the access as L1's on an architecture without nested support. */
    return TRUE;
}

BOOLEAN
KswordARKHvmNestedBitmapL1WantsPort(
    _In_ struct _KSW_HVM_RESIDENT_VCPU* Context,
    _In_ ULONG Port,
    _In_ ULONG AccessBytes
    )
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Port);
    UNREFERENCED_PARAMETER(AccessBytes);
    /* Report the access as L1's on an architecture without nested support. */
    return TRUE;
}

#endif
