/*++

Module Name:

    hvm_nested.c

Abstract:

    Implements a bounded vmcs12 state machine and explicit partial dispatch for
    VMX instructions.  L2 entry and shadow EPT are rejected with architectural
    VMfail semantics until their complete merge and invalidation paths exist.

Environment:

    Kernel-mode Driver Framework.

--*/

#include "hvm_nested.h"
#include "hvm_nested_decode.h"
#include "hvm_nested_l2.h"
#include "hvm_phys_window.h"
/*
 * The full resident context is needed, not just its forward declaration: L2
 * entry reaches through it for the VMCS pages and the mapping window.
 */
#include "hvm_resident.h"
#include "hvm_exit.h"
/* VMCS access goes through the seam in hvm_vmcs.h, never the raw intrinsic. */
#include "hvm_vmcs.h"
/* The evidence ring already retains nested-VMX events; this module writes them. */
#include "hvm_event.h"

#if defined(_M_AMD64)
#include <intrin.h>

/* Name the VMCS guest instruction pointer field. */
#define KSW_VMCS_GUEST_RIP 0x681EUL
/* Name the VMCS guest RFLAGS field. */
#define KSW_VMCS_GUEST_RFLAGS 0x6820UL

/* Name Intel VMX-instruction exit reasons handled by nested dispatch. */
#define KSW_VMX_EXIT_VMCLEAR   19UL
/* Name the Intel VMLAUNCH exit reason. */
#define KSW_VMX_EXIT_VMLAUNCH  20UL
/* Name the Intel VMPTRLD exit reason. */
#define KSW_VMX_EXIT_VMPTRLD   21UL
/* Name the Intel VMPTRST exit reason. */
#define KSW_VMX_EXIT_VMPTRST   22UL
/* Name the Intel VMREAD exit reason. */
#define KSW_VMX_EXIT_VMREAD    23UL
/* Name the Intel VMRESUME exit reason. */
#define KSW_VMX_EXIT_VMRESUME  24UL
/* Name the Intel VMWRITE exit reason. */
#define KSW_VMX_EXIT_VMWRITE   25UL
/* Name the Intel VMXOFF exit reason. */
#define KSW_VMX_EXIT_VMXOFF    26UL
/* Name the Intel VMXON exit reason. */
#define KSW_VMX_EXIT_VMXON     27UL
/* Name the Intel INVEPT exit reason. */
#define KSW_VMX_EXIT_INVEPT    50UL
/* Name the Intel INVVPID exit reason. */
#define KSW_VMX_EXIT_INVVPID   53UL

/* Identify carry and zero flags used by VMX instruction results. */
#define KSW_RFLAGS_CF (1ULL << 0)
/* Identify the zero flag used by VMfailValid. */
#define KSW_RFLAGS_ZF (1ULL << 6)

/* Name Intel VM-instruction error seven for incomplete L2 VM entry. */
#define KSW_VMX_ERROR_INVALID_CONTROL_FIELDS 7UL
/* Name Intel VM-instruction error five for resume before launch. */
#define KSW_VMX_ERROR_RESUME_NON_LAUNCHED_VMCS 5UL

/* Advance guest RIP after a fully decoded nested instruction. */
static BOOLEAN
KswordARKHvmNestedAdvanceRip(
    _In_ ULONG InstructionLength
    )
{
    SIZE_T guestRip = 0U;

    /* Reject a zero or architecturally oversized instruction length. */
    if (InstructionLength == 0UL ||
        InstructionLength > 15UL) {
        /* Report that the dispatcher cannot resume safely. */
        return FALSE;
    }
    /* Read the current guest instruction pointer. */
    if (KswordARKHvmVmcsFieldLoad(
            KSW_VMCS_GUEST_RIP,
            &guestRip) != 0U) {
        /* Report VMREAD failure to the caller. */
        return FALSE;
    }
    /* Advance to the instruction following the intercepted VMX operation. */
    guestRip += InstructionLength;
    /* Write the advanced guest instruction pointer. */
    return KswordARKHvmVmcsFieldStore(
        KSW_VMCS_GUEST_RIP,
        guestRip) == 0U;
}

/* Publish one VMX instruction result through guest CF and ZF. */
static BOOLEAN
KswordARKHvmNestedSetInstructionResult(
    _In_ UCHAR Result
    )
{
    SIZE_T guestRflags = 0U;

    /* Read the current guest RFLAGS field. */
    if (KswordARKHvmVmcsFieldLoad(
            KSW_VMCS_GUEST_RFLAGS,
            &guestRflags) != 0U) {
        /* Report VMREAD failure to the caller. */
        return FALSE;
    }
    /* Clear both VMX result flags before selecting the result. */
    guestRflags &=
        ~((SIZE_T)KSW_RFLAGS_CF |
          (SIZE_T)KSW_RFLAGS_ZF);
    /* Set carry for VMfailInvalid result two. */
    if (Result == 2U) {
        /* Publish VMfailInvalid through guest carry. */
        guestRflags |= (SIZE_T)KSW_RFLAGS_CF;
    /* Set zero for VMfailValid result one. */
    } else if (Result == 1U) {
        /* Publish VMfailValid through guest zero. */
        guestRflags |= (SIZE_T)KSW_RFLAGS_ZF;
    }
    /* Write the complete guest RFLAGS result. */
    return KswordARKHvmVmcsFieldStore(
        KSW_VMCS_GUEST_RFLAGS,
        guestRflags) == 0U;
}

VOID
KswordARKHvmNestedInitializeVcpu(
    _Out_ KSW_HVM_NESTED_VCPU* Nested,
    _In_ BOOLEAN Enabled,
    _In_ ULONGLONG L0EptPointer
    )
{
    /* Reject a missing per-processor state record. */
    if (Nested == NULL) {
        /* Return without publishing partial state. */
        return;
    }
    /* Initialize the complete bounded vmcs12 state. */
    RtlZeroMemory(Nested, sizeof(*Nested));
    /* Publish the caller-selected dispatch enable state. */
    Nested->Enabled = Enabled;
    /* Publish dispatch-ready only when explicitly enabled. */
    Nested->State = Enabled
        ? KSWORD_ARK_HVM_NESTED_STATE_DISPATCH_READY
        : KSWORD_ARK_HVM_NESTED_STATE_DISABLED;
    /* Initialize bounded vmcs12 and explicit partial vmcs02 state. */
    KswordARKHvmNestedVmcsInitialize(
        &Nested->Vmcs12,
        &Nested->Vmcs02);
    /* Initialize shadow-EPT state from the exact L0 EPT pointer. */
    KswordARKHvmNestedEptInitialize(
        &Nested->ShadowEpt,
        L0EptPointer);
}

NTSTATUS
KswordARKHvmNestedValidate(
    _Inout_ KSW_HVM_RUNTIME* Runtime
    )
{
    /* Reject a missing runtime before evaluating nested capability. */
    if (Runtime == NULL) {
        /* Return the exact caller-contract failure. */
        return STATUS_INVALID_PARAMETER;
    }
    /* Require VMX exposure before publishing instruction dispatch. */
    if ((Runtime->FeatureFlags &
            KSWORD_ARK_HVM_FEATURE_VMX) == 0ULL) {
        /* Preserve explicit unsupported maturity. */
        Runtime->NestedImplementation =
            KSWORD_ARK_HVM_IMPLEMENTATION_UNSUPPORTED;
        /* Preserve explicit disabled nested state. */
        Runtime->NestedState =
            KSWORD_ARK_HVM_NESTED_STATE_DISABLED;
        /* Return the explicit processor capability boundary. */
        return STATUS_NOT_SUPPORTED;
    }
    /* Publish bounded instruction-dispatch capability. */
    Runtime->FeatureFlags |=
        KSWORD_ARK_HVM_FEATURE_NESTED_VMX_DISPATCH |
        KSWORD_ARK_HVM_FEATURE_VMX_INSTRUCTION_EMULATION;
    /* Publish explicit partial maturity until vmcs02 and shadow EPT exist. */
    Runtime->NestedImplementation =
        KSWORD_ARK_HVM_IMPLEMENTATION_PARTIAL;
    /* Publish dispatch-ready rather than active L2 state. */
    Runtime->NestedState =
        KSWORD_ARK_HVM_NESTED_STATE_DISPATCH_READY;
    /* Publish protocol-visible partial state. */
    KswordARKHvmStateSet(Runtime, KSWORD_ARK_HVM_STATE_NESTED_PARTIAL);
    /* Return not-implemented so validation cannot be mistaken for L2 support. */
    return STATUS_NOT_IMPLEMENTED;
}

