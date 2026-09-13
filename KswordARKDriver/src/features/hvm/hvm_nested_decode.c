/*++

Module Name:

    hvm_nested_decode.c

Abstract:

    Implements VM-exit instruction-information decoding for nested VMX
    instruction dispatch.

Environment:

    Kernel-mode Driver Framework.

--*/

#include "hvm_nested_decode.h"
#include "hvm_exit.h"
/* VMCS access goes through the seam in hvm_vmcs.h, never the raw intrinsic. */
#include "hvm_vmcs.h"
/*
 * The guest accessors below walk the guest's own page tables and read the
 * entries as physical memory.  That is what this window is for, and it is the
 * only mapping primitive in the tree documented as VM-exit safe.
 */
#include "hvm_phys_window.h"

#if defined(_M_AMD64)

/* Name the VM-exit instruction-information field. */
#define KSW_VMCS_VMX_INSTRUCTION_INFORMATION 0x440EUL
/* Name the exit-qualification field, which carries the displacement. */
#define KSW_VMCS_EXIT_QUALIFICATION 0x6400UL
/* Name the VMCS guest stack pointer, which is not in the register frame. */
#define KSW_VMCS_GUEST_RSP 0x681CUL

/*
 * Name the guest segment base fields.
 *
 * They are consecutive at a stride of two starting from ES, so the segment
 * register number out of the instruction-information field indexes them
 * directly.  The stride is asserted below rather than assumed.
 */
#define KSW_VMCS_GUEST_ES_BASE 0x6806UL
#define KSW_VMCS_GUEST_SEGMENT_BASE_STRIDE 2UL
/* Name the highest segment register number Intel encodes (GS). */
#define KSW_VMCS_GUEST_SEGMENT_MAX 5UL

/* Name the architectural register number that denotes RSP. */
#define KSW_HVM_GPR_NUMBER_RSP 4UL
/* Name the count of architectural general-purpose registers. */
#define KSW_HVM_GPR_COUNT 16UL

UCHAR
KswordARKHvmNestedReadGpr(
    _In_ const struct _KSW_HVM_GPR_FRAME* Frame,
    _In_ ULONG RegisterNumber,
    _Out_ ULONGLONG* Value
    )
{
    /* Reject an incomplete caller contract before touching any storage. */
    if (Frame == NULL || Value == NULL) {
        /* Return the explicit contract failure. */
        return 1U;
    }
    *Value = 0ULL;
    /* Reject register numbers Intel does not encode. */
    if (RegisterNumber >= KSW_HVM_GPR_COUNT) {
        /* Return the explicit range failure. */
        return 1U;
    }
    /*
     * RSP is the hole in the frame, and it is a silent one.
     *
     * Intel numbers registers RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8..R15.
     * The VM-exit stub does not push RSP - it cannot, because the value that
     * matters is the guest's, and by the time the stub runs the stack pointer
     * is the host's.  The guest value lives in the VMCS instead.
     *
     * So the frame holds fifteen registers where Intel numbers sixteen, and
     * every number above four is shifted by one relative to a naive index.
     * Treating the frame as an array would hand back RBP for RSP, RSI for RBP,
     * and so on for the rest - wrong values, no error, for every instruction
     * whose operand used one of those registers as a base or index.
     */
    if (RegisterNumber == KSW_HVM_GPR_NUMBER_RSP) {
        SIZE_T guestRsp = 0U;

        /* Read the guest stack pointer from the only place that holds it. */
        if (KswordARKHvmVmcsFieldLoad(KSW_VMCS_GUEST_RSP, &guestRsp) != 0U) {
            /* Return the explicit VMCS read failure. */
            return 1U;
        }
        *Value = (ULONGLONG)guestRsp;
        /* Return the complete guest stack pointer. */
        return 0U;
    }
    switch (RegisterNumber) {
    case 0UL:  *Value = Frame->Rax; break;
    case 1UL:  *Value = Frame->Rcx; break;
    case 2UL:  *Value = Frame->Rdx; break;
    case 3UL:  *Value = Frame->Rbx; break;
    /* Case four is RSP and was handled above. */
    case 5UL:  *Value = Frame->Rbp; break;
    case 6UL:  *Value = Frame->Rsi; break;
    case 7UL:  *Value = Frame->Rdi; break;
    case 8UL:  *Value = Frame->R8;  break;
    case 9UL:  *Value = Frame->R9;  break;
    case 10UL: *Value = Frame->R10; break;
    case 11UL: *Value = Frame->R11; break;
    case 12UL: *Value = Frame->R12; break;
    case 13UL: *Value = Frame->R13; break;
    case 14UL: *Value = Frame->R14; break;
    case 15UL: *Value = Frame->R15; break;
    default:
        /* Return the explicit unreachable-number failure. */
        return 1U;
    }
    /* Return the complete register value. */
    return 0U;
}

