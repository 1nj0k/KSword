/* Immutable AMD identity NPT; no allocation or page-table edits in VMEXIT. */
#include "hvm_svm.h"
#include "../../platform/pool_compat.h"

/* Cache indices must refer to the existing host PAT; never rewrite it. */
static ULONG KswNptPatIndex(ULONGLONG Pat, ULONG Type)
{
    /* Scan eight architectural PAT slots. */
    ULONG index;
    /* Return an explicit invalid sentinel when no slot matches. */
    for (index = 0; index < 8; ++index) {
        /* Ignore reserved high bits by requiring the complete byte. */
        if (((Pat >> (index * 8)) & 0xffULL) == Type) { return index; }
    }
    /* Absence cannot be treated as WB. */
    return 8;
}

/* Classify an entire leaf: WB RAM, UC holes/MMIO, or mixed requiring split. */
static LONG KswNptCacheRange(const KSW_NPT* Npt, ULONGLONG Start, ULONGLONG Bytes)
{
    /* Ranges are page-aligned and validated by the builder. */
    ULONG index;
    /* End cannot overflow because coverage is bounded to 48 bits. */
    ULONGLONG end = Start + Bytes;
    /* An overlap which does not cover the whole leaf requires finer pages. */
    for (index = 0; Npt->Ranges[index].NumberOfBytes.QuadPart; ++index) {
        /* Decode the validated physical RAM interval. */
        ULONGLONG low = (ULONGLONG)Npt->Ranges[index].BaseAddress.QuadPart;
        /* Decode the validated exclusive end. */
        ULONGLONG high = low + (ULONGLONG)Npt->Ranges[index].NumberOfBytes.QuadPart;
        /* An entire RAM interval can use WB; hardware MTRRs still participate. */
        if (Start >= low && end <= high) { return (LONG)Npt->WbIndex; }
        /* Split any interval touching only part of RAM. */
        if (Start < high && end > low) { return -1; }
    }
    /* Unknown physical holes, including MMIO, use UC rather than guessed WB. */
    return (LONG)Npt->UcIndex;
}

/* Allocate a page and record ownership before returning its physical address. */
static PULONGLONG KswNptAllocate(KSW_NPT* Npt, ULONGLONG* Pa)
{
    /* Constrain hardware page allocations to the supported address mask. */
    PHYSICAL_ADDRESS highest;
    /* Keep the local ownership pointer. */
    PVOID page;
    /* Enforce the explicit 64-MiB table budget. */
    if (Npt->PageCount == KSW_NPT_MAX_PAGES) { return NULL; }
    /* Limit allocations to representable physical addresses. */
    highest.QuadPart = (LONGLONG)(Npt->Limit - 1);
    /* A single hardware page is physically contiguous by construction. */
    page = MmAllocateContiguousMemory(PAGE_SIZE, highest);
    /* Return without publishing a parent entry on failure. */
    if (page == NULL) { return NULL; }
    /* Initialize reserved fields and unused entries. */
    RtlZeroMemory(page, PAGE_SIZE);
    /* Record the allocation before the caller can publish it. */
    Npt->Pages[Npt->PageCount++] = page;
    /* Resolve its hardware address once in preparation. */
    *Pa = (ULONGLONG)MmGetPhysicalAddress(page).QuadPart;
    /* Return the writeable kernel mapping. */
    return (PULONGLONG)page;
}

/* Build one table recursively; recursion depth never exceeds four. */
static NTSTATUS KswNptFill(KSW_NPT* Npt, PULONGLONG Table, ULONG Level, ULONGLONG Base, PULONG Required)
{
    /* Each level covers 512 entries of a power-of-two span. */
    ULONGLONG span = 1ULL << (12 + 9 * (Level - 1));
    /* Loop index does not depend on untrusted guest state. */
    ULONG index;
    /* Dry traversal computes the exact split-table budget before allocating hardware tables. */
    if (Required != NULL && ++*Required > KSW_NPT_MAX_PAGES) { return STATUS_INSUFFICIENT_RESOURCES; }
    /* Fill only the range advertised by the guest physical-address width. */
    for (index = 0; index < 512 && Base + index * span < Npt->Limit; ++index) {
        /* Address translation is an identity mapping. */
        ULONGLONG address = Base + index * span;
        /* PAT indices cannot be chosen until the entire range is classified. */
        LONG cache = KswNptCacheRange(Npt, address, span);
        /* Large pages require a homogeneous range and enumerated page size. */
        if (Level == 1 || (cache >= 0 && (Level == 2 || (Level == 3 && Npt->Page1Gb)))) {
            /* A mixed 4-KiB leaf would imply a malformed RAM inventory. */
            ULONGLONG flags;
            /* Refuse an impossible subpage cache conflict. */
            if (cache < 0) { return STATUS_DATA_ERROR; }
            /* Cache/permission encoding is shared with exhaustive host-side tests. */
            flags = KswNptLeafFlags(Level, (ULONG)cache);
            /* Leave NX clear for this baseline identity map. */
            if (Table != NULL) { Table[index] = (address & Npt->AddressMask) | flags; }
        } else {
            /* Intermediate entries never contain leaf cache attributes. */
            ULONGLONG pa = 0;
            /* Allocate the next-level table. */
            PULONGLONG child = Required != NULL ? NULL : KswNptAllocate(Npt, &pa);
            /* Preserve the complete allocation ledger on any failure. */
            NTSTATUS status;
            /* Stop before publishing an absent table. */
            if (child == NULL && Required == NULL) { return STATUS_INSUFFICIENT_RESOURCES; }
            /* Fully populate children before linking their parent. */
            status = KswNptFill(Npt, child, Level - 1, address, Required);
            /* Propagate failure without creating a partially valid root. */
            if (!NT_SUCCESS(status)) { return status; }
            /* Publish the completed lower level. */
            if (Table != NULL) { Table[index] = pa | 7ULL; }
        }
    }
    /* Every advertised address now has an identity translation. */
    return STATUS_SUCCESS;
}