/*
 * Name the VMX instruction results the dispatchers return.
 *
 * These are the three architectural outcomes, not status codes: success sets
 * neither CF nor ZF, VMfailValid sets ZF and publishes an error number that L1
 * can VMREAD, and VMfailInvalid sets CF and carries no error number because
 * there is no current VMCS to record one in.
 */
#define KSW_HVM_VMX_RESULT_SUCCEED 0U
#define KSW_HVM_VMX_RESULT_FAIL_VALID 1U
#define KSW_HVM_VMX_RESULT_FAIL_INVALID 2U

/* Name the Intel VM-instruction errors this dispatch can report. */
#define KSW_VMX_ERROR_VMCLEAR_INVALID_ADDRESS 2UL
#define KSW_VMX_ERROR_VMCLEAR_VMXON_POINTER 3UL
#define KSW_VMX_ERROR_VMPTRLD_INVALID_ADDRESS 9UL
#define KSW_VMX_ERROR_VMPTRLD_VMXON_POINTER 10UL
#define KSW_VMX_ERROR_UNSUPPORTED_COMPONENT 12UL
#define KSW_VMX_ERROR_VMWRITE_READ_ONLY 13UL
#define KSW_VMX_ERROR_VMXON_IN_ROOT 15UL
#define KSW_VMX_ERROR_INVALID_INVALIDATION_OPERAND 28UL

/* Name the VMCS field-encoding type that marks a read-only component. */
#define KSW_VMCS_ENCODING_TYPE_READ_ONLY 1UL

/*
 * Check one VMX region pointer against the constraints the architecture fixes.
 *
 * Deliberately absent: the revision-identifier check.  Verifying it means
 * reading the first four bytes of the region, and the region is named by a
 * *physical* address while the only VM-exit-safe accessor we have takes a
 * linear one.  Being more permissive than the architecture is safe here
 * because we never read or write the region at all: L1's VMX state lives in
 * our own records, not in the pages it nominated.  Revisit once a
 * VM-exit-safe physical accessor exists - vmcs02 merge will need one too.
 */
static BOOLEAN
KswordARKHvmNestedIsRegionPointerValid(
    _In_ ULONGLONG Pointer
    )
{
    /* Reject the null pointer the architecture never accepts. */
    if (Pointer == 0ULL) {
        /* Report the pointer as unusable. */
        return FALSE;
    }
    /* Reject a region that is not page aligned. */
    if ((Pointer & 0xFFFULL) != 0ULL) {
        /* Report the pointer as unusable. */
        return FALSE;
    }
    /* Reject bits beyond the architectural physical-address maximum. */
    if ((Pointer & ~0x000FFFFFFFFFF000ULL) != 0ULL) {
        /* Report the pointer as unusable. */
        return FALSE;
    }
    /* Report the pointer as architecturally usable. */
    return TRUE;
}

/* Read the region pointer one memory-operand VMX instruction named. */
static BOOLEAN
KswordARKHvmNestedLoadRegionPointer(
    _Inout_opt_ struct _KSW_HVM_PHYS_WINDOW* Window,
    _In_ const struct _KSW_HVM_GPR_FRAME* Frame,
    _Out_ ULONGLONG* Pointer
    )
{
    KSW_HVM_VMX_OPERAND operand = { 0 };

    *Pointer = 0ULL;
    /* Decode the memory operand under the memory-only layout. */
    if (!NT_SUCCESS(KswordARKHvmNestedDecodeOperand(
            Frame,
            KSW_HVM_VMX_OPERAND_LAYOUT_MEMORY_ONLY,
            &operand))) {
        /* Report that no pointer could be produced. */
        return FALSE;
    }
    /* Read the eight-byte pointer the operand addresses. */
    if (!NT_SUCCESS(KswordARKHvmNestedReadGuestQword(
            Window,
            operand.LinearAddress,
            Pointer))) {
        /* Report that no pointer could be produced. */
        return FALSE;
    }
    /* Report a complete region pointer. */
    return TRUE;
}

/* Dispatch VMXON and establish emulated L1 VMX operation. */
static UCHAR
KswordARKHvmNestedDispatchVmxon(
    _Inout_ KSW_HVM_NESTED_VCPU* Nested,
    _In_ const struct _KSW_HVM_GPR_FRAME* Frame,
    _Out_ ULONG* InstructionError
    )
{
    ULONGLONG region = 0ULL;

    *InstructionError = 0UL;
    /* Report the architectural error for VMXON inside VMX operation. */
    if (Nested->Vmxon) {
        *InstructionError = KSW_VMX_ERROR_VMXON_IN_ROOT;
        /* Return the valid failure L1 can read an error number from. */
        return KSW_HVM_VMX_RESULT_FAIL_VALID;
    }
    /* Refuse when the operand could not be produced at all. */
    if (!KswordARKHvmNestedLoadRegionPointer(
            Nested->PhysWindow, Frame, &region)) {
        /* Return the invalid failure that carries no error number. */
        return KSW_HVM_VMX_RESULT_FAIL_INVALID;
    }
    /* Refuse a region pointer the architecture does not accept. */
    if (!KswordARKHvmNestedIsRegionPointerValid(region)) {
        /* Return the invalid failure that carries no error number. */
        return KSW_HVM_VMX_RESULT_FAIL_INVALID;
    }
    /* Record the region L1 nominated without ever reading it. */
    Nested->VmxonRegion = region;
    /* Enter emulated L1 VMX operation. */
    Nested->Vmxon = TRUE;
    /* Start with no current vmcs12, exactly as the architecture requires. */
    Nested->VmcsCurrent = FALSE;
    Nested->CurrentVmcs = 0ULL;
    /* Publish that L1 now holds VMX operation. */
    Nested->State = KSWORD_ARK_HVM_NESTED_STATE_L1_VMXON;
    /* Return the complete success. */
    return KSW_HVM_VMX_RESULT_SUCCEED;
}

/*
 * Spill one vmcs12's fields into the region L1 gave it.
 *
 * Declared here because the spill happens inside the pool save, which runs
 * earlier in this file than the region code it belongs with.
 */
static VOID
KswordARKHvmNestedRegionStore(
    _Inout_ KSW_HVM_NESTED_VCPU* Nested,
    _In_ ULONGLONG RegionPhysical
    );

/*
 * Find the pool slot holding one vmcs12, or none.
 *
 * A slot is in use exactly when its PhysicalAddress is non-zero; that field is
 * the key and the occupancy bit at once, so the two cannot disagree.
 */
static KSW_HVM_VMCS12_STATE*
KswordARKHvmNestedPoolFind(
    _Inout_ KSW_HVM_NESTED_VCPU* Nested,
    _In_ ULONGLONG Pointer,
    _Out_opt_ ULONG* SlotIndex
    )
{
    ULONG index = 0UL;

    if (SlotIndex != NULL) { *SlotIndex = 0UL; }
    if (Nested->Vmcs12Pool == NULL || Pointer == 0ULL) {
        /* Report that no slot holds it. */
        return NULL;
    }
    for (index = 0UL; index < Nested->Vmcs12Pool->Count; ++index) {
        if (Nested->Vmcs12Pool->Slots[index].PhysicalAddress == Pointer) {
            if (SlotIndex != NULL) { *SlotIndex = index; }
            /* Return the slot that already holds this vmcs12. */
            return &Nested->Vmcs12Pool->Slots[index];
        }
    }
    /* Report that no slot holds it. */
    return NULL;
}

/*
 * Copy the loaded vmcs12 into the pool so another can take its place.
 *
 * Picks a free slot, else the coldest one.  Evicting is recorded rather than
 * hidden: an evicted vmcs12 comes back zeroed on its next VMPTRLD, which looks
 * to L1 exactly like the single-vmcs12 defect this pool replaces, so the count
 * is the only thing that can tell the two apart afterwards.
 */