UCHAR
KswordARKHvmNestedWriteGpr(
    _Inout_ struct _KSW_HVM_GPR_FRAME* Frame,
    _In_ ULONG RegisterNumber,
    _In_ ULONGLONG Value
    )
{
    /* Reject an incomplete caller contract before touching any storage. */
    if (Frame == NULL || RegisterNumber >= KSW_HVM_GPR_COUNT) {
        /* Return the explicit contract failure. */
        return 1U;
    }
    /* RSP is written back through the VMCS for the same reason it is read. */
    if (RegisterNumber == KSW_HVM_GPR_NUMBER_RSP) {
        /* Return whatever the VMCS write reported, unmodified. */
        return KswordARKHvmVmcsFieldStore(
            KSW_VMCS_GUEST_RSP,
            (SIZE_T)Value);
    }
    switch (RegisterNumber) {
    case 0UL:  Frame->Rax = Value; break;
    case 1UL:  Frame->Rcx = Value; break;
    case 2UL:  Frame->Rdx = Value; break;
    case 3UL:  Frame->Rbx = Value; break;
    /* Case four is RSP and was handled above. */
    case 5UL:  Frame->Rbp = Value; break;
    case 6UL:  Frame->Rsi = Value; break;
    case 7UL:  Frame->Rdi = Value; break;
    case 8UL:  Frame->R8  = Value; break;
    case 9UL:  Frame->R9  = Value; break;
    case 10UL: Frame->R10 = Value; break;
    case 11UL: Frame->R11 = Value; break;
    case 12UL: Frame->R12 = Value; break;
    case 13UL: Frame->R13 = Value; break;
    case 14UL: Frame->R14 = Value; break;
    case 15UL: Frame->R15 = Value; break;
    default:
        /* Return the explicit unreachable-number failure. */
        return 1U;
    }
    /* Return the complete register write. */
    return 0U;
}

/* Guest paging-mode inputs the walk needs, all read from the VMCS. */
#define KSW_VMCS_GUEST_CR0 0x6800UL
#define KSW_VMCS_GUEST_CR3 0x6802UL
#define KSW_VMCS_GUEST_CR4 0x6804UL
#define KSW_VMCS_GUEST_IA32_EFER 0x2806UL

/* CR0.PG, CR4.PAE, CR4.LA57 and EFER.LMA select the paging mode. */
#define KSW_HVM_CR0_PG (1ULL << 31)
#define KSW_HVM_CR4_PAE (1ULL << 5)
#define KSW_HVM_CR4_LA57 (1ULL << 12)
#define KSW_HVM_EFER_LMA (1ULL << 10)

/* Paging-structure entry bits the walk reads. */
#define KSW_HVM_PTE_PRESENT (1ULL << 0)
#define KSW_HVM_PTE_LARGE (1ULL << 7)
/* Bits 51:12 of an entry hold the next table or the page frame. */
#define KSW_HVM_PTE_FRAME_MASK 0x000FFFFFFFFFF000ULL
/* A 1 GiB leaf keeps bits 51:30; a 2 MiB leaf keeps bits 51:21. */
#define KSW_HVM_PTE_FRAME_1G_MASK 0x000FFFFFC0000000ULL
#define KSW_HVM_PTE_FRAME_2M_MASK 0x000FFFFFFFE00000ULL

/*
 * Translate one guest linear address to a guest physical address.
 *
 * This function exists because of a machine check, not a code review.  The
 * accessors below used to dereference the guest linear address directly, on
 * the argument that HOST_CR3 and GUEST_CR3 name the same kernel half because
 * Windows maps it identically into every process.  That argument is true of
 * every Windows *process* and false of the thing this whole nested path exists
 * to host: another hypervisor runs its own page tables.  The first real one to
 * reach here handed us 0xFFFFFFFFFC407E98 - an address in its own monitor
 * world, mapped in its CR3 and in no Windows address space at all - and the
 * direct dereference took a page fault in root mode.  Bugcheck 0xD1, IRQL 0xFF,
 * inside the INVEPT handler.
 *
 * The old comment also said there was no VM-exit-safe way to ask whether a page
 * is present.  There is: walk the guest's own tables through the per-processor
 * physical window, which allocates nothing, takes no lock and calls no memory
 * manager routine.  A walk that ends on a clear present bit is a refusal, which
 * the callers already know how to turn into VMfailInvalid.
 *
 * The returned address is a guest physical address, and the caller reads it
 * back through the same window - which treats it as a host physical address.
 * That is sound only because our EPT identity-maps RAM, the same assumption
 * hvm_nested_bitmap.c and hvm_nested_ept.c already read guest pages under.
 *
 * Refused rather than implemented: five-level paging, and anything that is not
 * 4-level IA-32e paging.  A guest using either gets a clean refusal rather than
 * a walk under the wrong structure format.
 */
