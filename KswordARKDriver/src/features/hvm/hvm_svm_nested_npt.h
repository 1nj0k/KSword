/* AMD nested-NPT translation primitives; shared by the driver and host tests.
   APM vol. 2 sections 5.3 and 15.25. These routines never execute virtualization. */
#pragma once
#include "hvm_svm_arch.h"

/* Distinguish a guest translation fault from an inaccessible host operand. */
#define KSW_NNPT_OK 0U
#define KSW_NNPT_FAULT 1U
#define KSW_NNPT_UNREADABLE 2U
#define KSW_NNPT_UNSUPPORTED 3U
#define KSW_NNPT_RETRY 4U
/* Request bits use the architectural nested-fault write and instruction bits. */
#define KSW_NNPT_WRITE 2U
#define KSW_NNPT_EXECUTE 16U
/* AMD always treats guest accesses as user accesses at the nested level. */
#define KSW_NNPT_USER 4ULL
/* Preserve the architectural frame field before applying MAXPHYADDR. */
#define KSW_NNPT_FRAME 0x000ffffffffff000ULL
/* NX restrictions accumulate through every parent entry. */
#define KSW_NNPT_NX (1ULL << 63)
/* Reads must validate RAM ownership before mapping an untrusted table page. */
typedef int (*KSW_NNPT_READ)(void* Context, KSW_SVM_U64 Address, KSW_SVM_U64* Value);
/* Atomically OR A/D only if the entry still equals Expected; never clobber a remap. */
typedef int (*KSW_NNPT_COMPARE_OR)(void* Context, KSW_SVM_U64 Address,
    KSW_SVM_U64 Expected, KSW_SVM_U64 Bits);

/* Retain the complete source path for revalidation and hardware A/D propagation. */
typedef struct _KSW_NNPT_WALK {
    /* Bind the result to its input; equal page offsets alone do not prove a chain. */
    KSW_SVM_U64 InputAddress, Root;
    /* Address at the next physical level, including the original page offset. */
    KSW_SVM_U64 Address;
    /* Effective Present/RW/US/NX flags after intersecting all levels. */
    KSW_SVM_U64 Permissions;
    /* Architectural low fault bits; the caller preserves EXITINFO1[33:32]. */
    KSW_SVM_U64 Fault;
    /* Physical source locations, not pointers into a temporary window. */
    KSW_SVM_U64 EntryAddress[4];
    /* Exact source words at translation time, including software and A/D bits. */
    KSW_SVM_U64 EntryValue[4];
    /* Number of complete reads and the final leaf's address offset width. */
    unsigned int Count, LeafShift;
    /* PAT index decoded according to the final AMD leaf size. */
    unsigned int PatIndex;
    /* Only a successful walk may later publish A/D updates. */
    unsigned int Complete;
    /* A read-only translation cannot later authorize a dirty-bit update. */
    unsigned int Access;
} KSW_NNPT_WALK;

