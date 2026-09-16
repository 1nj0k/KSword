/* Pure EPT snapshot comparison, also compiled by the host-side unit test. */
#pragma once

/* Only monotonic hardware A/D updates preserve a cached translation. */
static __inline int KswordHvmEptEntryKeepsTranslation(
    unsigned long long Before, unsigned long long After,
    unsigned int Level, int AccessedDirty)
{
    /* No change always preserves both mapping and tracking semantics. */
    unsigned long long allowed = 0ULL;
    /* Permit A only for an existing entry when EPTP enabled A/D. */
    if (AccessedDirty && (Before & 7ULL) != 0ULL) {
        /* A is maintained for every traversed level. */
        allowed = 1ULL << 8;
        /* D exists only in a 4-KiB, 2-MiB, or 1-GiB leaf. */
        if (Level == 3U || ((Level == 1U || Level == 2U) &&
                               (Before & (1ULL << 7)) != 0ULL)) {
            /* Accept a hardware dirty-bit set in a valid leaf. */
            allowed |= 1ULL << 9;
        }
    }
    /* Clearing A/D starts a new observation epoch and must invalidate. */
    return ((Before & ~After) == 0ULL &&
            ((Before ^ After) & ~allowed) == 0ULL) ? 1 : 0;
}