static NTSTATUS
KswordARKHvmNestedTranslateGuestLinear(
    _Inout_ KSW_HVM_PHYS_WINDOW* Window,
    _In_ ULONGLONG LinearAddress,
    _Out_ ULONGLONG* GuestPhysical
    )
{
    SIZE_T cr0 = 0U;
    SIZE_T cr3 = 0U;
    SIZE_T cr4 = 0U;
    SIZE_T efer = 0U;
    ULONGLONG table = 0ULL;
    ULONG level = 0UL;

    *GuestPhysical = 0ULL;
    if (Window == NULL) {
        /* Return the explicit missing-window failure. */
        return STATUS_DEVICE_NOT_READY;
    }
    if (KswordARKHvmVmcsFieldLoad(KSW_VMCS_GUEST_CR0, &cr0) != 0 ||
        KswordARKHvmVmcsFieldLoad(KSW_VMCS_GUEST_CR3, &cr3) != 0 ||
        KswordARKHvmVmcsFieldLoad(KSW_VMCS_GUEST_CR4, &cr4) != 0 ||
        KswordARKHvmVmcsFieldLoad(KSW_VMCS_GUEST_IA32_EFER, &efer) != 0) {
        /* Return the explicit unreadable-guest-state failure. */
        return STATUS_UNSUCCESSFUL;
    }
    /* Refuse every paging mode other than 4-level IA-32e paging. */
    if (((ULONGLONG)cr0 & KSW_HVM_CR0_PG) == 0ULL ||
        ((ULONGLONG)cr4 & KSW_HVM_CR4_PAE) == 0ULL ||
        ((ULONGLONG)efer & KSW_HVM_EFER_LMA) == 0ULL ||
        ((ULONGLONG)cr4 & KSW_HVM_CR4_LA57) != 0ULL) {
        /* Return the explicit unsupported-paging-mode refusal. */
        return STATUS_NOT_SUPPORTED;
    }
    table = (ULONGLONG)cr3 & KSW_HVM_PTE_FRAME_MASK;
    /* Walk PML4 -> PDPT -> PD -> PT, stopping at the first leaf. */
    for (level = 4UL; level >= 1UL; --level) {
        const ULONG shift = 12UL + (9UL * (level - 1UL));
        const ULONGLONG index = (LinearAddress >> shift) & 0x1FFULL;
        ULONGLONG entry = 0ULL;

        if (!NT_SUCCESS(KswordARKHvmPhysWindowReadQword(
                Window,
                table + (index * 8ULL),
                &entry))) {
            /* Return the explicit unreadable-structure failure. */
            return STATUS_UNSUCCESSFUL;
        }
        if ((entry & KSW_HVM_PTE_PRESENT) == 0ULL) {
            /* Return the explicit not-present refusal. */
            return STATUS_NOT_FOUND;
        }
        /* A leaf at level 3 covers 1 GiB and at level 2 covers 2 MiB. */
        if (level == 3UL && (entry & KSW_HVM_PTE_LARGE) != 0ULL) {
            *GuestPhysical = (entry & KSW_HVM_PTE_FRAME_1G_MASK) |
                (LinearAddress & 0x3FFFFFFFULL);
            /* Return the complete one-gibibyte translation. */
            return STATUS_SUCCESS;
        }
        if (level == 2UL && (entry & KSW_HVM_PTE_LARGE) != 0ULL) {
            *GuestPhysical = (entry & KSW_HVM_PTE_FRAME_2M_MASK) |
                (LinearAddress & 0x1FFFFFULL);
            /* Return the complete two-mebibyte translation. */
            return STATUS_SUCCESS;
        }
        if (level == 1UL) {
            *GuestPhysical = (entry & KSW_HVM_PTE_FRAME_MASK) |
                (LinearAddress & 0xFFFULL);
            /* Return the complete four-kibibyte translation. */
            return STATUS_SUCCESS;
        }
        table = entry & KSW_HVM_PTE_FRAME_MASK;
    }
    /* Return the unreachable-walk failure. */
    return STATUS_UNSUCCESSFUL;
}