static VOID
KswordARKHvmNestedPoolSave(
    _Inout_ KSW_HVM_NESTED_VCPU* Nested,
    _Inout_ KSW_HVM_RUNTIME* Runtime
    )
{
    KSW_HVM_VMCS12_POOL* pool = Nested->Vmcs12Pool;
    KSW_HVM_VMCS12_STATE* slot = NULL;
    ULONG index = 0UL;
    ULONG chosen = 0UL;
    LONG64 coldest = MAXLONG64;

    if (!Nested->VmcsCurrent || Nested->CurrentVmcs == 0ULL) {
        /* Return without saving what has no identity. */
        return;
    }
    /*
     * The region first, and unconditionally.
     *
     * It is the backing store, not an optimization, so it must not be skipped
     * because the cache in front of it happens to be missing.
     */
    KswordARKHvmNestedRegionStore(Nested, Nested->CurrentVmcs);
    if (pool == NULL || pool->Count == 0UL) {
        /* Return; the region above already holds these fields. */
        return;
    }
    slot = KswordARKHvmNestedPoolFind(Nested, Nested->CurrentVmcs, &chosen);
    if (slot == NULL) {
        for (index = 0UL; index < pool->Count; ++index) {
            /*
             * Claim a free slot atomically, because the pool is shared.
             *
             * Two processors that VMPTRLD two vmcs12 they have never seen can
             * reach this loop at the same time, and a plain store would give
             * both the same slot - one configuration silently landing on top
             * of the other.  The exchange makes the key and the claim the same
             * operation, so the loser simply moves on to the next slot.
             */
            if (InterlockedCompareExchange64(
                    (volatile LONG64*)&pool->Slots[index].PhysicalAddress,
                    (LONG64)Nested->CurrentVmcs,
                    0) == 0) {
                chosen = index;
                slot = &pool->Slots[index];
                break;
            }
            if (pool->Stamp[index] < coldest) {
                coldest = pool->Stamp[index];
                chosen = index;
            }
        }
        if (slot == NULL) {
            slot = &pool->Slots[chosen];
            /*
             * Record the eviction where it outlives the pool.
             *
             * The pool is freed at devirtualization; the fact that an L1 kept
             * more VMCSs than we hold is exactly the kind of thing that is
             * only noticed afterwards, from a report.
             */
            InterlockedIncrement(&Runtime->NestedVmcs12EvictionCount);
            /* And on this processor, where no other core's work is mixed in. */
            Nested->Vmcs12EvictionCount += 1UL;
        }
    }
    RtlCopyMemory(slot, &Nested->Vmcs12, sizeof(*slot));
    slot->PhysicalAddress = Nested->CurrentVmcs;
    pool->Stamp[chosen] = InterlockedIncrement64(&pool->Clock);
}

/*
 * Make one vmcs12 loaded, from the pool when it is known.
 *
 * Returns TRUE when its fields were restored and FALSE when this pointer has
 * never been seen, in which case the caller starts it empty - which is what
 * the architecture says a freshly VMCLEARed region holds anyway.
 */
static BOOLEAN
KswordARKHvmNestedPoolLoad(
    _Inout_ KSW_HVM_NESTED_VCPU* Nested,
    _In_ ULONGLONG Pointer
    )
{
    ULONG chosen = 0UL;
    KSW_HVM_VMCS12_STATE* slot =
        KswordARKHvmNestedPoolFind(Nested, Pointer, &chosen);

    if (slot == NULL) {
        /* Report that nothing was restored. */
        return FALSE;
    }
    RtlCopyMemory(&Nested->Vmcs12, slot, sizeof(Nested->Vmcs12));
    Nested->Vmcs12Pool->Stamp[chosen] =
        InterlockedIncrement64(&Nested->Vmcs12Pool->Clock);
    /* Report that this vmcs12's fields came back. */
    return TRUE;
}

/* Forget one vmcs12 entirely, as VMCLEAR requires. */
static VOID
KswordARKHvmNestedPoolDrop(
    _Inout_ KSW_HVM_NESTED_VCPU* Nested,
    _In_ ULONGLONG Pointer
    )
{
    ULONG chosen = 0UL;
    KSW_HVM_VMCS12_STATE* slot =
        KswordARKHvmNestedPoolFind(Nested, Pointer, &chosen);

    if (slot == NULL) {
        /* Return without dropping what was never held. */
        return;
    }
    RtlZeroMemory(slot, sizeof(*slot));
    Nested->Vmcs12Pool->Stamp[chosen] = 0;
}

/*
 * Keep the vmcs12 contents in the VMCS region L1 named, not only beside it.
 *
 * On real hardware a VMCS *is* the region: the fields live in those 4 KiB in
 * an opaque format, so everything that happens to the page happens to the
 * VMCS.  We intercept every VMWRITE and keep the fields in our own storage
 * instead, which is correct for every operation that names the VMCS - and
 * wrong for every operation that names the *page*.
 *
 * Measured need: VMware configures a VMCS at one physical address, and then
 * switches to a second one and launches it after writing nothing but guest
 * state into it - the same VMPTRLD instruction, the same operand slot, a
 * different pointer.  Its controls, host state and EPT pointer never arrive,
 * so L2 enters with our own identity EPT and a real-mode guest fetches from
 * the wrong memory.  A hypervisor may legitimately move a VMCS's backing page
 * and expect the contents to move with it; ours stayed behind because they
 * were keyed to an address rather than stored in the page.
 *
 * So the region becomes the backing store and the pool stays a cache: spilled
 * whenever the working copy stops being current, reloaded when a pointer is
 * new to the pool.  Written as (encoding, value) pairs rather than a fixed
 * field map, so no per-field capacity table has to stay in step with the
 * architecture, and only fields L1 actually set take space.
 *
 * The first eight bytes are never touched: they are L1's VMCS revision
 * identifier and its VMX-abort indicator, and both belong to L1.
 */
#define KSW_HVM_VMCS12_REGION_MAGIC 0x5657534BUL
#define KSW_HVM_VMCS12_REGION_HEADER 24ULL
#define KSW_HVM_VMCS12_REGION_ENTRIES \
    ((4096ULL - KSW_HVM_VMCS12_REGION_HEADER) / 16ULL)
#define KSW_HVM_VMCS12_REGION_FLAG_LAUNCHED 0x1ULL

/* Spill one vmcs12's fields into the region L1 gave it. */
static VOID
KswordARKHvmNestedRegionStore(
    _Inout_ KSW_HVM_NESTED_VCPU* Nested,
    _In_ ULONGLONG RegionPhysical
    )
{
    volatile VOID* mapped = NULL;
    volatile ULONGLONG* words = NULL;
    ULONG slot = 0UL;
    ULONGLONG written = 0ULL;

    if (Nested->PhysWindow == NULL || RegionPhysical == 0ULL) {
        /* Return without pretending a region we cannot reach was written. */
        return;
    }
    if (KswordARKHvmPhysWindowMap(
            Nested->PhysWindow,
            RegionPhysical,
            4096UL,
            &mapped) != KSW_HVM_PHYS_WINDOW_OK ||
        mapped == NULL) {
        Nested->RegionStoreFailCount += 1UL;
        /* Return; the pool still holds these fields for this processor. */
        return;
    }
    words = (volatile ULONGLONG*)mapped;
    for (slot = 0UL;
         slot < KSW_HVM_VMCS12_SLOT_COUNT &&
             written < KSW_HVM_VMCS12_REGION_ENTRIES;
         ++slot) {
        const ULONGLONG value = Nested->Vmcs12.Fields[slot];
        ULONG width = 0UL;
        ULONG type = 0UL;
        ULONG index = 0UL;
        ULONGLONG base = 0ULL;

        if (value == 0ULL) {
            continue;
        }
        /* Rebuild the encoding this slot stands for, without a second table. */
        width = slot / 512UL;
        type = (slot % 512UL) / 128UL;
        index = slot % 128UL;
        base = (KSW_HVM_VMCS12_REGION_HEADER + written * 16ULL) / 8ULL;
        words[base] =
            ((ULONGLONG)width << 13) |
            ((ULONGLONG)type << 10) |
            ((ULONGLONG)index << 1);
        words[base + 1ULL] = value;
        written += 1ULL;
    }
    /* Publish the header last, so a torn region never reads as complete. */
    words[2] =
        (Nested->Vmcs12.Launched != FALSE)
            ? KSW_HVM_VMCS12_REGION_FLAG_LAUNCHED
            : 0ULL;
    words[1] =
        (ULONGLONG)KSW_HVM_VMCS12_REGION_MAGIC |
        (written << 32);
    KswordARKHvmPhysWindowUnmap(Nested->PhysWindow);
    Nested->RegionStoreOkCount += 1UL;
    /*
     * Report only when the shape of what we store changes.
     *
     * A hypervisor spills the same VMCS hundreds of times a second and the
     * count is identical almost every time; what matters is the moment it
     * drops - that is a vmcs12 losing fields, and it is invisible in any
     * end-state readout because the end state only shows where it landed.
     * One row per change keeps the whole run inside the ring.
     */
    if ((ULONG)written != Nested->RegionStoreEntries ||
        RegionPhysical != Nested->RegionLastStorePhysical) {
        KSWORD_ARK_HVM_EVENT_ROW row;

        RtlZeroMemory(&row, sizeof(row));
        row.type = KSWORD_ARK_HVM_EVENT_TYPE_NESTED_VMX;
        row.guestPhysicalAddress = RegionPhysical;
        /* What we just stored, and what the previous store held. */
        row.guestLinearAddress = written;
        row.guestRip = (ULONGLONG)Nested->RegionStoreEntries;
        row.qualification = Nested->RegionLastStorePhysical;
        row.ruleId = 0xF2u;
        KswordARKHvmEventPublish(&row);
    }
    Nested->RegionStoreEntries = (ULONG)written;
    Nested->RegionLastStorePhysical = RegionPhysical;
}

