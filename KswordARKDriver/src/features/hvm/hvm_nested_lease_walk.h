/* A lease identifies an EPT translation path, not merely its recyclable root. */
#pragma once

/* Keep the walker independent of Windows so adversarial tables can be tested. */
typedef unsigned long long KSW_LEASE_U64;
/* A reader must copy one complete aligned word or report failure. */
typedef int (*KSW_LEASE_READ)(void* Context, KSW_LEASE_U64 Address, KSW_LEASE_U64* Value);
/* Architectural EPT frame bits, independent of the host's pointer size. */
#define KSW_LEASE_FRAME 0x000FFFFFFFFFF000ULL
/* A bounded four-level path includes the terminating large or ordinary leaf. */
typedef struct _KSW_HVM_PAGE_TRANSLATION {
    /* Retain EPT configuration bits as well as the root frame. */
    KSW_LEASE_U64 EptPointer;
    /* Bind one page in the descendant physical-address domain. */
    KSW_LEASE_U64 GuestPage;
    /* Record the source frame in Windows 1's physical-address domain. */
    KSW_LEASE_U64 SourcePage;
    /* Retain path addresses so table-page replacement invalidates the lease. */
    KSW_LEASE_U64 EntryAddress[4];
    /* Mask only architecturally meaningful accessed/dirty flags. */
    KSW_LEASE_U64 EntryValue[4];
    /* Count exactly the entries successfully captured. */
    unsigned int EntryCount;
    /* Intersect permissions over the entire path. */
    unsigned int Permissions;
} KSW_HVM_PAGE_TRANSLATION;

/* Only A/D flags may change without changing the translation identity. */
static __inline KSW_LEASE_U64 KswordHvmLeaseNormalize(
    KSW_LEASE_U64 EptPointer, KSW_LEASE_U64 Entry, unsigned int Level)
{
    /* Dirty is defined only for a leaf; an interior reserved-bit change matters. */
    const int leaf = Level == 3U || ((Level == 1U || Level == 2U) && (Entry & 0x80ULL) != 0ULL);
    /* With A/D disabled these bits are not hardware-maintained metadata. */
    const KSW_LEASE_U64 ignored = (EptPointer & 0x40ULL) != 0ULL ? (leaf ? 0x300ULL : 0x100ULL) : 0ULL;
    /* Preserve every other permission, frame and configuration bit. */
    return Entry & ~ignored;
}

/* Capture ordinary WB RAM through the supported four-level EPT format. */
static __inline int KswordHvmLeaseCapture(KSW_LEASE_U64 EptPointer,
    KSW_LEASE_U64 GuestPage, KSW_LEASE_READ Read, void* Context,
    KSW_HVM_PAGE_TRANSLATION* Translation)
{
    /* Hardware walk order, including 1-GiB and 2-MiB leaf offsets. */
    static const unsigned int shifts[4] = {39U, 30U, 21U, 12U};
    /* Resolve the first source table without following a virtual pointer. */
    KSW_LEASE_U64 table = EptPointer & KSW_LEASE_FRAME;
    /* Start with the intersection identity for RWX permissions. */
    unsigned int permissions = 7U, level;
    /* Use a constant initializer supported by both WDK C and the host tests. */
    const KSW_HVM_PAGE_TRANSLATION empty = {0};
    /* Initialize the result before any possibly failing read. */
    *Translation = empty;
    /* This implementation accepts WB, four-level EPT without unknown EPTP bits. */
    if (Read == 0 || table == 0ULL || (EptPointer & 0x3FULL) != 0x1EULL ||
        (EptPointer & ~(KSW_LEASE_FRAME | 0x7FULL)) != 0ULL ||
        (GuestPage & ~KSW_LEASE_FRAME) != 0ULL || GuestPage >= (1ULL << 48)) { return 0; }
    /* Bind the exact root configuration and target page. */
    Translation->EptPointer = EptPointer;
    /* Preserve the caller's already page-aligned GPA. */
    Translation->GuestPage = GuestPage;
    /* Follow at most four bounded, independently checked physical reads. */
    for (level = 0U; level < 4U; ++level) {
        /* Select one aligned entry from the current table. */
        const KSW_LEASE_U64 address = table + (((GuestPage >> shifts[level]) & 0x1FFULL) << 3);
        /* Never inspect uninitialized bytes after a failed read. */
        KSW_LEASE_U64 entry = 0ULL;
        /* Inaccessible and non-present translations cannot be leased. */
        if (!Read(Context, address, &entry) || (entry & 7ULL) == 0ULL) { return 0; }
        /* Write without read is an architectural EPT misconfiguration. */
        if ((entry & 3ULL) == 2ULL) { return 0; }
        /* Large pages are legal only at the PDPT and PD levels. */
        if ((entry & 0x80ULL) != 0ULL && (level == 0U || level == 3U)) { return 0; }
        /* Preserve the actual entry address, including all path replacements. */
        Translation->EntryAddress[level] = address;
        /* Permit A/D maintenance without tolerating address or permission drift. */
        Translation->EntryValue[level] = KswordHvmLeaseNormalize(EptPointer, entry, level);
        /* Publish the number of complete source entries. */
        Translation->EntryCount = level + 1U;
        /* A parent permission restriction applies to every descendant leaf. */
        permissions &= (unsigned int)(entry & 7ULL);
        /* Stop at a supported large leaf or the final ordinary leaf. */
        if (level == 3U || (entry & 0x80ULL) != 0ULL) {
            /* Large-page address bits below its alignment must be zero. */
            const KSW_LEASE_U64 offsetMask = (1ULL << shifts[level]) - 1ULL;
            /* Reject non-WB memory, reserved address bits and an empty intersection. */
            if ((entry & 0x38ULL) != 0x30ULL ||
                (entry & KSW_LEASE_FRAME & offsetMask) != 0ULL || permissions == 0U) { return 0; }
            /* Preserve the target's offset within a large backing page. */
            Translation->SourcePage = (entry & KSW_LEASE_FRAME) | (GuestPage & offsetMask);
            /* Retain effective permissions for evidence and verification. */
            Translation->Permissions = permissions;
            /* A second read pass by the caller rejects an already changed capture. */
            return 1;
        }
        /* Continue only through a nonzero interior table frame. */
        table = entry & KSW_LEASE_FRAME;
        /* Never read the physical zero page as a missing child table. */
        if (table == 0ULL) { return 0; }
    }
    /* A walk that did not terminate cannot admit a page override. */
    return 0;
}

/* Returns 1 for the same path, 0 for drift, and -1 for unreadable source memory. */
static __inline int KswordHvmLeaseValidate(const KSW_HVM_PAGE_TRANSLATION* Translation,
    KSW_LEASE_READ Read, void* Context)
{
    /* Bound every verification independently of a caller's captured count. */
    unsigned int level;
    /* Empty or malformed captures are never considered valid leases. */
    if (Read == 0 || Translation->EntryCount == 0U || Translation->EntryCount > 4U) { return -1; }
    /* Revalidate parents before their children so changed paths fail immediately. */
    for (level = 0U; level < Translation->EntryCount; ++level) {
        /* A failed callback must not leave a usable stale word. */
        KSW_LEASE_U64 value = 0ULL;
        /* Report inability to verify separately from a proven semantic change. */
        if (!Read(Context, Translation->EntryAddress[level], &value)) { return -1; }
        /* Address, permission, cache-type and path changes all expire this lease. */
        if (KswordHvmLeaseNormalize(Translation->EptPointer, value, level) != Translation->EntryValue[level]) { return 0; }
    }
    /* This proves sampled translation identity, not an OS-level guest boot identity. */
    return 1;
}