/*
 * Check that one guest linear address may be accessed from the exit handler.
 *
 * Only alignment is left here.  An eight-byte access that straddles a page
 * boundary needs two translations, and the second page can be absent while the
 * first is present - one refusal turning into a half-completed read.  Refusing
 * the straddle keeps every access to exactly one walk.
 *
 * The kernel-half rule that used to live here is gone, and deliberately.  It
 * existed only to keep the direct dereference inside the half Windows maps the
 * same way in every address space; now that the walk uses GUEST_CR3, the
 * guest's own tables decide, and a user-half operand from a ring-0 guest is
 * simply an address the walk can resolve like any other.
 */
static BOOLEAN
KswordARKHvmNestedIsGuestAccessAllowed(
    _In_ ULONGLONG LinearAddress
    )
{
    /* Reject an unaligned eight-byte access. */
    if ((LinearAddress & 0x7ULL) != 0ULL) {
        /* Report the address as not accessible from here. */
        return FALSE;
    }
    /* Report that the access may proceed. */
    return TRUE;
}

NTSTATUS
KswordARKHvmNestedReadGuestQword(
    _Inout_opt_ struct _KSW_HVM_PHYS_WINDOW* Window,
    _In_ ULONGLONG LinearAddress,
    _Out_ ULONGLONG* Value
    )
{
    ULONGLONG guestPhysical = 0ULL;
    NTSTATUS status = STATUS_UNSUCCESSFUL;

    /* Reject an incomplete caller contract before any translation. */
    if (Value == NULL) {
        /* Return the exact caller-contract failure. */
        return STATUS_INVALID_PARAMETER;
    }
    *Value = 0ULL;
    /* Refuse an access this context cannot complete in one walk. */
    if (!KswordARKHvmNestedIsGuestAccessAllowed(LinearAddress)) {
        /* Return the explicit access refusal. */
        return STATUS_ACCESS_VIOLATION;
    }
    status = KswordARKHvmNestedTranslateGuestLinear(
        (KSW_HVM_PHYS_WINDOW*)Window,
        LinearAddress,
        &guestPhysical);
    if (!NT_SUCCESS(status)) {
        /* Return the exact translation failure. */
        return status;
    }
    /* Return the guest read performed through the physical window. */
    return KswordARKHvmPhysWindowReadQword(
        (KSW_HVM_PHYS_WINDOW*)Window,
        guestPhysical,
        Value);
}

NTSTATUS
KswordARKHvmNestedWriteGuestQword(
    _Inout_opt_ struct _KSW_HVM_PHYS_WINDOW* Window,
    _In_ ULONGLONG LinearAddress,
    _In_ ULONGLONG Value
    )
{
    ULONGLONG guestPhysical = 0ULL;
    NTSTATUS status = STATUS_UNSUCCESSFUL;

    /* Refuse an access this context cannot complete in one walk. */
    if (!KswordARKHvmNestedIsGuestAccessAllowed(LinearAddress)) {
        /* Return the explicit access refusal. */
        return STATUS_ACCESS_VIOLATION;
    }
    status = KswordARKHvmNestedTranslateGuestLinear(
        (KSW_HVM_PHYS_WINDOW*)Window,
        LinearAddress,
        &guestPhysical);
    if (!NT_SUCCESS(status)) {
        /* Return the exact translation failure. */
        return status;
    }
    /*
     * A write walks read-only structures and then writes the target page.
     *
     * The dirty and accessed bits the processor would have set are not set
     * here.  Nothing in this driver reads them for these pages, and setting
     * them would mean writing the guest's paging structures from root mode -
     * a larger promise than any caller needs.
     */
    return KswordARKHvmPhysWindowWriteQword(
        (KSW_HVM_PHYS_WINDOW*)Window,
        guestPhysical,
        Value);
}

/* Translate the encoded address-size field into a width in bytes. */
static UCHAR
KswordARKHvmNestedAddressSizeBytes(
    _In_ ULONG Encoded
    )
{
    /* Select the two-byte width Intel encodes as zero. */
    if (Encoded == 0UL) {
        /* Return sixteen-bit addressing. */
        return 2U;
    }
    /* Select the four-byte width Intel encodes as one. */
    if (Encoded == 1UL) {
        /* Return thirty-two-bit addressing. */
        return 4U;
    }
    /* Return sixty-four-bit addressing for every remaining encoding. */
    return 8U;
}