/*
 * Restore one vmcs12's fields from the region, when the region carries ours.
 *
 * Returns TRUE only when the magic was found: a region holding anything else
 * is L1's own business, and reading fields out of it would be inventing them.
 */
static BOOLEAN
KswordARKHvmNestedRegionLoad(
    _Inout_ KSW_HVM_NESTED_VCPU* Nested,
    _In_ ULONGLONG RegionPhysical
    )
{
    volatile VOID* mapped = NULL;
    volatile ULONGLONG* words = NULL;
    ULONGLONG header = 0ULL;
    ULONGLONG count = 0ULL;
    ULONGLONG entry = 0ULL;
    BOOLEAN restored = FALSE;

    if (Nested->PhysWindow == NULL || RegionPhysical == 0ULL) {
        /* Report that nothing was restored. */
        return FALSE;
    }
    if (KswordARKHvmPhysWindowMap(
            Nested->PhysWindow,
            RegionPhysical,
            4096UL,
            &mapped) != KSW_HVM_PHYS_WINDOW_OK ||
        mapped == NULL) {
        /* Report that nothing was restored. */
        return FALSE;
    }
    words = (volatile ULONGLONG*)mapped;
    header = words[1];
    count = header >> 32;
    Nested->RegionLastLoadPhysical = RegionPhysical;
    Nested->RegionLastLoadHeader = header;
    if ((ULONG)(header & 0xFFFFFFFFULL) == KSW_HVM_VMCS12_REGION_MAGIC &&
        count <= KSW_HVM_VMCS12_REGION_ENTRIES) {
        KswordARKHvmNestedVmcsInitialize(
            &Nested->Vmcs12,
            &Nested->Vmcs02);
        /*
         * Make it current before writing into it.
         *
         * KswordARKHvmNestedVmcs12Write refuses every field unless the vmcs12
         * says it is current, and the initialize above clears exactly that
         * flag.  Without these two lines all thirty-eight restored fields were
         * refused one by one while this function still reported success - the
         * region carried the data, the magic matched, the count was right, and
         * the vmcs12 came up empty anyway.
         */
        Nested->Vmcs12.Current = TRUE;
        Nested->Vmcs12.PhysicalAddress = RegionPhysical;
        for (entry = 0ULL; entry < count; ++entry) {
            const ULONGLONG base =
                (KSW_HVM_VMCS12_REGION_HEADER + entry * 16ULL) / 8ULL;

            if (!NT_SUCCESS(KswordARKHvmNestedVmcs12Write(
                    &Nested->Vmcs12,
                    (ULONG)words[base],
                    words[base + 1ULL]))) {
                /* Count what the region held and we could not take back. */
                Nested->RegionLoadRefusedFields += 1UL;
            }
        }
        Nested->Vmcs12.Launched =
            ((words[2] & KSW_HVM_VMCS12_REGION_FLAG_LAUNCHED) != 0ULL)
                ? TRUE
                : FALSE;
        restored = TRUE;
    }
    KswordARKHvmPhysWindowUnmap(Nested->PhysWindow);
    if (restored) {
        Nested->RegionLoadOkCount += 1UL;
    } else {
        Nested->RegionLoadMissCount += 1UL;
    }
    /* Report whether this region carried a vmcs12 of ours. */
    return restored;
}

/*
 * Describe what L1 left in a VMCS region itself, once, when it is first seen.
 *
 * We never write a VMCS region: every VMWRITE is intercepted and kept in the
 * shadow copy, so on this machine a region holds only whatever L1 put there
 * directly.  That makes the region's contents a direct test of one specific
 * suspicion - that a hypervisor produced a second VMCS by copying the first
 * one's page rather than by writing its fields again, which is invisible to us
 * and leaves the new VMCS looking empty no matter how correct our caching is.
 *
 * Published rather than returned: the answer is wanted from a dump long after
 * the run, and it is one row per VMCS ever loaded, not one per instruction.
 */
static VOID
KswordARKHvmNestedPublishRegionShape(
    _Inout_ KSW_HVM_NESTED_VCPU* Nested,
    _In_ ULONGLONG RegionPhysical,
    _In_ ULONG ExitReason
    )
{
    volatile VOID* mapped = NULL;
    KSWORD_ARK_HVM_EVENT_ROW row;
    ULONGLONG fold = 0ULL;
    ULONGLONG head = 0ULL;
    ULONG nonZero = 0UL;
    ULONG index = 0UL;

    if (Nested->PhysWindow == NULL || RegionPhysical == 0ULL) {
        /* Return without inventing a shape for a region we cannot read. */
        return;
    }
    if (KswordARKHvmPhysWindowMap(
            Nested->PhysWindow,
            RegionPhysical,
            4096UL,
            &mapped) != KSW_HVM_PHYS_WINDOW_OK ||
        mapped == NULL) {
        /* Return rather than report a shape derived from no mapping. */
        return;
    }
    for (index = 0UL; index < 512UL; ++index) {
        const ULONGLONG word = ((volatile ULONGLONG*)mapped)[index];

        if (index == 0UL) { head = word; }
        if (word != 0ULL) { nonZero += 1UL; }
        fold = (fold * 1000003ULL) ^ word;
    }
    KswordARKHvmPhysWindowUnmap(Nested->PhysWindow);
    RtlZeroMemory(&row, sizeof(row));
    row.type = KSWORD_ARK_HVM_EVENT_TYPE_NESTED_VMX;
    row.exitReason = ExitReason;
    row.guestPhysicalAddress = RegionPhysical;
    /* The fold identifies the bytes; the count says whether there are any. */
    row.qualification = fold;
    row.guestLinearAddress = (ULONGLONG)nonZero;
    /* The first eight bytes are the revision identifier and abort indicator. */
    row.guestRip = head;
    /* Mark the row so a reader cannot mistake it for an instruction trace. */
    row.ruleId = 0xF1u;
    KswordARKHvmEventPublish(&row);
}