/* Read-only four-level AMD walk. No guessed mappings or synthetic successful reads. */
static __inline unsigned int KswSvmNestedNptWalk(KSW_SVM_U64 Root,
    KSW_SVM_U64 Gpa, unsigned int PhysicalBits, unsigned int Page1Gb,
    unsigned int NxEnabled, unsigned int Access, KSW_NNPT_READ Read,
    void* Context, KSW_NNPT_WALK* Walk)
{
    /* Four-level NPT is the deliberately bounded current address-space contract. */
    const KSW_SVM_U64 mask = KswNptAddressMask(PhysicalBits);
    /* Initialize all fields even when the first operand fails validation. */
    const KSW_NNPT_WALK empty = {0};
    /* Start the permission intersection with Present/RW/US and executable. */
    KSW_SVM_U64 permissions = 7ULL, table = Root;
    /* The walk visits at most four entries. */
    unsigned int level;
    /* Publish deterministic failure output before checking input. */
    if (!Walk) { return KSW_NNPT_UNSUPPORTED; }
    /* Publish deterministic output even for invalid operands. */
    *Walk = empty;
    /* Unknown request types or unsupported addresses are host admission failures. */
    if (!mask || !Read || (Root & ~mask) || (Gpa & ~(mask | 0xfffULL)) ||
        (Access & ~(KSW_NNPT_WRITE | KSW_NNPT_EXECUTE))) { return KSW_NNPT_UNSUPPORTED; }
    /* Retain the exact guest address used to locate this mapping. */
    Walk->InputAddress = Gpa;
    /* Retain the virtual NPT identity for invalidation and diagnostics. */
    Walk->Root = Root;
    /* Retain the access that passed the permission check. */
    Walk->Access = Access;
    /* Nested faults always name a user access, even for an inner ring-zero CPU. */
    Walk->Fault = KSW_NNPT_USER | Access;
    /* Keep the loop bounded without allocating or retaining physical mappings. */
    for (level = 0; level < 4; ++level) {
        /* AMD four-level page-table index: PML4, PDPT, PD, PT. */
        unsigned int shift = 39U - level * 9U;
        /* Record the source slot before reading through the caller's RAM guard. */
        KSW_SVM_U64 location = table + (((Gpa >> shift) & 511ULL) * 8ULL);
        /* Never inspect uninitialized bits after a failed physical read. */
        KSW_SVM_U64 entry = 0;
        /* Identify leaves only after the source word has been read. */
        unsigned int leaf;
        /* A failed physical read is not an architectural not-present fault. */
        if (!Read(Context, location, &entry)) { return KSW_NNPT_UNREADABLE; }
        /* Retain source addresses for atomic A/D updates and stale-path detection. */
        Walk->EntryAddress[level] = location;
        /* Retain every bit, including guest software bits. */
        Walk->EntryValue[level] = entry;
        /* Publish only initialized path elements. */
        Walk->Count = level + 1;
        /* Nonpresent entries ignore their remaining bits architecturally. */
        if (!(entry & 1ULL)) { return KSW_NNPT_FAULT; }
        /* PS is illegal at PML4; at PT bit 7 is PAT, not PS. */
        leaf = level == 3U || (level != 0U && (entry & 0x80ULL) != 0);
        /* MAXPHYADDR and NX enablement apply at every present level. */
        if ((entry & KSW_NNPT_FRAME & ~mask) || (!NxEnabled && (entry & KSW_NNPT_NX)) ||
            (level == 0U && (entry & 0x80ULL)) || (level == 1U && leaf && !Page1Gb)) {
            /* P=1/RSV=1 distinguishes an invalid present entry from a missing page. */
            Walk->Fault |= 9ULL;
            /* Reflect a real reserved-bit violation instead of hiding it. */
            return KSW_NNPT_FAULT;
        }
        /* Writes/user access require permission at all traversed levels. */
        permissions = (permissions & (entry & 7ULL)) | ((permissions | entry) & KSW_NNPT_NX);
        /* Final-page translation retains the complete large-page offset. */
        if (leaf) {
            /* Bits below a large leaf's boundary are reserved, except PAT at bit 12. */
            KSW_SVM_U64 offsetMask = (1ULL << shift) - 1ULL;
            /* A large leaf never treats its PAT bit as physical address bit 12. */
            KSW_SVM_U64 reserved = shift == 12U ? 0ULL : offsetMask & KSW_NNPT_FRAME & ~0x1000ULL;
            /* Reject misaligned large pages rather than silently rounding the frame. */
            if (entry & reserved) { Walk->Fault |= 9ULL; return KSW_NNPT_FAULT; }
            /* Every nested access needs US; writes and fetches add restrictions. */
            if (!(permissions & 4ULL) || ((Access & KSW_NNPT_WRITE) && !(permissions & 2ULL)) ||
                ((Access & KSW_NNPT_EXECUTE) && (permissions & KSW_NNPT_NX))) {
                /* A protection violation has a present source mapping. */
                Walk->Fault |= 1ULL;
                /* Do not publish a usable translation for a denied access. */
                return KSW_NNPT_FAULT;
            }
            /* Combine the aligned physical base with the full guest page offset. */
            Walk->Address = (entry & mask & ~offsetMask) | (Gpa & offsetMask);
            /* Preserve effective restrictions for composition with the outer map. */
            Walk->Permissions = permissions;
            /* Preserve leaf size for A/D and cache diagnostics. */
            Walk->LeafShift = shift;
            /* AMD uses PWT/PCD plus PAT at bit 7 or bit 12, unlike Intel EPT types. */
            Walk->PatIndex = (unsigned int)((entry >> 3) & 3ULL) |
                (unsigned int)(((entry >> (shift == 12U ? 7U : 12U)) & 1ULL) << 2);
            /* No NPF is pending for a successful translation. */
            Walk->Fault = 0;
            /* Only a completed walk can participate in composition or A/D updates. */
            Walk->Complete = 1;
            /* Return the actual mapped result. */
            return KSW_NNPT_OK;
        }
        /* Interior entries always point at a 4-KiB table. */
        table = entry & mask;
    }
    /* The fourth level is always a leaf; never accept malformed loop fallthrough. */
    return KSW_NNPT_UNSUPPORTED;
}