/* Truncate one address to the operand address width. */
static ULONGLONG
KswordARKHvmNestedTruncateAddress(
    _In_ ULONGLONG Address,
    _In_ UCHAR AddressSizeBytes
    )
{
    /* Select sixteen-bit truncation. */
    if (AddressSizeBytes == 2U) {
        /* Return the low sixteen bits. */
        return Address & 0xFFFFULL;
    }
    /* Select thirty-two-bit truncation. */
    if (AddressSizeBytes == 4U) {
        /* Return the low thirty-two bits. */
        return Address & 0xFFFFFFFFULL;
    }
    /* Return the untruncated sixty-four-bit address. */
    return Address;
}

NTSTATUS
KswordARKHvmNestedDecodeOperand(
    _In_ const struct _KSW_HVM_GPR_FRAME* Frame,
    _In_ ULONG Layout,
    _Out_ KSW_HVM_VMX_OPERAND* Operand
    )
{
    SIZE_T rawInformation = 0U;
    SIZE_T rawQualification = 0U;
    ULONG information = 0UL;
    ULONG scaling = 0UL;
    ULONG segment = 0UL;
    ULONG indexRegister = 0UL;
    ULONG baseRegister = 0UL;
    BOOLEAN indexValid = FALSE;
    BOOLEAN baseValid = FALSE;
    ULONGLONG effectiveAddress = 0ULL;
    ULONGLONG component = 0ULL;
    SIZE_T segmentBase = 0U;

    /* Reject an incomplete caller contract before reading any VMCS field. */
    if (Frame == NULL || Operand == NULL) {
        /* Return the exact caller-contract failure. */
        return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(Operand, sizeof(*Operand));
    /* Read the instruction-information field that describes the operand. */
    if (KswordARKHvmVmcsFieldLoad(
            KSW_VMCS_VMX_INSTRUCTION_INFORMATION,
            &rawInformation) != 0U) {
        /* Return the explicit VMCS read failure. */
        return STATUS_UNSUCCESSFUL;
    }
    information = (ULONG)rawInformation;
    /* Decode the address width, which both layouts place at bits 9:7. */
    Operand->AddressSizeBytes = KswordARKHvmNestedAddressSizeBytes(
        (information >> 7) & 0x7UL);
    /*
     * Decode the register form, which only VMREAD and VMWRITE can take.
     *
     * Bit 10 is cleared to zero for the memory-only instructions, so reading
     * it under either layout is safe; what is not safe is reading bits 6:3 or
     * 31:28 under the memory-only layout, where Intel leaves them undefined.
     */
    if (Layout == KSW_HVM_VMX_OPERAND_LAYOUT_VMREAD_WRITE) {
        Operand->PrimaryRegister = (UCHAR)((information >> 3) & 0xFUL);
        Operand->SecondaryRegister = (UCHAR)((information >> 28) & 0xFUL);
        if (((information >> 10) & 0x1UL) != 0UL) {
            Operand->IsRegister = TRUE;
            /* Return the complete register-form operand. */
            return STATUS_SUCCESS;
        }
    } else if (Layout == KSW_HVM_VMX_OPERAND_LAYOUT_INVALIDATION) {
        /*
         * The invalidation pair has a register operand but never a register
         * *form*: the descriptor is always in memory, so bit 10 is not
         * consulted and the decode falls through to the memory path below.
         */
        Operand->SecondaryRegister = (UCHAR)((information >> 28) & 0xFUL);
    }
    /* Decode the memory form shared by both layouts. */
    scaling = information & 0x3UL;
    segment = (information >> 15) & 0x7UL;
    indexRegister = (information >> 18) & 0xFUL;
    /* Intel sets the invalid bits to one, so valid is the cleared state. */
    indexValid = (((information >> 22) & 0x1UL) == 0UL);
    baseRegister = (information >> 23) & 0xFUL;
    baseValid = (((information >> 27) & 0x1UL) == 0UL);
    /* Read the displacement, which the exit qualification carries whole. */
    if (KswordARKHvmVmcsFieldLoad(
            KSW_VMCS_EXIT_QUALIFICATION,
            &rawQualification) != 0U) {
        /* Return the explicit VMCS read failure. */
        return STATUS_UNSUCCESSFUL;
    }
    effectiveAddress = (ULONGLONG)rawQualification;
    /* Add the base register when the instruction named one. */
    if (baseValid) {
        if (KswordARKHvmNestedReadGpr(
                Frame,
                baseRegister,
                &component) != 0U) {
            /* Return the explicit register read failure. */
            return STATUS_UNSUCCESSFUL;
        }
        effectiveAddress += component;
    }
    /* Add the scaled index register when the instruction named one. */
    if (indexValid) {
        if (KswordARKHvmNestedReadGpr(
                Frame,
                indexRegister,
                &component) != 0U) {
            /* Return the explicit register read failure. */
            return STATUS_UNSUCCESSFUL;
        }
        effectiveAddress += (component << scaling);
    }
    /*
     * Truncate before adding the segment base, not after.
     *
     * The address-size attribute bounds the effective address that the
     * addressing expression produces; the segment base is then added to form a
     * linear address that is not itself truncated to that width.  Doing it in
     * the other order would mask off the high half of a long-mode FS or GS
     * base on any instruction that happened to use 32-bit addressing.
     */
    effectiveAddress = KswordARKHvmNestedTruncateAddress(
        effectiveAddress,
        Operand->AddressSizeBytes);
    Operand->SegmentRegister = segment;
    /* Reject a segment number Intel does not encode rather than index past. */
    if (segment > KSW_VMCS_GUEST_SEGMENT_MAX) {
        /* Return the explicit encoding failure. */
        return STATUS_UNSUCCESSFUL;
    }
    /* Read the base of the segment the operand was relative to. */
    if (KswordARKHvmVmcsFieldLoad(
            (SIZE_T)(KSW_VMCS_GUEST_ES_BASE +
                (segment * KSW_VMCS_GUEST_SEGMENT_BASE_STRIDE)),
            &segmentBase) != 0U) {
        /* Return the explicit VMCS read failure. */
        return STATUS_UNSUCCESSFUL;
    }
    Operand->LinearAddress = effectiveAddress + (ULONGLONG)segmentBase;
    /* Return the complete memory-form operand. */
    return STATUS_SUCCESS;
}

#else

UCHAR
KswordARKHvmNestedReadGpr(
    _In_ const struct _KSW_HVM_GPR_FRAME* Frame,
    _In_ ULONG RegisterNumber,
    _Out_ ULONGLONG* Value
    )
{
    UNREFERENCED_PARAMETER(Frame);
    UNREFERENCED_PARAMETER(RegisterNumber);
    /* Reject a missing output before reporting the architecture boundary. */
    if (Value != NULL) {
        *Value = 0ULL;
    }
    /* Return the explicit unsupported-architecture failure. */
    return 1U;
}

UCHAR
KswordARKHvmNestedWriteGpr(
    _Inout_ struct _KSW_HVM_GPR_FRAME* Frame,
    _In_ ULONG RegisterNumber,
    _In_ ULONGLONG Value
    )
{
    UNREFERENCED_PARAMETER(Frame);
    UNREFERENCED_PARAMETER(RegisterNumber);
    UNREFERENCED_PARAMETER(Value);
    /* Return the explicit unsupported-architecture failure. */
    return 1U;
}

NTSTATUS
KswordARKHvmNestedReadGuestQword(
    _In_ ULONGLONG LinearAddress,
    _Out_ ULONGLONG* Value
    )
{
    UNREFERENCED_PARAMETER(LinearAddress);
    /* Zero the output so no caller reads uninitialized operand state. */
    if (Value != NULL) {
        *Value = 0ULL;
    }
    /* Return the explicit unsupported-architecture boundary. */
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
KswordARKHvmNestedWriteGuestQword(
    _In_ ULONGLONG LinearAddress,
    _In_ ULONGLONG Value
    )
{
    UNREFERENCED_PARAMETER(LinearAddress);
    UNREFERENCED_PARAMETER(Value);
    /* Return the explicit unsupported-architecture boundary. */
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
KswordARKHvmNestedDecodeOperand(
    _In_ const struct _KSW_HVM_GPR_FRAME* Frame,
    _In_ ULONG Layout,
    _Out_ KSW_HVM_VMX_OPERAND* Operand
    )
{
    UNREFERENCED_PARAMETER(Frame);
    UNREFERENCED_PARAMETER(Layout);
    /* Zero the output so no caller reads uninitialized operand state. */
    if (Operand != NULL) {
        RtlZeroMemory(Operand, sizeof(*Operand));
    }
    /* Return the explicit unsupported-architecture boundary. */
    return STATUS_NOT_SUPPORTED;
}

#endif