/* Dispatch VMCLEAR, VMPTRLD and VMPTRST against the current vmcs12. */
static UCHAR
KswordARKHvmNestedDispatchVmcsPointer(
    _Inout_ KSW_HVM_NESTED_VCPU* Nested,
    _Inout_ KSW_HVM_RUNTIME* Runtime,
    _In_ const struct _KSW_HVM_GPR_FRAME* Frame,
    _In_ ULONG ExitReason,
    _Out_ ULONG* InstructionError
    )
{
    KSW_HVM_VMX_OPERAND operand = { 0 };
    ULONGLONG pointer = 0ULL;

    *InstructionError = 0UL;
    /* Refuse every vmcs12 instruction outside L1 VMX operation. */
    if (!Nested->Vmxon) {
        /* Return the invalid failure that carries no error number. */
        return KSW_HVM_VMX_RESULT_FAIL_INVALID;
    }
    /* Decode the memory operand under the memory-only layout. */
    if (!NT_SUCCESS(KswordARKHvmNestedDecodeOperand(
            Frame,
            KSW_HVM_VMX_OPERAND_LAYOUT_MEMORY_ONLY,
            &operand))) {
        /* Return the invalid failure that carries no error number. */
        return KSW_HVM_VMX_RESULT_FAIL_INVALID;
    }
    /* VMPTRST writes the current pointer out and reads no pointer in. */
    if (ExitReason == KSW_VMX_EXIT_VMPTRST) {
        /* Publish the architectural empty value when none is current. */
        const ULONGLONG stored = Nested->VmcsCurrent
            ? Nested->CurrentVmcs
            : 0xFFFFFFFFFFFFFFFFULL;

        if (!NT_SUCCESS(KswordARKHvmNestedWriteGuestQword(
                Nested->PhysWindow,
                operand.LinearAddress,
                stored))) {
            /* Return the invalid failure that carries no error number. */
            return KSW_HVM_VMX_RESULT_FAIL_INVALID;
        }
        /* Return the complete success. */
        return KSW_HVM_VMX_RESULT_SUCCEED;
    }
    /* Read the vmcs12 pointer the operand addresses. */
    if (!NT_SUCCESS(KswordARKHvmNestedReadGuestQword(
            Nested->PhysWindow,
            operand.LinearAddress,
            &pointer))) {
        /* Return the invalid failure that carries no error number. */
        return KSW_HVM_VMX_RESULT_FAIL_INVALID;
    }
    /* Refuse a pointer the architecture does not accept. */
    if (!KswordARKHvmNestedIsRegionPointerValid(pointer)) {
        *InstructionError = (ExitReason == KSW_VMX_EXIT_VMCLEAR)
            ? KSW_VMX_ERROR_VMCLEAR_INVALID_ADDRESS
            : KSW_VMX_ERROR_VMPTRLD_INVALID_ADDRESS;
        /* Return the valid failure L1 can read an error number from. */
        return KSW_HVM_VMX_RESULT_FAIL_VALID;
    }
    /* Refuse aiming a vmcs12 instruction at the VMXON region. */
    if (pointer == Nested->VmxonRegion) {
        *InstructionError = (ExitReason == KSW_VMX_EXIT_VMCLEAR)
            ? KSW_VMX_ERROR_VMCLEAR_VMXON_POINTER
            : KSW_VMX_ERROR_VMPTRLD_VMXON_POINTER;
        /* Return the valid failure L1 can read an error number from. */
        return KSW_HVM_VMX_RESULT_FAIL_VALID;
    }
    /*
     * Record which vmcs12 this instruction names.
     *
     * The generic exit row carries the exit qualification, which for these
     * instructions is the operand's displacement - a stack offset, not the
     * pointer.  So the trace could show a hypervisor switching VMCSs a
     * thousand times without ever saying between which, and the field writes
     * either side of a switch could not be attributed to a VMCS at all.
     */
    {
        KSWORD_ARK_HVM_EVENT_ROW row;

        RtlZeroMemory(&row, sizeof(row));
        row.type = KSWORD_ARK_HVM_EVENT_TYPE_NESTED_VMX;
        row.exitReason = ExitReason;
        /* The vmcs12 named, and the one that was current before it. */
        row.guestPhysicalAddress = pointer;
        row.guestLinearAddress = Nested->VmcsCurrent
            ? Nested->CurrentVmcs
            : 0ULL;
        KswordARKHvmEventPublish(&row);
    }
    if (ExitReason == KSW_VMX_EXIT_VMCLEAR) {
        /*
         * VMCLEAR sets the launch state to clear.  It does **not** erase the
         * VMCS.
         *
         * This used to drop the pooled copy and zero the cached fields, on the
         * stated grounds that returning them afterwards would be "worse than
         * the zeroes it expects".  L1 expects no zeroes: Intel specifies that
         * VMCLEAR initializes the launch state and ensures the data is in
         * memory, and a hypervisor may VMCLEAR, VMPTRLD again and read back
         * every field it wrote.  Erasing them was our invention.
         *
         * It cost a real guest.  VMware VMCLEARs constantly - 574 times in one
         * two-second run - so its VMCS was emptied over and over, and the
         * VMLAUNCH it finally issued carried only the handful of fields written
         * since the last VMCLEAR: guest state and primary controls present, pin
         * and exit and entry controls, the secondary controls, the EPT pointer
         * and the bitmaps all gone.  L2 entered on a VMCS we had silently
         * hollowed out and triple-faulted on its first instruction, and nothing
         * anywhere reported a problem.
         *
         * The launch state is the only thing that changes, so a later VMRESUME
         * on this pointer is refused and a VMLAUNCH is required - which is the
         * distinction VMCLEAR exists to make.
         */
        {
            ULONG chosen = 0UL;
            KSW_HVM_VMCS12_STATE* pooled =
                KswordARKHvmNestedPoolFind(Nested, pointer, &chosen);

            if (pooled != NULL) {
                pooled->Launched = FALSE;
            }
        }
        /* Clear the current pointer only when VMCLEAR names it. */
        if (Nested->VmcsCurrent && Nested->CurrentVmcs == pointer) {
            /*
             * Spill the fields before letting go of them.
             *
             * The working copy is about to stop being current, and without this
             * save the only copy of what L1 configured would be the one we are
             * about to reinitialize - reintroducing the erasure through the
             * back door.
             */
            Nested->Vmcs12.Launched = FALSE;
            KswordARKHvmNestedPoolSave(Nested, Runtime);
            Nested->VmcsCurrent = FALSE;
            Nested->CurrentVmcs = 0ULL;
            /* Start the working copy empty; the pool holds the real contents. */
            KswordARKHvmNestedVmcsInitialize(
                &Nested->Vmcs12,
                &Nested->Vmcs02);
            /* Fall back to holding VMX operation with no current VMCS. */
            Nested->State = KSWORD_ARK_HVM_NESTED_STATE_L1_VMXON;
        }
        /* Return the complete success. */
        return KSW_HVM_VMX_RESULT_SUCCEED;
    }
    /*
     * VMPTRLD makes one vmcs12 current, and the previous one keeps its fields.
     *
     * This used to reinitialize on every pointer change, because exactly one
     * vmcs12 was modelled.  That was correct given the model - keeping the
     * previous VMCS's fields would have let them answer VMREADs issued against
     * a different VMCS - and wrong as a model: a hypervisor keeps several and
     * switches between them constantly, so it would read back zeroes for
     * fields it had written moments earlier.
     *
     * Now the outgoing one is spilled to the pool and the incoming one
     * restored from it.  A pointer never seen before starts empty, which is
     * what the architecture says a freshly VMCLEARed region holds.
     */
    if (!Nested->VmcsCurrent || Nested->CurrentVmcs != pointer) {
        const ULONGLONG outgoing = Nested->VmcsCurrent
            ? Nested->CurrentVmcs
            : 0ULL;

        KswordARKHvmNestedPoolSave(Nested, Runtime);
        if (!KswordARKHvmNestedPoolLoad(Nested, pointer)) {
            /*
             * First sight of this VMCS: record both regions before deciding
             * anything, and record the one being left at the same instant so
             * the two can be compared rather than described separately.
             */
            KswordARKHvmNestedPublishRegionShape(Nested, pointer, ExitReason);
            KswordARKHvmNestedPublishRegionShape(Nested, outgoing, ExitReason);
            /*
             * The pool has never seen this pointer, which is not the same as
             * the VMCS being empty: L1 may have moved it to a new page, and
             * the contents travel in the page.  Ask the region before falling
             * back to the architectural "a fresh region holds nothing".
             */
            if (!KswordARKHvmNestedRegionLoad(Nested, pointer)) {
                KswordARKHvmNestedVmcsInitialize(
                    &Nested->Vmcs12,
                    &Nested->Vmcs02);
            }
        }
    }
    Nested->CurrentVmcs = pointer;
    Nested->VmcsCurrent = TRUE;
    Nested->Vmcs12.Current = TRUE;
    Nested->Vmcs12.PhysicalAddress = pointer;
    /* Publish that one vmcs12 is current. */
    Nested->State = KSWORD_ARK_HVM_NESTED_STATE_VMCS12_CURRENT;
    /* Return the complete success. */
    return KSW_HVM_VMX_RESULT_SUCCEED;
}

/*
 * Report whether one linear address is canonical under 4-level paging.
 *
 * Bits 63:47 must all match bit 47.  Five-level paging widens this to 57 bits,
 * and an address canonical only under that rule is rejected here - which agrees
 * with the rest of the nested path: the guest page walk in hvm_nested_decode.c
 * refuses LA57 outright rather than walk a structure format it does not
 * implement.  A guest using five-level paging is refused consistently rather
 * than served inconsistently.
 */
static BOOLEAN
KswordARKHvmNestedIsCanonicalAddress(
    _In_ ULONGLONG LinearAddress
    )
{
    const ULONGLONG high = LinearAddress >> 47;

    /* Report canonical for the low half and for the sign-extended high half. */
    return (high == 0ULL || high == 0x1FFFFULL) ? TRUE : FALSE;
}