/* Called only after all SVM CPUs have stopped, or before any CPU entered. */
VOID KswordNptRelease(KSW_NPT* Npt)
{
    /* Free reverse allocation order without walking untrusted table entries. */
    while (Npt->PageCount != 0) { MmFreeContiguousMemory(Npt->Pages[--Npt->PageCount]); }
    /* RAM inventory is preparation-only. */
    if (Npt->Ranges != NULL) { ExFreePool(Npt->Ranges); }
    /* The bookkeeping array is ordinary NX pool. */
    if (Npt->Pages != NULL) { ExFreePoolWithTag(Npt->Pages, 'nSvK'); }
    /* Clear all address and ownership evidence. */
    RtlZeroMemory(Npt, sizeof(*Npt));
}

/* Construct the entire immutable NPT before any SVM entry. */
NTSTATUS KswordNptBuild(KSW_NPT* Npt, const KSW_SVM_CAPS* Caps)
{
    /* Root mapping and status stay private until construction completes. */
    PULONGLONG root;
    /* Retain the exact failure from allocation or inventory validation. */
    NTSTATUS status;
    /* Bound resource estimation independently of installed RAM. */
    ULONGLONG minimumPages;
    /* Exact dry-walk table count including RAM-boundary splits. */
    ULONG required = 0;
    /* Inventory iterator. */
    ULONG index;
    /* Never replace live page tables. */
    if (Npt->Pages != NULL) { return STATUS_ALREADY_REGISTERED; }
    /* Four-level AMD NPT is explicitly limited to the supported width. */
    Npt->AddressMask = KswNptAddressMask(Caps->PhysicalBits);
    /* Reject malformed/unsupported CPUID before a shift or allocation. */
    if (Npt->AddressMask == 0) { return STATUS_NOT_SUPPORTED; }
    /* Establish exact coverage. */
    Npt->Limit = 1ULL << Caps->PhysicalBits;
    /* Preserve enumerated one-GiB support. */
    Npt->Page1Gb = Caps->Page1Gb;
    /* Find WB and UC in the existing hardware PAT. */
    Npt->WbIndex = KswNptPatIndex(Caps->Pat, 6);
    /* Holes/MMIO cannot default to WB. */
    Npt->UcIndex = KswNptPatIndex(Caps->Pat, 0);
    /* Every leaf must have a representable cache policy. */
    if (Npt->WbIndex == 8 || Npt->UcIndex == 8) { return STATUS_NOT_SUPPORTED; }
    /* Estimate structural pages before doing a potentially enormous walk. */
    minimumPages = 1 + ((Npt->Limit + (1ULL << 39) - 1) >> 39);
    /* Without one-GiB leaves every GiB additionally requires one PD. */
    if (!Npt->Page1Gb) { minimumPages += Npt->Limit >> 30; }
    /* Reject incomplete coverage rather than silently clipping it. */
    if (minimumPages > KSW_NPT_MAX_PAGES) { return STATUS_INSUFFICIENT_RESOURCES; }
    /* Allocate a bounded ownership ledger. */
    Npt->Pages = KswordARKAllocateNonPagedPool(KSW_NPT_MAX_PAGES * sizeof(PVOID), 'nSvK');
    /* Leave no partial success on allocation failure. */
    if (Npt->Pages == NULL) { return STATUS_INSUFFICIENT_RESOURCES; }
    /* Snapshot RAM only for cache classification, not for coverage. */
    Npt->Ranges = MmGetPhysicalMemoryRanges();
    /* Clean up partial ownership if Windows cannot supply the inventory. */
    if (Npt->Ranges == NULL) { KswordNptRelease(Npt); return STATUS_INSUFFICIENT_RESOURCES; }
    /* Validate RAM inventory before using any interval arithmetic. */
    for (index = 0; Npt->Ranges[index].NumberOfBytes.QuadPart; ++index) {
        /* Require nonnegative, page-aligned intervals within exact coverage. */
        ULONGLONG start = (ULONGLONG)Npt->Ranges[index].BaseAddress.QuadPart;
        /* Retain unsigned length after checking through the range bound. */
        ULONGLONG bytes = (ULONGLONG)Npt->Ranges[index].NumberOfBytes.QuadPart;
        /* Reject wraparound, partial pages and inventory beyond CPUID. */
        if (start >= Npt->Limit || bytes > Npt->Limit - start || ((start | bytes) & 0xfffULL)) {
            /* Do not leave a usable root after failed coverage validation. */
            KswordNptRelease(Npt); return STATUS_DATA_ERROR;
        }
    }
    /* Count every necessary split using the same traversal as construction. */
    status = KswNptFill(Npt, NULL, 4, 0, &required);
    /* An over-budget map is refused before any hardware table is allocated. */
    if (!NT_SUCCESS(status)) { KswordNptRelease(Npt); return status; }
    /* Allocate the PML4 and recursively construct all lower levels. */
    root = KswNptAllocate(Npt, &Npt->RootPa);
    /* Preserve the first construction failure. */
    status = root != NULL ? KswNptFill(Npt, root, 4, 0, NULL) : STATUS_INSUFFICIENT_RESOURCES;
    /* An incomplete hierarchy must never survive as ready. */
    if (!NT_SUCCESS(status)) { KswordNptRelease(Npt); return status; }
    /* Retain the validated inventory for VMEXIT-safe nested table RAM admission. */
    /* Complete identity coverage is ready. */
    return STATUS_SUCCESS;
}
