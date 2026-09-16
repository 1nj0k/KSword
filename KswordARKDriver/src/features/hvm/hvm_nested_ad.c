/* Lossless per-CPU EPT A/D propagation with no allocation in a VM exit. */
#include "hvm_nested_ept.h"
#include "hvm_nested_ad_compare.h"
#include "../../platform/pool_compat.h"

/* Keep allocation accounting separate from the page-table pool tag. */
#define KSW_HVM_AD_TAG 'AdHK'
/* Architectural A/D bits in an EPT entry. */
#define KSW_HVM_AD_BITS 0x300ULL

NTSTATUS KswordARKHvmNestedAdPrepare(KSW_HVM_SHADOW_EPT_STATE* Shadow)
{
    /* Allocate entries and a dense pending-index array in one owned block. */
    const SIZE_T entries = (SIZE_T)KSW_HVM_NEPT_AD_RECORDS * sizeof(*Shadow->AdEntries);
    /* Reserve enough pending indices for every possible shadow leaf. */
    const SIZE_T bytes = entries + (SIZE_T)KSW_HVM_NEPT_AD_RECORDS * sizeof(ULONG);
    /* Preserve an existing reservation. */
    if (Shadow->AdEntries != NULL) { return STATUS_SUCCESS; }
    /* Only called while preparing resources, before any VMX-root entry. */
    Shadow->AdEntries = (KSW_HVM_NEPT_AD_ENTRY*)KswordARKAllocateNonPagedPool(bytes, KSW_HVM_AD_TAG);
    /* Refuse advertising A/D without its backing ledger. */
    if (Shadow->AdEntries == NULL) { return STATUS_INSUFFICIENT_RESOURCES; }
    /* Every slot begins absent and belongs to generation zero. */
    RtlZeroMemory(Shadow->AdEntries, bytes);
    /* ULONG indices follow the naturally aligned entry array. */
    Shadow->AdPendingSlots = (ULONG*)((UCHAR*)Shadow->AdEntries + entries);
    /* No shadow leaf has been composed yet. */
    Shadow->AdRecordCount = 0UL;
    /* Report a complete reservation. */
    return STATUS_SUCCESS;
}

VOID KswordARKHvmNestedAdRelease(KSW_HVM_SHADOW_EPT_STATE* Shadow)
{
    /* Cleanup is also valid after a partial preparation failure. */
    if (Shadow->AdEntries != NULL) {
        /* All CPUs have stopped using this owned allocation. */
        ExFreePool(Shadow->AdEntries);
        /* Do not retain a dangling allocation pointer. */
        Shadow->AdEntries = NULL;
    }
    /* The pending array is an interior pointer into the same allocation. */
    Shadow->AdPendingSlots = NULL;
    /* No pending indices remain valid after release. */
    Shadow->AdRecordCount = 0UL;
}

BOOLEAN KswordARKHvmNestedAdRecord(KSW_HVM_SHADOW_EPT_STATE* Shadow,
    volatile ULONGLONG* Leaf, ULONGLONG L1EntryAddress)
{
    /* All leaf pointers must refer into this CPU's reserved table block. */
    const ULONG_PTR offset = (ULONG_PTR)Leaf - (ULONG_PTR)Shadow->PageBlock;
    /* Division is exact after the alignment check below. */
    const SIZE_T slot = offset / sizeof(ULONGLONG);
    /* Select a slot only after validating its allocation and address. */
    KSW_HVM_NEPT_AD_ENTRY* entry = NULL;
    /* Reject invalid addresses rather than silently losing dirty tracking. */
    if (Shadow->AdEntries == NULL || (offset & 7UL) != 0UL ||
        slot >= KSW_HVM_NEPT_AD_RECORDS || (L1EntryAddress & 7ULL) != 0ULL ||
        L1EntryAddress == 0ULL) {
        /* Make every contract failure observable. */
        Shadow->AdOverflowCount += 1UL;
        /* The caller must refuse entry with an untracked leaf. */
        return FALSE;
    }
    /* A leaf index directly selects its source metadata. */
    entry = &Shadow->AdEntries[slot];
    /* A new generation or a completed leaf needs one pending index. */
    if (entry->Generation != Shadow->Generation || entry->Pending == 0U) {
        /* One index per leaf makes overflow impossible without a defect. */
        if (Shadow->AdRecordCount >= KSW_HVM_NEPT_AD_RECORDS) {
            /* Count rather than discard an older pending dirty observation. */
            Shadow->AdOverflowCount += 1UL;
            /* Refuse the new mapping. */
            return FALSE;
        }
        /* Append this unique shadow-leaf slot. */
        Shadow->AdPendingSlots[Shadow->AdRecordCount++] = (ULONG)slot;
    }
    /* Bind the slot to its current hierarchy generation. */
    entry->Generation = Shadow->Generation;
    /* Retain only an L1 physical address, never a temporary window mapping. */
    entry->L1EntryAddress = L1EntryAddress;
    /* A refilled leaf starts a fresh hardware observation. */
    entry->PublishedBits = 0U;
    /* The slot must remain until future writes can no longer add a bit. */
    entry->Pending = 1U;
    /* Report that every future dirty bit has a destination. */
    return TRUE;
}