/*
 * Dispatch INVEPT and INVVPID.
 *
 * These used to be refused, and the stated reason was that precise
 * invalidation needs a reverse map from L1 physical back to every L2 page
 * composed through it.  That reason stopped applying once the shadow chose to
 * drop its whole hierarchy instead of individual entries - there is no reverse
 * map to build, and the work is already done by the time we answer.
 *
 * Refusing while having done the work was the worst of both: L1 issues INVEPT
 * precisely to announce that a mapping it installed is now stale, and that
 * announcement is the *only* channel it has, because our shadow is otherwise
 * dropped only when the EPT pointer itself changes.  Telling L1 the flush
 * failed leaves it believing the stale mapping is still live, or treating the
 * failure as fatal - neither of which reflects what happened.
 */
static UCHAR
KswordARKHvmNestedDispatchInvalidate(
    _Inout_ KSW_HVM_NESTED_VCPU* Nested,
    _In_ const struct _KSW_HVM_GPR_FRAME* Frame,
    _In_ ULONG ExitReason,
    _Out_ ULONG* InstructionError
    )
{
    KSW_HVM_VMX_OPERAND operand = { 0 };
    ULONGLONG type = 0ULL;
    ULONGLONG descriptor = 0ULL;
    ULONGLONG linearAddress = 0ULL;

    *InstructionError = 0UL;
    /* Refuse invalidation outside L1 VMX operation. */
    if (!Nested->Vmxon) {
        /* Return the invalid failure that carries no error number. */
        return KSW_HVM_VMX_RESULT_FAIL_INVALID;
    }
    /* Decode under the layout that adds a type register to a memory operand. */
    if (!NT_SUCCESS(KswordARKHvmNestedDecodeOperand(
            Frame,
            KSW_HVM_VMX_OPERAND_LAYOUT_INVALIDATION,
            &operand))) {
        /* Return the invalid failure that carries no error number. */
        return KSW_HVM_VMX_RESULT_FAIL_INVALID;
    }
    if (KswordARKHvmNestedReadGpr(
            Frame,
            operand.SecondaryRegister,
            &type) != 0U) {
        /* Return the invalid failure that carries no error number. */
        return KSW_HVM_VMX_RESULT_FAIL_INVALID;
    }
    /*
     * Read the first eight bytes of the descriptor.
     *
     * That is the EPT pointer for INVEPT and the VPID plus reserved bits for
     * INVVPID.
     */
    if (!NT_SUCCESS(KswordARKHvmNestedReadGuestQword(
            Nested->PhysWindow,
            operand.LinearAddress,
            &descriptor))) {
        /* Return the invalid failure that carries no error number. */
        return KSW_HVM_VMX_RESULT_FAIL_INVALID;
    }
    /*
     * The second eight bytes carry a linear address, and only one form reads
     * them: INVVPID's individual-address type.
     *
     * Fetched conditionally rather than always, because every fetch is a walk
     * of the guest's page tables through the physical window, and a guest
     * hypervisor issues this instruction in the hundreds while it starts.
     */
    if (ExitReason == KSW_VMX_EXIT_INVVPID && type == 0ULL) {
        if (!NT_SUCCESS(KswordARKHvmNestedReadGuestQword(
                Nested->PhysWindow,
                operand.LinearAddress + 8ULL,
                &linearAddress))) {
            /* Return the invalid failure that carries no error number. */
            return KSW_HVM_VMX_RESULT_FAIL_INVALID;
        }
    }
    /*
     * From here the two instructions stop being the same instruction.
     *
     * They share an encoding, an operand layout and this dispatch, and they
     * invalidate two unrelated things: INVEPT retires EPT-derived translations,
     * INVVPID retires linear ones.  Serving both with one action was safe only
     * while INVVPID was refused outright.
     */
    if (ExitReason == KSW_VMX_EXIT_INVVPID) {
        /*
         * Accept the three types we advertise, and only those.
         *
         * Type 3 (single-context, retaining globals) is not advertised in
         * KSWORD_ARK_HVM_VMX_EPT_CAP_ALLOWED, so accepting it here would be a
         * promise the capability MSR does not make.
         */
        if (type > 2ULL) {
            *InstructionError = KSW_VMX_ERROR_INVALID_INVALIDATION_OPERAND;
            /* Return the valid failure L1 can read an error number from. */
            return KSW_HVM_VMX_RESULT_FAIL_VALID;
        }
        /*
         * The architectural operand checks, which cost a shift and a compare.
         *
         * A VPID of zero is invalid for the two types that name a context, and
         * an individual-address invalidation must name a canonical address.
         * Accepting either would make this instruction succeed here and fail on
         * real hardware - a difference L1 has no way to see coming.
         */
        {
            const ULONGLONG vpid = descriptor & 0xFFFFULL;

            if (vpid == 0ULL && type != 2ULL) {
                *InstructionError = KSW_VMX_ERROR_INVALID_INVALIDATION_OPERAND;
                /* Return the valid failure L1 can read an error number from. */
                return KSW_HVM_VMX_RESULT_FAIL_VALID;
            }
            if (type == 0ULL &&
                !KswordARKHvmNestedIsCanonicalAddress(linearAddress)) {
                *InstructionError = KSW_VMX_ERROR_INVALID_INVALIDATION_OPERAND;
                /* Return the valid failure L1 can read an error number from. */
                return KSW_HVM_VMX_RESULT_FAIL_VALID;
            }
        }
        /*
         * Then do nothing, because nothing is what it takes.
         *
         * We never enable VPID, so L2 runs under VPID 0000H, and the processor
         * invalidates linear and combined mappings for VPID 0000H on **every**
         * VM entry and VM exit.  Whatever translation L1 is retiring is already
         * gone before L2 next executes an instruction.
         *
         * The tempting wrong answer is to drop the shadow EPT the way INVEPT
         * does.  It would be a no-op for correctness and a disaster for cost:
         * this instruction arrives in the hundreds while a guest hypervisor
         * starts, and each drop rebuilds a hierarchy fault by fault.
         */
        Nested->InvvpidServedCount += 1ULL;
        /* Return the complete success. */
        return KSW_HVM_VMX_RESULT_SUCCEED;
    }
    /*
     * INVEPT accepts only the two context-wide types.
     *
     * There is no individual-address form, so the bound is architectural rather
     * than a limit of ours.
     */
    if (type != 1ULL && type != 2ULL) {
        *InstructionError = KSW_VMX_ERROR_INVALID_INVALIDATION_OPERAND;
        /* Return the valid failure L1 can read an error number from. */
        return KSW_HVM_VMX_RESULT_FAIL_VALID;
    }
    /* INVEPT's single-context type must name an EPT pointer we could use. */
    if (type == 1ULL &&
        (descriptor & 0x000FFFFFFFFFF000ULL) == 0ULL) {
        *InstructionError = KSW_VMX_ERROR_INVALID_INVALIDATION_OPERAND;
        /* Return the valid failure L1 can read an error number from. */
        return KSW_HVM_VMX_RESULT_FAIL_VALID;
    }
    /*
     * Drop everything regardless of which context was named.
     *
     * Coarser than asked for, and deliberately so: over-invalidating costs
     * refills, under-invalidating leaves L2 running on a translation L1 has
     * retired - with no symptom until the memory underneath is reused.
     */
    KswordARKHvmNestedEptInvalidate(&Nested->ShadowEpt);
    /* Return the complete success. */
    return KSW_HVM_VMX_RESULT_SUCCEED;
}