/* Validate first, then publish A/D using compare-and-OR on the exact source slots. */
static __inline unsigned int KswSvmNestedNptCommitAd(KSW_NNPT_WALK* Walk,
    unsigned int Written, KSW_NNPT_READ Read, KSW_NNPT_COMPARE_OR Update, void* Context)
{
    /* A partial walk must never dirty an unrelated source entry. */
    unsigned int index;
    /* Require both the read and atomic update contracts. */
    if (!Walk || !Walk->Complete || Walk->Count == 0U || Walk->Count > 4U || !Read || !Update ||
        (Written && !(Walk->Access & KSW_NNPT_WRITE))) { return KSW_NNPT_UNSUPPORTED; }
    /* A failed commit must not leave a publishable translation behind. */
    Walk->Complete = 0;
    /* Revalidate the entire path before the first software-maintained A/D write. */
    for (index = 0; index < Walk->Count; ++index) {
        /* Each read starts from a defined sentinel. */
        KSW_SVM_U64 value = 0;
        /* Failed access must not become a guest page fault. */
        if (!Read(Context, Walk->EntryAddress[index], &value)) { return KSW_NNPT_UNREADABLE; }
        /* Concurrent remapping, including A/D changes, requires a fresh walk. */
        if (value != Walk->EntryValue[index]) { return KSW_NNPT_RETRY; }
    }
    /* Partial accessed-bit progress on a retry is legal; frame updates are never lost. */
    for (index = 0; index < Walk->Count; ++index) {
        /* Only the final leaf receives Dirty for an actually performed write. */
        KSW_SVM_U64 bits = 0x20ULL | ((Written && index + 1U == Walk->Count) ? 0x40ULL : 0ULL);
        /* Never write a stale cached entry back over a concurrent source mutation. */
        if (!Update(Context, Walk->EntryAddress[index], Walk->EntryValue[index], bits)) { return KSW_NNPT_RETRY; }
        /* Keep the snapshot consistent with this successful update. */
        Walk->EntryValue[index] |= bits;
    }
    /* The caller still owns invalidation/epoch serialization before hardware publication. */
    Walk->Complete = 1;
    /* Every source word still referred to the walked mapping at its atomic update. */
    return KSW_NNPT_OK;
}

/* Compose a 4-KiB WB/UC leaf; unsupported cache types require explicit admission refusal. */
static __inline unsigned int KswSvmNestedNptCompose4k(const KSW_NNPT_WALK* Inner,
    const KSW_NNPT_WALK* Outer, KSW_SVM_U64 InnerPat, KSW_SVM_U64 OuterPat,
    KSW_SVM_U64 HardwarePat, KSW_SVM_U64* Leaf)
{
    /* Resolve both source leaf cache indices without rewriting the host PAT. */
    unsigned int innerType, outerType, type, index;
    /* A rejected composition must not leave a stale valid leaf behind. */
    if (!Leaf) { return KSW_NNPT_UNSUPPORTED; }
    /* Clear output before validating either input. */
    *Leaf = 0;
    /* Do not compose unrelated or incomplete physical translations. */
    if (!Inner || !Outer || !Inner->Complete || !Outer->Complete ||
        Inner->Address != Outer->InputAddress || Inner->Access != Outer->Access ||
        Inner->PatIndex > 7U || Outer->PatIndex > 7U ||
        (Inner->Address & 0xfffULL) != (Outer->Address & 0xfffULL)) { return KSW_NNPT_UNSUPPORTED; }
    /* Guest NPT cache semantics use the corresponding hypervisor's PAT. */
    innerType = (unsigned int)((InnerPat >> (Inner->PatIndex * 8U)) & 0xffULL);
    /* Outer source NPT has its own cache selection. */
    outerType = (unsigned int)((OuterPat >> (Outer->PatIndex * 8U)) & 0xffULL);
    /* Initial composition deliberately accepts only the unambiguous WB/UC subset. */
    if ((innerType != 0U && innerType != 6U) || (outerType != 0U && outerType != 6U)) { return KSW_NNPT_UNSUPPORTED; }
    /* UC dominates WB; an unknown memory type is never promoted to WB. */
    type = innerType == 0U || outerType == 0U ? 0U : 6U;
    /* Select an existing host PAT slot with exactly the required effective type. */
    for (index = 0; index < 8U; ++index) {
        /* Preserve all permission intersections and propagate either NX restriction. */
        if (((HardwarePat >> (index * 8U)) & 0xffULL) == type) {
            /* Hardware A/D begins clear and must later be propagated through both paths. */
            *Leaf = (Outer->Address & KSW_NNPT_FRAME) | (Inner->Permissions & Outer->Permissions & 7ULL) |
                ((Inner->Permissions | Outer->Permissions) & KSW_NNPT_NX) | (KswNptLeafFlags(1U, index) & ~7ULL);
            /* Return the actual encoding selected for the host's PAT layout. */
            return KSW_NNPT_OK;
        }
    }
    /* No compatible PAT slot is an unsupported mapping, not a reason to write PAT. */
    return KSW_NNPT_UNSUPPORTED;
}