/* Update only our own A/D changes in the snapshot, preserving mapping edits. */
static VOID KswordHvmAdUpdateCopy(KSW_HVM_SHADOW_EPT_STATE* Shadow,
    ULONGLONG Address, ULONGLONG Bits)
{
    /* Find the snapshot of the source table, never a shadow table. */
    ULONG index = 0UL;
    /* Bound the search by the pages actually recorded. */
    for (index = 0UL; index < Shadow->TrackedCount; ++index) {
        /* A source entry belongs to exactly one physical table page. */
        if (Shadow->TrackedFrame[index] == (Address & ~0xFFFULL)) {
            /* Compute the aligned entry in our private snapshot. */
            ULONGLONG* copy = (ULONGLONG*)((UCHAR*)Shadow->TrackedCopyBlock +
                (SIZE_T)index * PAGE_SIZE + (SIZE_T)(Address & 0xFFFULL));
            /* Preserve all original address and permission fields. */
            *copy |= Bits;
            /* Do not accidentally absorb an unrelated L1 table edit. */
            return;
        }
    }
}

ULONG KswordARKHvmNestedEptPropagateAccessedDirty(
    KSW_HVM_SHADOW_EPT_STATE* Shadow, KSW_HVM_PHYS_WINDOW* Window)
{
    /* Walk only entries with a bit still capable of changing. */
    ULONG pending = 0UL;
    /* Count physical source entries actually changed by this call. */
    ULONG updated = 0UL;
    /* Nothing needs propagation when A/D is not in use. */
    if (Shadow == NULL || Window == NULL || !Shadow->AccessedDirtyActive ||
        Shadow->AdEntries == NULL || Shadow->PageBlock == NULL) { return 0UL; }
    /* A dense pending list avoids an O(all reserved entries) scan. */
    while (pending < Shadow->AdRecordCount) {
        /* Every index was range-checked by the record publisher. */
        const ULONG slot = Shadow->AdPendingSlots[pending];
        /* Read the hardware-maintained leaf directly, without four page walks. */
        const ULONGLONG leaf = ((volatile ULONGLONG*)Shadow->PageBlock)[slot];
        /* Source metadata remains valid for this shadow generation. */
        KSW_HVM_NEPT_AD_ENTRY* entry = &Shadow->AdEntries[slot];
        /* Only two hardware bits can be published. */
        const ULONGLONG bits = leaf & KSW_HVM_AD_BITS;
        /* Skip physical mappings when no newly set bit exists. */
        if ((bits & ~((ULONGLONG)entry->PublishedBits << 8)) != 0ULL) {
            /* A temporary mapping never survives this propagation call. */
            volatile VOID* mapped = NULL;
            /* An inaccessible source retains its pending record for retry. */
            if (KswordARKHvmPhysWindowMap(Window, entry->L1EntryAddress,
                    sizeof(ULONGLONG), &mapped) == KSW_HVM_PHYS_WINDOW_OK && mapped != NULL) {
                /* Atomic OR cannot lose another CPU's mapping or A/D update. */
                const ULONGLONG prior = (ULONGLONG)InterlockedOr64(
                    (volatile LONG64*)mapped, (LONG64)bits);
                /* Restore the physical window before touching other mappings. */
                KswordARKHvmPhysWindowUnmap(Window);
                /* Count real source-bit changes, not already-published bits. */
                if ((prior & bits) != bits) { updated += 1UL; }
                /* Remember the set bits so L1 clearing them remains detectable. */
                KswordHvmAdUpdateCopy(Shadow, entry->L1EntryAddress, bits);
                /* This shadow leaf no longer needs another OR of those bits. */
                entry->PublishedBits |= (UCHAR)(bits >> 8);
            }
        }
        /* A read-only leaf cannot later become dirty without another refill. */
        if (entry->PublishedBits == 3U ||
            (entry->PublishedBits == 1U && (leaf & 2ULL) == 0ULL)) {
            /* Mark completion before compacting its index out of the list. */
            entry->Pending = 0U;
            /* Remove in O(1), retaining the last pending entry for this index. */
            Shadow->AdRecordCount -= 1UL;
            /* The replacement index is processed on the next loop iteration. */
            Shadow->AdPendingSlots[pending] = Shadow->AdPendingSlots[Shadow->AdRecordCount];
        } else {
            /* Writable A-only leaves remain observable for a future write. */
            pending += 1UL;
        }
    }
    /* Keep a cumulative count for runtime evidence. */
    Shadow->AdPropagatedCount += updated;
    /* Report real source updates to the caller. */
    return updated;
}

BOOLEAN KswordARKHvmNestedAdCompare(KSW_HVM_SHADOW_EPT_STATE* Shadow,
    ULONG TrackedIndex, const volatile ULONGLONG* Current)
{
    /* Point at the exact snapshot that produced the cached mappings. */
    ULONGLONG* copy = (ULONGLONG*)((UCHAR*)Shadow->TrackedCopyBlock +
        (SIZE_T)TrackedIndex * PAGE_SIZE);
    /* Inspect every entry in the tracked table. */
    ULONG index = 0UL;
    /* Entry loads are aligned and naturally atomic on the supported x64 host. */
    for (index = 0UL; index < 512UL; ++index) {
        /* Read once so comparison and snapshot advancement use the same value. */
        const ULONGLONG now = Current[index];
        /* Any changed mapping, reserved bit, or A/D clear requires a rebuild. */
        if (!KswordHvmEptEntryKeepsTranslation(copy[index], now,
                Shadow->TrackedLevel[TrackedIndex], Shadow->L1RequestedAccessedDirty)) {
            /* The caller immediately discards this whole snapshot generation. */
            return FALSE;
        }
        /* Accept only monotonic A/D progress; later clears must be observable. */
        copy[index] = now;
    }
    /* No translation or observation epoch changed in this table. */
    return TRUE;
}