/* Dispatch VMREAD and VMWRITE against the bounded vmcs12 field cache. */
static UCHAR
KswordARKHvmNestedDispatchVmcsField(
    _Inout_ KSW_HVM_NESTED_VCPU* Nested,
    _Inout_ struct _KSW_HVM_GPR_FRAME* Frame,
    _In_ ULONG ExitReason,
    _Out_ ULONG* InstructionError
    )
{
    KSW_HVM_VMX_OPERAND operand = { 0 };
    ULONGLONG encoding = 0ULL;
    ULONGLONG value = 0ULL;
    NTSTATUS writeStatus = STATUS_SUCCESS;

    *InstructionError = 0UL;
    /* Refuse every field access without a current vmcs12. */
    if (!Nested->Vmxon || !Nested->VmcsCurrent) {
        /* Return the invalid failure that carries no error number. */
        return KSW_HVM_VMX_RESULT_FAIL_INVALID;
    }
    /* Decode under the layout that spends bits on both register operands. */
    if (!NT_SUCCESS(KswordARKHvmNestedDecodeOperand(
            Frame,
            KSW_HVM_VMX_OPERAND_LAYOUT_VMREAD_WRITE,
            &operand))) {
        /* Return the invalid failure that carries no error number. */
        return KSW_HVM_VMX_RESULT_FAIL_INVALID;
    }
    /*
     * The field encoding is the second register operand for both instructions.
     *
     * VMREAD writes the field's value into the first operand and VMWRITE reads
     * the new value out of it, so the two are mirror images - but the register
     * naming the *field* is the same one in both, which is why one decode
     * serves both.
     */
    if (KswordARKHvmNestedReadGpr(
            Frame,
            operand.SecondaryRegister,
            &encoding) != 0U) {
        /* Return the invalid failure that carries no error number. */
        return KSW_HVM_VMX_RESULT_FAIL_INVALID;
    }
    if (ExitReason == KSW_VMX_EXIT_VMREAD) {
        /* Refuse a field this bounded cache never accepted. */
        if (!NT_SUCCESS(KswordARKHvmNestedVmcs12Read(
                &Nested->Vmcs12,
                (ULONG)encoding,
                &value))) {
            *InstructionError = KSW_VMX_ERROR_UNSUPPORTED_COMPONENT;
            /* Return the valid failure L1 can read an error number from. */
            return KSW_HVM_VMX_RESULT_FAIL_VALID;
        }
        /* Deliver the value to whichever destination form was decoded. */
        if (operand.IsRegister) {
            if (KswordARKHvmNestedWriteGpr(
                    Frame,
                    operand.PrimaryRegister,
                    value) != 0U) {
                /* Return the invalid failure that carries no error number. */
                return KSW_HVM_VMX_RESULT_FAIL_INVALID;
            }
        } else if (!NT_SUCCESS(KswordARKHvmNestedWriteGuestQword(
                Nested->PhysWindow,
                operand.LinearAddress,
                value))) {
            /* Return the invalid failure that carries no error number. */
            return KSW_HVM_VMX_RESULT_FAIL_INVALID;
        }
        /* Return the complete success. */
        return KSW_HVM_VMX_RESULT_SUCCEED;
    }
    /* Refuse writing a component the architecture marks read-only. */
    if (((encoding >> 10) & 0x3ULL) == KSW_VMCS_ENCODING_TYPE_READ_ONLY) {
        *InstructionError = KSW_VMX_ERROR_VMWRITE_READ_ONLY;
        /* Return the valid failure L1 can read an error number from. */
        return KSW_HVM_VMX_RESULT_FAIL_VALID;
    }
    /* Collect the new value from whichever source form was decoded. */
    if (operand.IsRegister) {
        if (KswordARKHvmNestedReadGpr(
                Frame,
                operand.PrimaryRegister,
                &value) != 0U) {
            /* Return the invalid failure that carries no error number. */
            return KSW_HVM_VMX_RESULT_FAIL_INVALID;
        }
    } else if (!NT_SUCCESS(KswordARKHvmNestedReadGuestQword(
            Nested->PhysWindow,
            operand.LinearAddress,
            &value))) {
        /* Return the invalid failure that carries no error number. */
        return KSW_HVM_VMX_RESULT_FAIL_INVALID;
    }
    /*
     * A full cache reports the architectural unsupported-component error.
     *
     * That is not a euphemism.  The cache bounds which components this vmcs12
     * actually supports, and telling L1 the component is unsupported is the
     * one answer it already knows how to act on - far better than succeeding
     * and silently discarding a control it will later rely on.
     */
    writeStatus = KswordARKHvmNestedVmcs12Write(
        &Nested->Vmcs12,
        (ULONG)encoding,
        value);
    /*
     * Record what L1 configured, when someone asked for the trace.
     *
     * The question this answers cannot be answered any other way: a field that
     * reads back as zero at entry is indistinguishable from a field L1 never
     * wrote, because the cache has no "never written" state - and the two lead
     * to opposite conclusions about whose defect it is.  The encoding and the
     * value go into the evidence ring, which already retains nested-VMX events
     * as one of its four classes and which hvm_ctl already reads.
     *
     * Published unconditionally, and deliberately **not** behind the routine-exit
     * trace switch.  That was the first attempt and it defeated itself: turning
     * that switch on writes every ordinary exit to the ring, and one idle guest
     * produced 309,391 HLT rows that evicted the entire nested trace within the
     * same run.  Nested VMX is already one of the four classes the ring exists
     * to retain; a hypervisor's VMWRITEs number in the hundreds while it starts,
     * against tens of thousands of routine exits per second.
     */
    {
        KSWORD_ARK_HVM_EVENT_ROW row;

        RtlZeroMemory(&row, sizeof(row));
        row.type = KSWORD_ARK_HVM_EVENT_TYPE_NESTED_VMX;
        row.exitReason = ExitReason;
        /* The field L1 named, and the value it put there. */
        row.qualification = encoding;
        row.guestPhysicalAddress = value;
        /*
         * Which vmcs12 this write landed in.
         *
         * Without it the trace says a field was written and cannot say to
         * what, and "L1 never wrote the control" and "L1 wrote it to a
         * different VMCS than the one it launched" look identical - while
         * pointing at completely different defects.
         */
        row.guestLinearAddress = Nested->CurrentVmcs;
        /*
         * Whether the field was actually kept.
         *
         * Published for refusals too, which is the change that matters: a
         * refused VMWRITE used to return before this point and leave nothing
         * behind, so "L1 never configured the control" and "L1 configured it
         * and we threw it away" produced identical traces.  One of those is
         * L1's problem and the other is ours.
         */
        row.status = writeStatus;
        {
            SIZE_T rip = 0U;

            if (KswordARKHvmVmcsFieldLoad(KSW_VMCS_GUEST_RIP, &rip) == 0) {
                row.guestRip = (ULONGLONG)rip;
            }
        }
        KswordARKHvmEventPublish(&row);
    }
    if (!NT_SUCCESS(writeStatus)) {
        *InstructionError = KSW_VMX_ERROR_UNSUPPORTED_COMPONENT;
        /* Return the valid failure L1 can read an error number from. */
        return KSW_HVM_VMX_RESULT_FAIL_VALID;
    }
    /* Return the complete success. */
    return KSW_HVM_VMX_RESULT_SUCCEED;
}

BOOLEAN
KswordARKHvmNestedHandleExit(
    _Inout_ struct _KSW_HVM_RESIDENT_VCPU* Vcpu,
    _Inout_ struct _KSW_HVM_GPR_FRAME* Frame,
    _In_ ULONG ExitReason,
    _In_ ULONG InstructionLength
    )
{
    KSW_HVM_RUNTIME* Runtime = (Vcpu != NULL) ? Vcpu->Runtime : NULL;
    KSW_HVM_NESTED_VCPU* Nested = (Vcpu != NULL) ? &Vcpu->Nested : NULL;
    UCHAR instructionResult = 2U;
    ULONG instructionError = 0UL;

    /* Reject invalid fixed state or disabled nested dispatch. */
    if (Runtime == NULL ||
        Nested == NULL ||
        Frame == NULL ||
        !Nested->Enabled) {
        /* Report that the exit was not handled. */
        return FALSE;
    }
    /* Count every bounded nested instruction dispatch. */
    Nested->InstructionCount += 1ULL;
    /* VMXOFF has no memory operand and can complete from local state. */
    if (ExitReason == KSW_VMX_EXIT_VMXOFF) {
        /* Reject VMXOFF before a valid L1 VMXON state. */
        if (!Nested->Vmxon) {
            /* Select VMfailInvalid for an absent VMX operation. */
            instructionResult = 2U;
        } else {
            /*
             * Spill the loaded vmcs12 before VMX operation ends.
             *
             * VMXOFF ends VMX operation; it does not modify any VMCS.  L1 may
             * VMXON again, VMPTRLD the same region and read back every field
             * it wrote - including fields written since the last VMCLEAR,
             * which until now lived only in the working copy and were dropped
             * here without a trace.
             *
             * Measured cost: VMware leaves and re-enters VMX operation on
             * every world switch - 126 times in one retained window - and the
             * round that configures a VMCS ends this way rather than with a
             * VMCLEAR.  Its pin, exit, entry and secondary controls and its
             * EPT pointer were therefore never in the vmcs12 it finally
             * launched, and the entry failed on guest state built from our
             * own controls merged over nothing.
             */
            KswordARKHvmNestedPoolSave(Nested, Runtime);
            /* Leave the local L1 VMX operation. */
            Nested->Vmxon = FALSE;
            /* Clear the current vmcs12 pointer. */
            Nested->VmcsCurrent = FALSE;
            /*
             * And forget which one it was, so nothing can alias it later.
             *
             * A stale pointer here is not inert: the next VMPTRLD compares
             * against it, and a pointer that happens to match would skip the
             * restore and run on whatever the working copy holds.
             */
            Nested->CurrentVmcs = 0ULL;
            /* Start the working copy empty; the pool holds the real contents. */
            KswordARKHvmNestedVmcsInitialize(
                &Nested->Vmcs12,
                &Nested->Vmcs02);
            /* Clear any prior L2 launch attempt. */
            Nested->L2LaunchAttempted = FALSE;
            /*
             * Clear the no-progress latch: VMXOFF is L1 starting over.
             *
             * Not cleared anywhere earlier on purpose.  The latch exists to
             * stop an L1 from resuming straight back into the loop it was just
             * pulled out of, so anything short of abandoning VMX operation
             * entirely leaves it armed.  What it recorded is kept - the
             * diagnosis outlives the latch, because the reader of that
             * diagnosis always arrives after L1 has given up.
             */
            Nested->L2FuseTripped = FALSE;
            Nested->L2NoProgressCount = 0UL;
            /* Publish dispatch-ready state after VMXOFF. */
            Nested->State =
                KSWORD_ARK_HVM_NESTED_STATE_DISPATCH_READY;
            /* Select VMX instruction success. */
            instructionResult = 0U;
        }
    /* VMLAUNCH and VMRESUME both attempt a real L2 entry. */
    } else if (ExitReason == KSW_VMX_EXIT_VMLAUNCH ||
               ExitReason == KSW_VMX_EXIT_VMRESUME) {
        /* Report invalid current state before any vmcs12 pointer exists. */
        if (!Nested->Vmxon || !Nested->VmcsCurrent) {
            /* Select VMfailInvalid for an absent current VMCS. */
            instructionResult = 2U;
        } else {
            /*
             * Advance L1's RIP before entering, not after.
             *
             * If entry succeeds nothing here runs again: the processor leaves
             * for L2 and the next thing on this processor is the exit stub.
             * The RIP L1 resumes at, whenever it eventually does, has to have
             * been recorded already - and the entry path captures it from the
             * VMCS, which still holds the pre-advance value at this point.
             */
            const ULONG entryError = KswordARKHvmNestedL2Enter(
                Vcpu,
                Frame,
                (BOOLEAN)(ExitReason == KSW_VMX_EXIT_VMRESUME));

            /* Reaching here at all means the entry did not happen. */
            InterlockedIncrement(
                &Runtime->NestedL2LaunchRefusedCount);
            /*
             * Publish which refusal it was, beside the count that says how many.
             *
             * Seven conditions inside the entry path return the same
             * architectural error, because the architecture has exactly one
             * number for "invalid control field".  The count alone therefore
             * says a launch was refused and nothing about why - and the site is
             * the only part anyone can act on.  Published from here rather than
             * scanned from the per-processor state later, because this is the
             * one place that already knows an entry was refused.
             */
            InterlockedExchange(
                &Runtime->NestedLastRefusalSite,
                (LONG)Nested->L2LastRefusalSite);
            Nested->L2LaunchAttempted = TRUE;
            Nested->State =
                KSWORD_ARK_HVM_NESTED_STATE_L2_PARTIAL;
            instructionResult = 1U;
            instructionError = entryError;
        }
    /* VMXON establishes L1 VMX operation from a decoded region pointer. */
    } else if (ExitReason == KSW_VMX_EXIT_VMXON) {
        instructionResult = KswordARKHvmNestedDispatchVmxon(
            Nested,
            Frame,
            &instructionError);
    /* The vmcs12 pointer instructions share one decoded memory operand. */
    } else if (ExitReason == KSW_VMX_EXIT_VMCLEAR ||
               ExitReason == KSW_VMX_EXIT_VMPTRLD ||
               ExitReason == KSW_VMX_EXIT_VMPTRST) {
        instructionResult = KswordARKHvmNestedDispatchVmcsPointer(
            Nested,
            Runtime,
            Frame,
            ExitReason,
            &instructionError);
    /* VMREAD and VMWRITE address the bounded vmcs12 field cache. */
    } else if (ExitReason == KSW_VMX_EXIT_VMREAD ||
               ExitReason == KSW_VMX_EXIT_VMWRITE) {
        instructionResult = KswordARKHvmNestedDispatchVmcsField(
            Nested,
            Frame,
            ExitReason,
            &instructionError);
    /* Invalidation drops every composed mapping and reports success. */
    } else if (ExitReason == KSW_VMX_EXIT_INVEPT ||
               ExitReason == KSW_VMX_EXIT_INVVPID) {
        instructionResult = KswordARKHvmNestedDispatchInvalidate(
            Nested,
            Frame,
            ExitReason,
            &instructionError);
    } else {
        /* Report that this exit reason does not belong to nested dispatch. */
        return FALSE;
    }
    /* Preserve the last architectural VM-instruction error. */
    Nested->LastInstructionError = instructionError;
    /*
     * VMfailValid also stores the error number where L1 can VMREAD it.
     *
     * Setting ZF and keeping the number only in our own records satisfies half
     * the contract: L1 sees that something failed and has no way to learn
     * what.  Every hypervisor reads this field after a failed VMLAUNCH, so
     * without it the failure is reported as error zero - which reads as "no
     * error" and sends the reader looking in the wrong place.
     */
    if (instructionResult == KSW_HVM_VMX_RESULT_FAIL_VALID &&
        Nested->VmcsCurrent) {
        (void)KswordARKHvmNestedVmcs12Write(
            &Nested->Vmcs12,
            0x4400UL,
            (ULONGLONG)instructionError);
    }
    /* Publish the current nested state to the runtime snapshot. */
    InterlockedExchange(
        (volatile LONG*)&Runtime->NestedState,
        (LONG)Nested->State);
    /* Publish VMX result flags before advancing guest RIP. */
    if (!KswordARKHvmNestedSetInstructionResult(
            instructionResult)) {
        /* Report a fatal VMCS write failure. */
        return FALSE;
    }
    /* Advance past the fully decoded VMX instruction. */
    return KswordARKHvmNestedAdvanceRip(
        InstructionLength);
}

#else

VOID
KswordARKHvmNestedInitializeVcpu(
    _Out_ KSW_HVM_NESTED_VCPU* Nested,
    _In_ BOOLEAN Enabled,
    _In_ ULONGLONG L0EptPointer
    )
{
    /* Keep non-x64 builds explicit and warning-free. */
    UNREFERENCED_PARAMETER(Enabled);
    /* Keep non-x64 builds explicit and warning-free. */
    UNREFERENCED_PARAMETER(L0EptPointer);
    /* Clear a supplied state record before returning. */
    if (Nested != NULL) {
        /* Initialize the complete unsupported nested state. */
        RtlZeroMemory(Nested, sizeof(*Nested));
    }
}

NTSTATUS
KswordARKHvmNestedValidate(
    _Inout_ KSW_HVM_RUNTIME* Runtime
    )
{
    /* Keep non-x64 builds explicit and warning-free. */
    UNREFERENCED_PARAMETER(Runtime);
    /* Return the explicit architecture boundary. */
    return STATUS_NOT_SUPPORTED;
}

BOOLEAN
KswordARKHvmNestedHandleExit(
    _Inout_ struct _KSW_HVM_RESIDENT_VCPU* Vcpu,
    _Inout_ struct _KSW_HVM_GPR_FRAME* Frame,
    _In_ ULONG ExitReason,
    _In_ ULONG InstructionLength
    )
{
    KSW_HVM_RUNTIME* Runtime = (Vcpu != NULL) ? Vcpu->Runtime : NULL;
    KSW_HVM_NESTED_VCPU* Nested = (Vcpu != NULL) ? &Vcpu->Nested : NULL;
    /* Keep non-x64 builds explicit and warning-free. */
    UNREFERENCED_PARAMETER(Runtime);
    /* Keep non-x64 builds explicit and warning-free. */
    UNREFERENCED_PARAMETER(Nested);
    /* Keep non-x64 builds explicit and warning-free. */
    UNREFERENCED_PARAMETER(Frame);
    /* Keep non-x64 builds explicit and warning-free. */
    UNREFERENCED_PARAMETER(ExitReason);
    /* Keep non-x64 builds explicit and warning-free. */
    UNREFERENCED_PARAMETER(InstructionLength);
    /* Report that no VMX instruction exit was handled. */
    return FALSE;
}

#endif
