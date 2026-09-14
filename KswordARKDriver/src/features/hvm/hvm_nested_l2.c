/*++

Module Name:

    hvm_nested_l2.c

Abstract:

    Implements vmcs02 construction, L2 entry, and L2 exit routing.

Environment:

    Kernel-mode Driver Framework.

--*/

#include "hvm_nested_l2.h"
#include "driver/KswordArkHvmControls.h"
#include "hvm_nested_bitmap.h"
#include "hvm_nested_decode.h"
#include "hvm_nested_ept.h"
#include "hvm_resident.h"
#include "hvm_exit.h"
#include "hvm_vmcs.h"

#if defined(_M_AMD64)
#include <intrin.h>

/* Name the Intel VM-instruction errors this module reports to L1. */
#define KSW_L2_ERROR_VMLAUNCH_NONCLEAR_VMCS 4UL
#define KSW_L2_ERROR_VMRESUME_NONLAUNCHED_VMCS 5UL
#define KSW_L2_ERROR_INVALID_CONTROL_FIELDS 7UL
#define KSW_L2_ERROR_INVALID_HOST_STATE 8UL

/* Name the VMCS fields this module addresses by hand. */
#define KSW_L2_IO_BITMAP_A 0x2000UL
#define KSW_L2_IO_BITMAP_B 0x2002UL
#define KSW_L2_MSR_BITMAP 0x2004UL
/*
 * The MSR areas and the TSC offset: fields L1 writes and we never copied.
 *
 * Same shape as the bitmap defect above, one step worse.  A bitmap at least
 * has a control bit that could in principle be cleared; the MSR-area counts
 * are honoured unconditionally, so there was no capability we could have
 * stopped advertising - L1 asks for a list of MSRs to be loaded into its
 * guest, and we simply do not do it.  L2 then runs with our MSR values where
 * L1 intended its own, and nothing anywhere reports a problem.
 *
 * The VM-exit MSR-LOAD address (0x2008) is deliberately absent from this
 * list; see where the others are written for why copying it would corrupt
 * our own host state.
 */
#define KSW_L2_EXIT_MSR_STORE_ADDRESS 0x2006UL
#define KSW_L2_ENTRY_MSR_LOAD_ADDRESS 0x200AUL
#define KSW_L2_TSC_OFFSET 0x2010UL
#define KSW_L2_EXIT_MSR_STORE_COUNT 0x400EUL
#define KSW_L2_ENTRY_MSR_LOAD_COUNT 0x4014UL
#define KSW_L2_VMCS_LINK_POINTER 0x2800UL
#define KSW_L2_EPT_POINTER 0x201AUL
#define KSW_L2_PIN_CONTROLS 0x4000UL
#define KSW_L2_PRIMARY_CONTROLS 0x4002UL
#define KSW_L2_EXCEPTION_BITMAP 0x4004UL
#define KSW_L2_EXIT_CONTROLS 0x400CUL
#define KSW_L2_ENTRY_CONTROLS 0x4012UL
#define KSW_L2_SECONDARY_CONTROLS 0x401EUL
#define KSW_L2_EXIT_REASON 0x4402UL
#define KSW_L2_EXIT_INTR_INFO 0x4404UL
/* VM-entry interruption information: the event L1 asks us to deliver. */
#define KSW_L2_ENTRY_INTR_INFO 0x4016UL
/* Its two companions, needed only when re-delivering an interrupted event. */
#define KSW_L2_ENTRY_INTR_ERROR 0x4018UL
#define KSW_L2_ENTRY_INSTRUCTION_LENGTH 0x401AUL
#define KSW_L2_EXIT_INTR_ERROR 0x4406UL
#define KSW_L2_IDT_VECTORING_INFO 0x4408UL
#define KSW_L2_IDT_VECTORING_ERROR 0x440AUL
#define KSW_L2_EXIT_INSTRUCTION_LENGTH 0x440CUL
#define KSW_L2_EXIT_INSTRUCTION_INFO 0x440EUL
#define KSW_L2_EXIT_QUALIFICATION 0x6400UL
#define KSW_L2_GUEST_LINEAR_ADDRESS 0x640AUL
#define KSW_L2_GUEST_PHYSICAL_ADDRESS 0x2400UL
#define KSW_L2_GUEST_RSP 0x681CUL
#define KSW_L2_GUEST_RIP 0x681EUL
#define KSW_L2_GUEST_RFLAGS 0x6820UL
#define KSW_L2_GUEST_CR0 0x6800UL
#define KSW_L2_GUEST_CR3 0x6802UL
#define KSW_L2_GUEST_CR4 0x6804UL
#define KSW_L2_GUEST_ACTIVITY_STATE 0x4826UL
#define KSW_L2_GUEST_INTERRUPTIBILITY 0x4824UL

/* Name the secondary control that turns on EPT for L2. */
#define KSW_L2_SECONDARY_ENABLE_EPT 0x00000002UL
/*
 * Name the secondary control that lets L2 run unpaged or in real mode.
 *
 * Intel requires enable-EPT alongside it; a VMCS with one and not the other
 * fails VM entry with nothing but an error number to explain it.
 */
#define KSW_L2_SECONDARY_UNRESTRICTED_GUEST 0x00000080UL
/* Name the primary control that makes CR8 read and write a guest page. */
#define KSW_L2_PRIMARY_USE_TPR_SHADOW 0x00200000UL
/* Name the vmcs field holding the page that control points at. */
#define KSW_L2_VIRTUAL_APIC_ADDRESS 0x2012UL
/* Name the primary control that activates the secondary controls. */
#define KSW_L2_PRIMARY_ACTIVATE_SECONDARY 0x80000000UL

/*
 * Describe one field copied verbatim between vmcs12 and vmcs02.
 *
 * Guest state moves in both directions: into vmcs02 on entry so L2 runs with
 * the state L1 configured, and back into vmcs12 on reflection so L1 sees where
 * L2 got to.  One table serves both because the field list is identical.
 */
static const ULONG g_KswordL2GuestFields[] = {
    /* Segment selectors. */
    0x0800UL, 0x0802UL, 0x0804UL, 0x0806UL, 0x0808UL, 0x080AUL,
    0x080CUL, 0x080EUL,
    /* 64-bit guest state. */
    0x2802UL, 0x2804UL, 0x2806UL, 0x280AUL, 0x280CUL, 0x280EUL, 0x2810UL,
    /* Segment limits. */
    0x4800UL, 0x4802UL, 0x4804UL, 0x4806UL, 0x4808UL, 0x480AUL,
    0x480CUL, 0x480EUL, 0x4810UL, 0x4812UL,
    /* Access rights. */
    0x4814UL, 0x4816UL, 0x4818UL, 0x481AUL, 0x481CUL, 0x481EUL,
    0x4820UL, 0x4822UL,
    /* Interruptibility, activity state and SYSENTER selector. */
    0x4824UL, 0x4826UL, 0x482AUL,
    /* Control registers and segment bases. */
    0x6800UL, 0x6802UL, 0x6804UL, 0x6806UL, 0x6808UL, 0x680AUL,
    0x680CUL, 0x680EUL, 0x6810UL, 0x6812UL, 0x6814UL, 0x6816UL,
    0x6818UL, 0x681AUL, 0x681CUL, 0x681EUL, 0x6820UL, 0x6822UL,
    0x6824UL, 0x6826UL
};

/* Describe the host-state fields vmcs02 inherits from vmcs01 unchanged. */
static const ULONG g_KswordL2HostFields[] = {
    0x0C00UL, 0x0C02UL, 0x0C04UL, 0x0C06UL, 0x0C08UL, 0x0C0AUL, 0x0C0CUL,
    0x2C00UL, 0x2C02UL,
    0x4C00UL,
    0x6C00UL, 0x6C02UL, 0x6C04UL, 0x6C06UL, 0x6C08UL, 0x6C0AUL,
    0x6C0CUL, 0x6C0EUL, 0x6C10UL, 0x6C12UL, 0x6C14UL, 0x6C16UL
};

/*
 * Describe the control fields taken from vmcs12 without merging.
 *
 * These name behaviour that is entirely L1's business - which exceptions it
 * wants, what it injects, how it masks control-register bits - and none of
 * them can cause an exit to bypass us.  Controls that decide *whether we keep
 * control* are merged separately and never copied.
 */
static const ULONG g_KswordL2CopiedControlFields[] = {
    /* Exception bitmap and page-fault matching. */
    0x4004UL, 0x4006UL, 0x4008UL,
    /* Event injection. */
    0x4016UL, 0x4018UL, 0x401AUL,
    /*
     * TPR shadow: the threshold and the page it is compared against.
     *
     * These two must travel together.  The threshold was copied here long
     * before the address was, which was harmless only because the control that
     * consumes them was never advertised - the moment "use TPR shadow" became
     * advertisable, a copied threshold with an uncopied address would have sent
     * the processor to read a virtual-APIC page at physical zero.  That is the
     * same split that once made USE_MSR_BITMAPS live with no bitmap address,
     * and it is invisible from every status bit.
     *
     * The address is one of L1's guest-physical addresses and goes into vmcs02
     * unchanged, which is sound for the same reason the MSR-area addresses
     * above it are: our EPT identity-maps RAM.
     */
    0x401CUL, 0x2012UL,
    /* TSC offset. */
    0x2010UL,
    /* Control-register masks and read shadows. */
    0x6000UL, 0x6002UL, 0x6004UL, 0x6006UL
};

/* Read one field of the currently loaded VMCS, or zero. */
static ULONGLONG
KswordARKHvmNestedL2Read(
    _In_ ULONG Field
    )
{
    SIZE_T value = 0U;

    /* Report zero for a field the processor refused to produce. */
    if (KswordARKHvmVmcsFieldLoad((SIZE_T)Field, &value) != 0U) {
        /* Return the deterministic value every failure path shares. */
        return 0ULL;
    }
    /* Return the exact field value. */
    return (ULONGLONG)value;
}

/* Write one field of the currently loaded VMCS, ignoring refusal. */
static VOID
KswordARKHvmNestedL2Write(
    _In_ ULONG Field,
    _In_ ULONGLONG Value
    )
{
    /*
     * A refused write is not escalated here.
     *
     * Fields differ across processor models, and vmcs02 is validated as a
     * whole by VM entry itself: if something essential did not land, VMLAUNCH
     * fails with an architectural error that goes straight back to L1.  That
     * is a better report than aborting the merge on the first optional field
     * this processor happens not to implement.
     */
    (void)KswordARKHvmVmcsFieldStore((SIZE_T)Field, (SIZE_T)Value);
}

/* Clamp one control field to what this processor actually permits. */
static ULONG
KswordARKHvmNestedL2ClampControl(
    _In_ ULONG Requested,
    _In_ ULONGLONG CapabilityMsr
    )
{
    const ULONG allowedZero = (ULONG)(CapabilityMsr & 0xFFFFFFFFULL);
    const ULONG allowedOne = (ULONG)(CapabilityMsr >> 32);

    /*
     * The low half forces bits on and the high half permits bits at all.
     *
     * Handing the hardware a control it does not support fails VM entry with
     * an error L1 cannot act on, because the control L1 asked for was legal on
     * L1's own view of the processor.  Clamping keeps the entry valid; the
     * caller separately refuses requests whose loss would change semantics.
     */
    return (Requested | allowedZero) & allowedOne;
}

ULONG
KswordARKHvmNestedL2Enter(
    _Inout_ struct _KSW_HVM_RESIDENT_VCPU* Context,
    _In_opt_ struct _KSW_HVM_GPR_FRAME* Frame,
    _In_ BOOLEAN IsResume
    )
{
    KSW_HVM_NESTED_VCPU* nested = &Context->Nested;
    KSW_HVM_VMCS12_STATE* vmcs12 = &nested->Vmcs12;
    KSW_HVM_NESTED_BITMAP_MERGE bitmaps = { 0 };
    /*
     * Stamped first and consumed immediately before the launch.
     *
     * It bounds everything this function does to get L2 running, which is what
     * the merge's cost has to be compared against.  Taking it later would
     * flatter the merge by excluding work that is equally on the entry path.
     */
    const ULONGLONG entryStart = __rdtsc();
    ULONGLONG hostFields[RTL_NUMBER_OF(g_KswordL2HostFields)] = { 0 };
    ULONGLONG vmcs01Physical = 0ULL;
    ULONGLONG vmcs02Physical = 0ULL;
    ULONGLONG value = 0ULL;
    ULONGLONG eptPointer = 0ULL;
    ULONG primary = 0UL;
    ULONG secondary = 0UL;
    ULONG index = 0UL;

    /*
     * Refuse to re-enter an L2 the fuse already stopped.
     *
     * Reflecting the looping exit hands L1 control, and the very next thing a
     * hypervisor does with control is resume its guest - straight back into
     * the same loop.  The latch is what turns one trip into a permanent
     * refusal, so L1 sees a VM-instruction error it can report instead of the
     * machine going away.
     *
     * Reported as an invalid control field because that is what it is from
     * L1's side: some control it set produces an entry we cannot make
     * progress on.  Clearing the latch takes VMXOFF, which is L1 starting
     * over.
     */
    if (nested->L2FuseTripped) {
        /* Return the refusal L1 can read a number from. */
        nested->L2LastRefusalSite = 1UL;
        return KSW_L2_ERROR_INVALID_CONTROL_FIELDS;
    }
    /* Refuse an entry whose launch state does not match the instruction. */
    if (IsResume && !vmcs12->Launched) {
        /* Return the exact resume-before-launch error. */
        return KSW_L2_ERROR_VMRESUME_NONLAUNCHED_VMCS;
    }
    if (!IsResume && vmcs12->Launched) {
        /* Return the exact launch-on-launched error. */
        return KSW_L2_ERROR_VMLAUNCH_NONCLEAR_VMCS;
    }
    /* Refuse without the resources L2 execution needs. */
    if (Context->Resource == NULL ||
        Context->Resource->Vmcs02Virtual == NULL ||
        Context->PhysWindow == NULL) {
        /* Return the exact unavailable-resource error. */
        nested->L2LastRefusalSite = 2UL;
        return KSW_L2_ERROR_INVALID_CONTROL_FIELDS;
    }
    vmcs01Physical = (ULONGLONG)Context->Resource->VmcsPhysical.QuadPart;
    vmcs02Physical = (ULONGLONG)Context->Resource->Vmcs02Physical.QuadPart;
    (void)KswordARKHvmNestedVmcs12Read(vmcs12, KSW_L2_PRIMARY_CONTROLS, &value);
    primary = (ULONG)value;
    value = 0ULL;
    if ((primary & KSW_L2_PRIMARY_ACTIVATE_SECONDARY) != 0UL) {
        (void)KswordARKHvmNestedVmcs12Read(
            vmcs12,
            KSW_L2_SECONDARY_CONTROLS,
            &value);
        secondary = (ULONG)value;
    }
    /*
     * Arm the shadow hierarchy when L1 asked for EPT, and use our own when it
     * did not.
     *
     * Without EPT12, L2 physical addresses are L1 physical addresses, so our
     * own identity hierarchy already describes them correctly - composing a
     * shadow would produce the same mapping at the cost of a fault per page.
     */
    if ((secondary & KSW_L2_SECONDARY_ENABLE_EPT) != 0UL) {
        value = 0ULL;
        (void)KswordARKHvmNestedVmcs12Read(
            vmcs12,
            KSW_L2_EPT_POINTER,
            &value);
        if (!NT_SUCCESS(KswordARKHvmNestedEptSetL1Pointer(
                &nested->ShadowEpt,
                value))) {
            /* Return the exact unusable-EPT-pointer error. */
            nested->L2LastRefusalSite = 3UL;
            return KSW_L2_ERROR_INVALID_CONTROL_FIELDS;
        }
        eptPointer = nested->ShadowEpt.ComposedEptPointer;
    } else {
        nested->ShadowEpt.Active = FALSE;
        eptPointer = (Context->EptLocal != NULL)
            ? Context->EptLocal->EptPointer
            : Context->Runtime->EptPointer;
    }
    /* Refuse rather than enter L2 without a hierarchy to run it under. */
    if (eptPointer == 0ULL) {
        /* Return the exact unusable-EPT-pointer error. */
        nested->L2LastRefusalSite = 4UL;
        return KSW_L2_ERROR_INVALID_CONTROL_FIELDS;
    }
    /*
     * Build the bitmaps before touching vmcs02.
     *
     * Reading L1's pages needs the physical window, not a loaded VMCS, so this
     * runs while vmcs01 is still current - a refusal can then return with
     * nothing disturbed instead of leaving vmcs02 half-written.
     */
    {
        const ULONGLONG mergeStart = __rdtsc();

        KswordARKHvmNestedBitmapMerge(Context, primary, &bitmaps);
        nested->L2MergeCycles += (__rdtsc() - mergeStart);
    }
    if (bitmaps.MsrBitmapPhysical == 0ULL ||
        bitmaps.IoBitmapAPhysical == 0ULL ||
        bitmaps.IoBitmapBPhysical == 0ULL) {
        /*
         * Refuse rather than enter with an address the processor would read as
         * page zero.  An incomplete *merge* is survivable - it intercepts
         * everything - but a missing *page* is not, because there is nothing
         * to point the control at.
         */
        nested->L2LastRefusalSite = 5UL;
        return KSW_L2_ERROR_INVALID_CONTROL_FIELDS;
    }
    /*
     * The virtual-APIC page check, kept and switched off.
     *
     * Added alongside the TPR-shadow advertisement, refusing a zero or
     * misaligned address on the grounds that the processor would otherwise
     * treat physical page zero as a virtual APIC.  It became the only thing
     * standing between VMware and its first VM entry, and it is wrong on its
     * own terms: zero is four-kilobyte aligned and inside the physical-address
     * width, so the architecture accepts it.  The check invents a rule the
     * processor does not have.
     *
     * Left in place rather than deleted because the observation behind it is
     * still unexplained and still worth returning to: VMware sets the TPR
     * shadow control (vmcs12 primary = 0xB5A07DFA, bit 21 on) and, in every
     * VMWRITE we captured, never writes 0x2012.  Turn this on to stop at that
     * moment again.
     *
     * Off by default, so the address travels into vmcs02 through the copied
     * control table like every other field L1 owns, and VM entry validates it
     * the way it validates the rest.  The processor's error number then reaches
     * L1 unchanged, which is both more accurate than one we made up and the
     * answer L1 is written to handle.
     */
#define KSW_L2_ENFORCE_VIRTUAL_APIC_PAGE 0
#if KSW_L2_ENFORCE_VIRTUAL_APIC_PAGE
    if ((primary & KSW_L2_PRIMARY_USE_TPR_SHADOW) != 0UL) {
        ULONGLONG virtualApic = 0ULL;

        (void)KswordARKHvmNestedVmcs12Read(
            vmcs12,
            KSW_L2_VIRTUAL_APIC_ADDRESS,
            &virtualApic);
        /*
         * Zero and misaligned are two sites on purpose: they mean opposite
         * things about where the fault is.  Zero is also what a field reads
         * when L1 never wrote it, because the cache has no never-written
         * state; misaligned means the write did reach us and we are reading
         * something wrong.
         */
        if (virtualApic == 0ULL) {
            /* Return the exact missing-virtual-APIC-page error. */
            nested->L2LastRefusalSite = 6UL;
            return KSW_L2_ERROR_INVALID_CONTROL_FIELDS;
        }
        if ((virtualApic & 0xFFFULL) != 0ULL) {
            /* Return the exact misaligned-virtual-APIC-page error. */
            nested->L2LastRefusalSite = 8UL;
            return KSW_L2_ERROR_INVALID_CONTROL_FIELDS;
        }
    }
#endif
    /* Capture our host state while vmcs01 is still the loaded VMCS. */
    for (index = 0UL;
         index < RTL_NUMBER_OF(g_KswordL2HostFields);
         ++index) {
        hostFields[index] =
            KswordARKHvmNestedL2Read(g_KswordL2HostFields[index]);
    }
    /* Preserve where L1 must resume once L2 hands control back. */
    nested->L1ResumeRip =
        KswordARKHvmNestedL2Read(KSW_L2_GUEST_RIP) +
        KswordARKHvmNestedL2Read(KSW_L2_EXIT_INSTRUCTION_LENGTH);
    nested->L1ResumeRsp = KswordARKHvmNestedL2Read(KSW_L2_GUEST_RSP);
    nested->L1ResumeRflags = KswordARKHvmNestedL2Read(KSW_L2_GUEST_RFLAGS);
    nested->Vmcs01Physical = vmcs01Physical;
    /* Load vmcs02 and make every subsequent access address it. */
    if (__vmx_vmptrld(&vmcs02Physical) != 0) {
        /* Return the exact control-field error for an unusable vmcs02. */
        nested->L2LastRefusalSite = 7UL;
        return KSW_L2_ERROR_INVALID_CONTROL_FIELDS;
    }
    /* Host state is always ours, never L1's. */
    for (index = 0UL;
         index < RTL_NUMBER_OF(g_KswordL2HostFields);
         ++index) {
        KswordARKHvmNestedL2Write(
            g_KswordL2HostFields[index],
            hostFields[index]);
    }
    /* Guest state is whatever L1 configured for L2. */
    for (index = 0UL;
         index < RTL_NUMBER_OF(g_KswordL2GuestFields);
         ++index) {
        value = 0ULL;
        (void)KswordARKHvmNestedVmcs12Read(
            vmcs12,
            g_KswordL2GuestFields[index],
            &value);
        KswordARKHvmNestedL2Write(g_KswordL2GuestFields[index], value);
    }
    /* Controls that cannot cost us control are L1's verbatim. */
    for (index = 0UL;
         index < RTL_NUMBER_OF(g_KswordL2CopiedControlFields);
         ++index) {
        value = 0ULL;
        (void)KswordARKHvmNestedVmcs12Read(
            vmcs12,
            g_KswordL2CopiedControlFields[index],
            &value);
        KswordARKHvmNestedL2Write(
            g_KswordL2CopiedControlFields[index],
            value);
    }
    /*
     * Controls that decide who keeps control are the union of both sides.
     *
     * Ours must all survive: an exit we rely on that L1 did not request still
     * has to reach us.  L1's must also survive: an exit L1 arranged for and
     * does not receive is a hypervisor silently losing its own guest.  The
     * union satisfies both, and the clamp keeps the result legal on this
     * processor.  Nothing here ever removes one of our bits.
     */
    value = 0ULL;
    (void)KswordARKHvmNestedVmcs12Read(vmcs12, KSW_L2_PIN_CONTROLS, &value);
    KswordARKHvmNestedL2Write(
        KSW_L2_PIN_CONTROLS,
        KswordARKHvmNestedL2ClampControl(
            (ULONG)value | Context->Runtime->ActiveControls.Pin,
            Context->Runtime->ActiveControls.PinCapability));
    KswordARKHvmNestedL2Write(
        KSW_L2_PRIMARY_CONTROLS,
        KswordARKHvmNestedL2ClampControl(
            primary | Context->Runtime->ActiveControls.Primary,
            Context->Runtime->ActiveControls.PrimaryCapability));
    {
        /*
         * L1 gets the secondary controls we advertised, and nothing else.
         *
         * The clamp below uses this processor's capability, which is what the
         * hardware would allow - not what we told L1 it could have.  Those are
         * different sets, and the gap is a control L1 may set and we never
         * implemented.  "Advertised but not implemented" is a defect this code
         * already guards against; this is the same defect mirrored, and it is
         * worse, because nothing anywhere reports it.
         *
         * Measured: VMware asked for enable-VPID, which is not in
         * KSWORD_ARK_HVM_VMX_PROC2_ALLOWED and which nothing here maintains.
         * It survived the merge, and vmcs02 then carried enable-VPID with a
         * VPID of zero - which the architecture forbids.  Every VM entry after
         * that failed with "invalid control field", we handed the error back,
         * and VMware died with "VM-entry failed; VMCS valid (error code 7)".
         * Its guest had already drawn its boot menu, so the screen simply
         * stopped: no countdown, no keystrokes, and a processor at full load.
         *
         * Our own bits are added after the mask, not before: they are what we
         * need for the guest to run at all, and they are not L1's to ask for.
         */
        ULONG mergedSecondary = KswordARKHvmNestedL2ClampControl(
            (secondary & KSWORD_ARK_HVM_VMX_PROC2_ALLOWED) |
                Context->Runtime->ActiveControls.Secondary,
            Context->Runtime->ActiveControls.SecondaryCapability);

        /*
         * Unrestricted guest without enable-EPT is an illegal pair that fails
         * VM entry, exactly like virtual NMIs without NMI exiting.  Drop the
         * dependent bit rather than send a control pair we did not verify into
         * VMLAUNCH - the failure would arrive as a bare error number on a path
         * where L1, not us, looks responsible.
         *
         * The clamp above can produce this on its own: L1 may legitimately ask
         * for unrestricted guest while its own EPT bit is cleared by the
         * capability clamp, and then the two disagree through no fault of L1's.
         */
        if ((mergedSecondary & KSW_L2_SECONDARY_ENABLE_EPT) == 0UL) {
            mergedSecondary &= ~(ULONG)KSW_L2_SECONDARY_UNRESTRICTED_GUEST;
        }
        /*
         * A guest with paging or protection off cannot be entered without it.
         *
         * The processor requires CR0.PE and CR0.PG to be one unless this
         * control is set, so a vmcs02 carrying a real-mode guest without it is
         * not a VMCS the hardware will accept - and the guest state is L1's,
         * copied field by field from vmcs12, so this is about making the VMCS
         * self-consistent rather than granting L1 anything.
         *
         * Measured need: VMware's launch arrived with guest CR0 = 0x30 (PE and
         * PG both clear, the architectural reset state, CS:RIP = F000:FFF0) and
         * a vmcs12 whose secondary controls were entirely zero - the merged
         * value equalled our own set exactly.  Its guest is a BIOS starting in
         * real mode; without this bit there is no legal way to run it.
         *
         * Conditional on the guest state rather than always on, because the bit
         * changes what the processor accepts and nothing should change for the
         * paged guests that make up every other entry.
         */
        {
            ULONGLONG guestCr0 = 0ULL;

            (void)KswordARKHvmNestedVmcs12Read(
                vmcs12,
                KSW_L2_GUEST_CR0,
                &guestCr0);
            if (((guestCr0 & 0x1ULL) == 0ULL ||
                 (guestCr0 & 0x80000000ULL) == 0ULL) &&
                (mergedSecondary & KSW_L2_SECONDARY_ENABLE_EPT) != 0UL) {
                mergedSecondary |= KSW_L2_SECONDARY_UNRESTRICTED_GUEST;
            }
        }
        KswordARKHvmNestedL2Write(
            KSW_L2_SECONDARY_CONTROLS,
            mergedSecondary);
    }
    value = 0ULL;
    (void)KswordARKHvmNestedVmcs12Read(vmcs12, KSW_L2_EXIT_CONTROLS, &value);
    KswordARKHvmNestedL2Write(
        KSW_L2_EXIT_CONTROLS,
        KswordARKHvmNestedL2ClampControl(
            (ULONG)value | Context->Runtime->ActiveControls.Exit,
            Context->Runtime->ActiveControls.ExitCapability));
    /*
     * Entry controls are L1's alone - the union that is right everywhere else
     * is wrong here.
     *
     * Pin, primary, secondary and exit controls decide who intercepts what and
     * what host state an exit restores, so our bits have to survive.  Entry
     * controls decide nothing of ours: every one of them describes the guest
     * being entered, and that guest is L1's, copied field by field from
     * vmcs12.  Carrying ours across states something about L1's guest that L1
     * never said.
     *
     * "IA-32e mode guest" is where that turns fatal.  It is a description, not
     * a permission: the processor requires CR0.PG and CR4.PAE when it is set.
     * Ours is set because the guest we run is 64-bit Windows, so the union put
     * it on a vmcs02 whose guest CR0 was 0x30 - VMware's BIOS at the reset
     * vector, protection and paging both off - and VM entry failed with
     * "invalid guest state" (exit reason 0x80000021, read back out of vmcs02).
     * Every other bit is a load-this-guest-MSR request, and honouring one L1
     * did not make loads a guest register out of a vmcs02 field L1 never
     * wrote.
     *
     * The clamp still applies, so the architectural reserved bits are set and
     * nothing L1 asked for outruns this processor.
     */
    value = 0ULL;
    (void)KswordARKHvmNestedVmcs12Read(vmcs12, KSW_L2_ENTRY_CONTROLS, &value);
    KswordARKHvmNestedL2Write(
        KSW_L2_ENTRY_CONTROLS,
        KswordARKHvmNestedL2ClampControl(
            (ULONG)value,
            Context->Runtime->ActiveControls.EntryCapability));
    /*
     * Point vmcs02 at bitmaps that actually exist.
     *
     * These three addresses are separate VMCS fields from the controls that
     * consult them, and the union above always leaves USE_MSR_BITMAPS set
     * because we need it whether or not L1 asked.  Leaving the address field
     * alone therefore does not disable filtering - it aims the processor at
     * whatever the field already held, which on a fresh vmcs02 is physical
     * page zero.  Measured before this existed: primary 0xB40065F2 with bit 28
     * set and MSR_BITMAP 0x0, VM entry succeeding, L2 running, and which MSRs
     * exited decided by whatever bits live in the BIOS area.
     */
    KswordARKHvmNestedL2Write(
        KSW_L2_MSR_BITMAP,
        bitmaps.MsrBitmapPhysical);
    KswordARKHvmNestedL2Write(
        KSW_L2_IO_BITMAP_A,
        bitmaps.IoBitmapAPhysical);
    KswordARKHvmNestedL2Write(
        KSW_L2_IO_BITMAP_B,
        bitmaps.IoBitmapBPhysical);
    /*
     * Hand L2 the MSR areas L1 asked for.
     *
     * Passed through unchanged rather than translated, for the same reason the
     * shared bitmap pages are: EPT01 is an identity map, so an L1 physical
     * address is a host physical address.  The EPT12 walk already depends on
     * that; if it stops holding, these break together with it rather than one
     * of them going quietly wrong.
     *
     * Only the MSR areas.  The TSC offset is not here because
     * g_KswordL2CopiedControlFields already carries it - a duplicate write
     * stood here briefly, added on the belief that the field was unpropagated,
     * and a second writer of one field is exactly the kind of thing that later
     * makes someone ask which of the two is authoritative.
     *
     * Three of the four MSR-area fields are copied and the fourth is not:
     *
     *   entry MSR-load (0x200A) applies to the guest being entered, which is
     *   L2, so L1's list is exactly right;
     *
     *   exit MSR-store (0x2006) saves L2's MSRs on the way out, into L1's own
     *   page, which is where L1 will look for them;
     *
     *   exit MSR-LOAD (0x2008) loads *host* MSRs after the exit - and the host
     *   is us, not L1.  Copying L1's list there would have the processor load
     *   L1's host values into our root context on every single L2 exit, which
     *   is a way to lose the machine rather than a missing feature.  This
     *   version therefore drops it; servicing it correctly means applying that
     *   list while reflecting the exit to L1, in the reflection path, where
     *   "L1's host state" is the thing being restored anyway.
     */
    value = 0ULL;
    (void)KswordARKHvmNestedVmcs12Read(
        vmcs12, KSW_L2_ENTRY_MSR_LOAD_ADDRESS, &value);
    KswordARKHvmNestedL2Write(KSW_L2_ENTRY_MSR_LOAD_ADDRESS, value);
    value = 0ULL;
    (void)KswordARKHvmNestedVmcs12Read(
        vmcs12, KSW_L2_ENTRY_MSR_LOAD_COUNT, &value);
    KswordARKHvmNestedL2Write(KSW_L2_ENTRY_MSR_LOAD_COUNT, value);
    value = 0ULL;
    (void)KswordARKHvmNestedVmcs12Read(
        vmcs12, KSW_L2_EXIT_MSR_STORE_ADDRESS, &value);
    KswordARKHvmNestedL2Write(KSW_L2_EXIT_MSR_STORE_ADDRESS, value);
    value = 0ULL;
    (void)KswordARKHvmNestedVmcs12Read(
        vmcs12, KSW_L2_EXIT_MSR_STORE_COUNT, &value);
    KswordARKHvmNestedL2Write(KSW_L2_EXIT_MSR_STORE_COUNT, value);
    /* The hierarchy is ours: either the composed shadow or our own. */
    KswordARKHvmNestedL2Write(KSW_L2_EPT_POINTER, eptPointer);
    /*
     * The link pointer is always the architectural empty value.
     *
     * L1 may have written its own; propagating it would tell the processor
     * that vmcs02 shadows a VMCS that does not exist from its point of view.
     */
    KswordARKHvmNestedL2Write(KSW_L2_VMCS_LINK_POINTER, ~0ULL);
    /*
     * Read back what vmcs02 will actually run with, before handing it to the
     * processor.
     *
     * Deliberately a read of the loaded VMCS rather than a copy of the values
     * computed above: the two differ exactly when a field was never written,
     * and that is the failure this exists to make visible.
     */
    nested->LastEntryPrimaryControls =
        (ULONG)KswordARKHvmNestedL2Read(KSW_L2_PRIMARY_CONTROLS);
    nested->LastEntrySecondaryControls =
        (ULONG)KswordARKHvmNestedL2Read(KSW_L2_SECONDARY_CONTROLS);
    /*
     * Which vmcs12 this entry came from, and the guest state it carries.
     *
     * Without the physical address there is no way to tell afterwards which of
     * the pooled vmcs12 structures the entry used, and the pool keeps changing
     * underneath.  Without the guest state there is no way to tell an entry we
     * built wrongly from one L1 configured to die.
     */
    nested->LastEntryVmcs12Physical = nested->CurrentVmcs;
    /* And which vCPU of L1's this entry belongs to - see the field comment. */
    {
        ULONG slot = 0UL;

        for (slot = 0UL; slot < 4UL; ++slot) {
            if (nested->L2Vmcs12Regions[slot] == nested->CurrentVmcs) {
                break;
            }
            if (nested->L2Vmcs12Regions[slot] == 0ULL) {
                nested->L2Vmcs12Regions[slot] = nested->CurrentVmcs;
                break;
            }
        }
        if (slot < 4UL) {
            nested->L2Vmcs12RegionEntries[slot] += 1ULL;
            /*
             * Read from vmcs02 rather than from LastEntryGuestRip: that field
             * is assigned a few lines below, so using it here would record the
             * *previous* entry's address against this region.
             */
            nested->L2Vmcs12RegionLastRip[slot] =
                KswordARKHvmNestedL2Read(KSW_L2_GUEST_RIP);
        } else {
            nested->L2Vmcs12RegionMissCount += 1ULL;
        }
    }
    nested->LastEntryPinControls =
        (ULONG)KswordARKHvmNestedL2Read(KSW_L2_PIN_CONTROLS);
    nested->LastEntryExitControls =
        (ULONG)KswordARKHvmNestedL2Read(KSW_L2_EXIT_CONTROLS);
    nested->LastEntryEntryControls =
        (ULONG)KswordARKHvmNestedL2Read(KSW_L2_ENTRY_CONTROLS);
    nested->LastEntryEptPointer =
        KswordARKHvmNestedL2Read(KSW_L2_EPT_POINTER);
    nested->LastEntryGuestCr0 =
        KswordARKHvmNestedL2Read(KSW_L2_GUEST_CR0);
    nested->LastEntryGuestCr4 =
        KswordARKHvmNestedL2Read(KSW_L2_GUEST_CR4);
    nested->LastEntryGuestRip =
        KswordARKHvmNestedL2Read(KSW_L2_GUEST_RIP);
    nested->LastEntryGuestCsAr =
        (ULONG)KswordARKHvmNestedL2Read(0x4816UL);
    nested->LastEntryGuestActivity =
        (ULONG)KswordARKHvmNestedL2Read(KSW_L2_GUEST_ACTIVITY_STATE);
    /*
     * Whether this entry is carrying an event L1 asked to be delivered.
     *
     * Read back from vmcs02 rather than from vmcs12, so it counts what the
     * processor will act on.  An injection L1 requested and we failed to
     * propagate is invisible everywhere else: L1 believes its guest took the
     * interrupt, the guest never did, and both keep running.
     */
    {
        const ULONGLONG entryEvent = KswordARKHvmNestedL2Read(0x4016UL);

        if ((entryEvent & 0x80000000ULL) != 0ULL) {
            ULONG region = 0UL;

            nested->L2InjectionCount += 1ULL;
            /* And to which of L1's processors it was handed. */
            for (region = 0UL; region < 4UL; ++region) {
                if (nested->L2Vmcs12Regions[region] == nested->CurrentVmcs) {
                    nested->L2Vmcs12RegionInjections[region] += 1ULL;
                    break;
                }
            }
            /*
             * And the mode it is landing in - see the field comment.  Taken
             * from the fields captured just above, which were read out of
             * vmcs02 and are therefore what the processor is about to use.
             */
            if (nested->L2InjectStateIndex < 8UL) {
                const ULONG slot = nested->L2InjectStateIndex;

                nested->L2InjectStateVector[slot] = (ULONG)entryEvent;
                nested->L2InjectStateCr0[slot] =
                    (ULONG)nested->LastEntryGuestCr0;
                nested->L2InjectStateRflags[slot] =
                    (ULONG)KswordARKHvmNestedL2Read(0x6820UL);
                nested->L2InjectStateCsAr[slot] = nested->LastEntryGuestCsAr;
                nested->L2InjectStateIndex += 1UL;
            }
        }
    }
    nested->LastEntryMsrBitmap =
        KswordARKHvmNestedL2Read(KSW_L2_MSR_BITMAP);
    nested->LastEntryIoBitmapA =
        KswordARKHvmNestedL2Read(KSW_L2_IO_BITMAP_A);
    nested->LastEntryIoBitmapB =
        KswordARKHvmNestedL2Read(KSW_L2_IO_BITMAP_B);
    /* The newly propagated fields, read back for exactly the same reason. */
    nested->LastEntryTscOffset =
        KswordARKHvmNestedL2Read(KSW_L2_TSC_OFFSET);
    nested->LastEntryMsrLoadAddress =
        KswordARKHvmNestedL2Read(KSW_L2_ENTRY_MSR_LOAD_ADDRESS);
    nested->LastEntryMsrLoadCount =
        (ULONG)KswordARKHvmNestedL2Read(KSW_L2_ENTRY_MSR_LOAD_COUNT);
    nested->LastEntryMsrStoreAddress =
        KswordARKHvmNestedL2Read(KSW_L2_EXIT_MSR_STORE_ADDRESS);
    nested->LastEntryMsrStoreCount =
        (ULONG)KswordARKHvmNestedL2Read(KSW_L2_EXIT_MSR_STORE_COUNT);
    /* Close the entry measurement before the instruction that does not return. */
    nested->L2EntryCycles += (__rdtsc() - entryStart);
    /* Publish that this processor is about to be running L2. */
    nested->InL2 = TRUE;
    nested->State = KSWORD_ARK_HVM_NESTED_STATE_L2_ACTIVE;
    nested->L2EntryCount += 1ULL;
    /*
     * Enter L2.  On success this does not return - the processor leaves for
     * L2 and comes back through the exit stub with vmcs02 loaded.
     */
    /*
     * Enter with L1's registers, not ours.
     *
     * The entry that actually runs is issued here, in the exit handler, so
     * without this the processor carries the handler's register values into
     * L2 - and VM entry never loads GPRs from the VMCS to correct them.
     * Frame holds exactly what L1 had when it executed its VMLAUNCH, which is
     * what the architecture says its guest inherits.
     *
     * A missing Frame keeps the old behaviour rather than refusing: the entry
     * is still architecturally valid, just with registers nobody promised.
     */
    if (Frame != NULL) {
        (void)KswordARKHvmAsmNestedL2Enter(Frame, IsResume ? 1UL : 0UL);
    } else if (IsResume) {
        (void)__vmx_vmresume();
    } else {
        (void)__vmx_vmlaunch();
    }
    /* Entry failed, so nothing is running L2 and the claim must be undone. */
    nested->InL2 = FALSE;
    nested->State = KSWORD_ARK_HVM_NESTED_STATE_VMCS12_CURRENT;
    value = KswordARKHvmNestedL2Read(0x4400UL);
    (void)__vmx_vmptrld(&vmcs01Physical);
    /* Report whatever the processor said, or a generic control failure. */
    return (value != 0ULL)
        ? (ULONG)value
        : KSW_L2_ERROR_INVALID_HOST_STATE;
}

/* Name what the ownership test concluded about one L2 exit. */
#define KSW_L2_OWNER_L1 0UL
#define KSW_L2_OWNER_US_RESOLVED 1UL
#define KSW_L2_OWNER_US_NEEDS_SERVICE 2UL

/*
 * Deliver again the event whose delivery this exit interrupted.
 *
 * A VM exit can happen while the processor is still delivering an event - it
 * is reading the IDT or pushing the fault frame when the access faults.  The
 * event is then *not* delivered, and the processor says so in the
 * IDT-vectoring information field.  Whoever handles the exit is the one that
 * has to deliver it again; nothing else in the machine remembers it.
 *
 * Only the exits this driver answers itself need this.  A reflected exit
 * carries the field into vmcs12 and L1 does the re-delivery, which is what
 * real hardware would report to it.  An exit we resolve and resume from has no
 * L1 in the loop at all - and that is the overwhelming majority: composing a
 * shadow EPT leaf resolves 99% of L2's exits, and a cold shadow hierarchy
 * faults exactly where event delivery touches memory.
 *
 * What it cost to not do this: L1 acknowledges its virtual interrupt
 * controller *before* asking for the injection, so an event destroyed here is
 * an interrupt already taken off the controller with no handler and therefore
 * no EOI.  An 8259 will not assert INTR again while an interrupt of the same
 * or lower priority is in service, and IRQ 0 is the highest priority there
 * is.  Measured in the guest: ISR stuck at 0x03, IRR stuck at 0x41, BIOS tick
 * frozen for the entire run, zero injections requested in a 40-second window
 * because from L1's side there was nothing left to ask for.  Two lost
 * interrupts, early, and the machine never took another one.
 *
 * Not done here: merging a fault that arrived during delivery into #DF.  That
 * rule applies to an exception raised while delivering another exception, and
 * exception exits are L1's - they never reach this path.  If that routing ever
 * changes, this is the second half that has to come with it.
 */
static void
KswordARKHvmNestedL2RedeliverInterruptedEvent(
    _Inout_ KSW_HVM_NESTED_VCPU* Nested
    )
{
    const ULONGLONG vectoring =
        KswordARKHvmNestedL2Read(KSW_L2_IDT_VECTORING_INFO);
    ULONGLONG entry = 0ULL;
    ULONG type = 0UL;

    if ((vectoring & 0x80000000ULL) == 0ULL) {
        return;
    }
    Nested->L2IdtVectoringSeenCount += 1ULL;

    /*
     * Vector, type and the error-code flag carry over unchanged; that is bits
     * 11:0.  Bit 12 is the NMI-unblocking report, which exists only on exit
     * and is reserved on entry, and bits 30:13 are reserved in both.  Copying
     * the field wholesale would set a reserved bit and fail the entry.
     */
    entry = (vectoring & 0x00000FFFULL) | 0x80000000ULL;
    type = (ULONG)((vectoring >> 8) & 0x7ULL);

    if ((entry & 0x00000800ULL) != 0ULL) {
        KswordARKHvmNestedL2Write(
            KSW_L2_ENTRY_INTR_ERROR,
            KswordARKHvmNestedL2Read(KSW_L2_IDT_VECTORING_ERROR));
    }
    /*
     * Types 4, 5 and 6 are the software-originated ones - INT n, INT1, INT3
     * and INTO.  Re-delivering those needs the length of the instruction that
     * raised them, so the processor can set the return address past it; a
     * hardware interrupt or fault carries no length and must not have one.
     */
    if (type == 4UL || type == 5UL || type == 6UL) {
        KswordARKHvmNestedL2Write(
            KSW_L2_ENTRY_INSTRUCTION_LENGTH,
            KswordARKHvmNestedL2Read(KSW_L2_EXIT_INSTRUCTION_LENGTH));
    }
    /*
     * Put CR2 back before re-delivering.
     *
     * A page fault carries its address in CR2, not in the VMCS, so a
     * re-delivered #PF is only as good as CR2 still being what it was when the
     * delivery was interrupted.  Anything this driver did in between - a fault
     * of its own in root mode, the emulator arming a #PF - has overwritten it,
     * and the guest would be handed an address that is ours.
     *
     * Restored for every re-delivery rather than only for vector 14: writing
     * back the value the processor already had is a no-op for every other
     * event, and a condition here would be one more thing to get wrong.
     */
    __writecr2((ULONG_PTR)Nested->L2ExitCr2);
    KswordARKHvmNestedL2Write(KSW_L2_ENTRY_INTR_INFO, entry);
    Nested->L2IdtVectoringLastInfo = (ULONG)vectoring;
    Nested->L2IdtVectoringReinjectedCount += 1ULL;
    Nested->L2IdtVectoringLastExitOrdinal = Nested->L2ExitTotalCount;
}

/*
 * Decide who owns one L2 exit, and if it is ours, whether anything remains.
 *
 * Two questions, not one.  Ownership asks whether this exit happened because
 * of a control *L1* set or only one we set: anything L1 asked for must reach
 * L1, and handing L1 an exit it never armed makes it demultiplex an event it
 * has no case for.  The second question only matters for our own exits, and
 * the two answers are not interchangeable - a shadow-resolved EPT violation
 * needs nothing further, while an MSR access nobody emulated will re-execute
 * forever if we simply resume.
 */
static ULONG
KswordARKHvmNestedL2ExitOwner(
    _Inout_ struct _KSW_HVM_RESIDENT_VCPU* Context,
    _In_ struct _KSW_HVM_GPR_FRAME* Frame,
    _In_ ULONG ExitReason
    )
{
    KSW_HVM_NESTED_VCPU* nested = &Context->Nested;

    switch (ExitReason) {
    case 0UL: {
        /*
         * An NMI we sent ourselves is ours, wherever it lands.
         *
         * We broadcast an NMI to pull sibling processors out of non-root so
         * their stale translations go with the VM entry that follows.  A
         * sibling running L2 takes that NMI as an ordinary exception-or-NMI
         * exit, and reflecting it hands L1 a physical NMI that never happened
         * to its guest.  VMware's answer to one is to pass it to the host, so
         * the interrupt we created for our own bookkeeping arrives at Windows
         * with nothing to attribute it to - bugcheck 0x80, reproduced twice,
         * about twenty seconds into a guest boot and never before L2 actually
         * ran.  The ledger entry is also left unclaimed, so the next genuine
         * NMI on that processor is swallowed in its place.
         *
         * Only NMIs, and only credited ones.  An exception - L1 sets an
         * exception bitmap and its guest faults constantly - stays L1's, and
         * so does an NMI nobody in this driver asked for.
         */
        const ULONGLONG interruptionInfo =
            KswordARKHvmNestedL2Read(KSW_L2_EXIT_INTR_INFO);

        if ((interruptionInfo & 0x80000000ULL) != 0ULL &&
            ((interruptionInfo >> 8) & 0x7ULL) == 2ULL &&
            KswordARKHvmResidentClaimTlbNmi(Context->ApicId)) {
            nested->L2NmiClaimedCount += 1ULL;
            /* Report it as ours; the VM entry that follows is the flush. */
            return KSW_L2_OWNER_US_RESOLVED;
        }
        /* Report every other exception or NMI as L1's. */
        return KSW_L2_OWNER_L1;
    }
    case 48UL: {
        /*
         * An EPT violation is ours exactly when composing the leaf resolves
         * it.  A refusal means L1's own EPT12 denied the access, and that is
         * precisely the event L1 installed EPT to receive.
         */
        const ULONGLONG guestPhysical =
            KswordARKHvmNestedL2Read(KSW_L2_GUEST_PHYSICAL_ADDRESS);
        const ULONGLONG qualification =
            KswordARKHvmNestedL2Read(KSW_L2_EXIT_QUALIFICATION);
        const ULONG access = (ULONG)(qualification & 0x7ULL);

        /*
         * Device registers start above where this guest's RAM ends.  See the
         * field comment for why this is a probe threshold and not a boundary.
         */
        const BOOLEAN isDeviceSpace =
            (guestPhysical >= 0xC0000000ULL) ? TRUE : FALSE;

        /* Keep the address and the access, whatever is decided below. */
        nested->L2LastEptGuestPhysical = guestPhysical;
        nested->L2LastEptQualification = qualification;
        if (isDeviceSpace) {
            nested->L2LastMmioGuestPhysical = guestPhysical;
            nested->L2LastMmioQualification = qualification;
        }

        if (!nested->ShadowEpt.Active) {
            /*
             * No shadow means L2 is running on our own hierarchy, so this is
             * a violation against our leaves - our views and tripwires - and
             * the ordinary handling is exactly what evaluates those.
             *
             * This used to resolve to "ours, nothing further", which is only
             * true when something already fixed the leaf.  Here nothing has:
             * resuming re-executes the same access against the same leaf, and
             * the processor faults again with no error and no progress.
             */
            nested->L2LastEptDisposition = 3UL;
            if (isDeviceSpace) { nested->L2LastMmioDisposition = 3UL; }
            return KSW_L2_OWNER_US_NEEDS_SERVICE;
        }
        if (KswordARKHvmNestedEptFill(
                Context->Runtime,
                &nested->ShadowEpt,
                Context->PhysWindow,
                guestPhysical,
                access)) {
            /* Report the satisfied violation as needing nothing further. */
            nested->L2LastEptDisposition = 1UL;
            if (isDeviceSpace) {
                nested->L2LastMmioDisposition = 1UL;
                nested->L2MmioComposedCount += 1ULL;
            }
            return KSW_L2_OWNER_US_RESOLVED;
        }
        /* Report the refused violation as L1's. */
        nested->L2LastEptDisposition = 2UL;
        if (isDeviceSpace) {
            nested->L2LastMmioDisposition = 2UL;
            nested->L2MmioReflectedCount += 1ULL;
        }
        return KSW_L2_OWNER_L1;
    }
    case 2UL: {
        /*
         * A triple fault is L1's - it is the one that resets the processor -
         * but the scene has to be copied out first.
         *
         * Reflecting comes second because L1's response is to reset the vCPU,
         * and after that nothing about how the guest got here exists anywhere.
         * See the field comment for why the first one is the only one kept.
         */
        if (nested->L2TripleFaultCount == 0ULL) {
            ULONG back = 0UL;

            nested->L2TripleFaultRip =
                KswordARKHvmNestedL2Read(KSW_L2_GUEST_RIP);
            nested->L2TripleFaultCr0 =
                KswordARKHvmNestedL2Read(KSW_L2_GUEST_CR0);
            nested->L2TripleFaultCr3 = KswordARKHvmNestedL2Read(0x6802UL);
            nested->L2TripleFaultCr4 =
                KswordARKHvmNestedL2Read(KSW_L2_GUEST_CR4);
            nested->L2TripleFaultEfer = KswordARKHvmNestedL2Read(0x2806UL);
            nested->L2TripleFaultCsAr =
                (ULONG)KswordARKHvmNestedL2Read(0x4816UL);
            nested->L2TripleFaultActivity =
                (ULONG)KswordARKHvmNestedL2Read(KSW_L2_GUEST_ACTIVITY_STATE);
            /*
             * The four exits before this one, newest first.  The ring index
             * already counts this exit, so step back from it.
             */
            for (back = 0UL; back < 4UL; ++back) {
                const ULONG slot =
                    (nested->L2ExitRingIndex - 1UL - back) & 0xFUL;

                nested->L2TripleFaultPrevRip[back] =
                    nested->L2ExitRipRing[slot];
                nested->L2TripleFaultPrevReason[back] =
                    nested->L2ExitReasonRing[slot];
            }
            nested->L2TripleFaultExitOrdinal = nested->L2ExitTotalCount;
            nested->L2TripleFaultReinjectOrdinal =
                nested->L2IdtVectoringLastExitOrdinal;
            nested->L2TripleFaultLastVectoringInfo =
                nested->L2IdtVectoringLastInfo;
        }
        nested->L2TripleFaultCount += 1ULL;
        /* Report the triple fault as L1's. */
        return KSW_L2_OWNER_L1;
    }
    case 1UL:
        /*
         * An external interrupt during L2 is L1's, and this one is not allowed
         * to fall through to the default.
         *
         * Every other reason reaches the default and is reflected because
         * reflecting is the conservative direction - the cost of guessing wrong
         * is a spurious exit L1 resumes from.  Here the cost is different in
         * kind.  L1 may set "acknowledge interrupt on exit", and then the
         * processor has already taken the vector off the interrupt controller
         * by the time we look at it: nothing will ever re-deliver it.  Handling
         * such an exit ourselves does not cost L1 an exit, it destroys an
         * interrupt, and the symptom is a hang with nothing written down.
         *
         * Counted, because this is the first of the two places an interrupt
         * bound for L1's guest can go missing, and the other one cannot be
         * read without knowing whether anything arrived here at all.
         *
         * We never request external-interrupt exiting for ourselves, so a
         * reason of one can only exist because L1 asked for it.  Stating that
         * here rather than leaning on the default is the point: someone adding
         * a case for their own reasons should have to read this first.
         */
        nested->L2ExternalInterruptCount += 1ULL;
        return KSW_L2_OWNER_L1;
    case 18UL:
        /*
         * VMCALL from L2 is L1's.
         *
         * Our own hypercall surface belongs to the guest we host directly.  A
         * VMCALL two levels down is L1's guest talking to L1, and answering it
         * ourselves would impersonate L1 to its own guest.
         */
        return KSW_L2_OWNER_L1;
    case 30UL: {
        /*
         * A port access is L1's exactly when L1's own I/O controls asked for
         * it.  We request no I/O exiting at all, so anything left over is an
         * exit only the merge could have produced.
         *
         * Exit qualification for an I/O instruction (SDM 28.2.1): bits 2:0
         * hold size minus one and bits 31:16 hold the port.
         */
        const ULONGLONG qualification =
            KswordARKHvmNestedL2Read(KSW_L2_EXIT_QUALIFICATION);
        const ULONG port = (ULONG)((qualification >> 16) & 0xFFFFULL);
        const ULONG bytes = (ULONG)((qualification & 0x7ULL) + 1ULL);

        /* Record which device this was, before deciding whose exit it is. */
        {
            const ULONG slot =
                (port == 0x60UL || port == 0x64UL) ? 0UL :
                ((port >= 0x170UL && port <= 0x177UL) || port == 0x376UL) ? 1UL :
                ((port >= 0x1F0UL && port <= 0x1F7UL) || port == 0x3F6UL) ? 2UL :
                (port >= 0x3B0UL && port <= 0x3DFUL) ? 3UL :
                (port >= 0x3F8UL && port <= 0x3FFUL) ? 4UL :
                (port >= 0x40UL && port <= 0x43UL) ? 5UL :
                (port == 0x70UL || port == 0x71UL) ? 6UL : 7UL;

            nested->L2PortCounts[slot] += 1ULL;
            /*
             * The interrupt controller specifically, with the byte written.
             *
             * 0x20/0x21 are the master PIC's command and data ports, 0xA0/0xA1
             * the slave's.  An OUT carries its data in AL, which is the low
             * byte of the frame's RAX - string forms carry it in memory
             * instead, and are excluded rather than silently mis-read
             * (qualification bit 4 marks a string instruction).
             */
            if (port >= 0x40UL && port <= 0x43UL &&
                (qualification & 0x8ULL) == 0ULL &&
                (qualification & 0x10ULL) == 0ULL &&
                Frame != NULL) {
                nested->L2PitWriteTotal += 1ULL;
                if (nested->L2PitWriteIndex < 16UL) {
                    nested->L2PitWrites[nested->L2PitWriteIndex] =
                        (port << 16) | (ULONG)(Frame->Rax & 0xFFULL);
                    nested->L2PitWriteIndex += 1UL;
                }
            }
            if ((port == 0x20UL || port == 0x21UL ||
                 port == 0xA0UL || port == 0xA1UL) &&
                (qualification & 0x8ULL) == 0ULL &&
                (qualification & 0x10ULL) == 0ULL &&
                Frame != NULL) {
                const ULONG datum = (ULONG)(Frame->Rax & 0xFFULL);

                nested->L2PicWriteTotal += 1ULL;
                if (nested->L2PicWriteIndex < 16UL) {
                    nested->L2PicWrites[nested->L2PicWriteIndex] =
                        (port << 16) | datum;
                    nested->L2PicWriteIndex += 1UL;
                }
                /*
                 * A write to the data port with no initialisation in progress
                 * is OCW1, the mask.  Keeping the latest is the whole point -
                 * see the field comment.  0x0100 marks the value as set so a
                 * mask of zero is distinguishable from never having written.
                 */
                if (port == 0x21UL) {
                    nested->L2PicLastMaster = datum | 0x0100UL;
                    nested->L2PicMaskWrites += 1ULL;
                    InterlockedExchange(
                        &Context->Runtime->L2PicMaskMaster,
                        (LONG)(datum | 0x0100UL));
                } else if (port == 0xA1UL) {
                    nested->L2PicLastSlave = datum | 0x0100UL;
                    nested->L2PicMaskWrites += 1ULL;
                    InterlockedExchange(
                        &Context->Runtime->L2PicMaskSlave,
                        (LONG)(datum | 0x0100UL));
                }
            }
            /*
             * And the port itself - but only the ones no bucket names.
             *
             * The first version sampled every port and filled all sixteen
             * slots with 0x3D4, a port whose own bucket had counted seventeen
             * hundred accesses against ninety-three thousand in the catch-all.
             * Sampling the tail of a stream tells you what happened last, not
             * what happens most, and those differed by a factor of fifty here.
             * Restricting the ring to the unnamed bucket makes it sample the
             * thing it was built to identify.
             *
             * Direction and width ride along in the high half: the same port
             * read and written are different events, and "the guest is reading
             * a status register that never changes" is precisely the shape
             * this is meant to be able to show.
             */
            if (slot == 7UL) {
                const ULONG key =
                    port |
                    ((ULONG)bytes << 16) |
                    (((qualification & 0x8ULL) != 0ULL) ? 0x80000000UL : 0UL);
                ULONG probe = 0UL;

                nested->L2PortRing[nested->L2PortRingIndex & 0xFUL] = key;
                nested->L2PortRingIndex += 1UL;
                /* Port zero is the empty key; nothing here talks to it. */
                for (probe = 0UL; probe < 32UL; ++probe) {
                    if (nested->L2PortKeys[probe] == key) {
                        nested->L2PortKeyCounts[probe] += 1ULL;
                        break;
                    }
                    if (nested->L2PortKeys[probe] == 0UL) {
                        nested->L2PortKeys[probe] = key;
                        nested->L2PortKeyCounts[probe] = 1ULL;
                        break;
                    }
                }
                if (probe == 32UL) {
                    nested->L2PortKeyMissCount += 1ULL;
                }
            }
        }
        if (KswordARKHvmNestedBitmapL1WantsPort(Context, port, bytes)) {
            nested->L2IoExitsReflected += 1ULL;
            /* Report the port access as L1's. */
            return KSW_L2_OWNER_L1;
        }
        nested->L2IoExitsHandled += 1ULL;
        /* Report the port access as ours and still unserviced. */
        return KSW_L2_OWNER_US_NEEDS_SERVICE;
    }
    case 31UL:
    case 32UL: {
        /*
         * RDMSR (31) and WRMSR (32) route on L1's own bitmap, not vmcs02's.
         *
         * vmcs02's bitmap is the union, so a set bit there means "somebody
         * wanted this MSR" and cannot say who.  Handing L1 an MSR exit only we
         * armed makes it demultiplex an event it has no case for; withholding
         * one it did arm loses its guest's event silently.
         *
         * ECX carries the MSR index for both instructions.
         */
        const ULONG msrIndex = (ULONG)((Frame != NULL)
            ? (Frame->Rcx & 0xFFFFFFFFULL)
            : 0ULL);
        const BOOLEAN isWrite = (ExitReason == 32UL);

        /* Which MSR, and for a write the value, before deciding whose it is. */
        {
            const ULONG slot = nested->L2MsrRingIndex & 0x7UL;

            nested->L2MsrRing[slot] =
                (msrIndex & 0x7FFFFFFFUL) | (isWrite ? 0x80000000UL : 0UL);
            nested->L2MsrRingIndex += 1UL;
            if (isWrite && Frame != NULL) {
                const ULONGLONG value =
                    ((Frame->Rdx & 0xFFFFFFFFULL) << 32) |
                    (Frame->Rax & 0xFFFFFFFFULL);

                nested->L2LastMsrWriteIndex = msrIndex;
                nested->L2LastMsrWriteValue = value;
                /*
                 * 0x830 is the x2APIC interrupt-command register: one write is
                 * one inter-processor interrupt, destination and vector
                 * included.  Kept separately because it is rare and the MSR
                 * ring is saturated by the timer pair.
                 */
                if (msrIndex == 0x830UL) {
                    nested->L2IcrRing[nested->L2IcrRingIndex & 0x3UL] = value;
                    nested->L2IcrRingIndex += 1UL;
                    nested->L2IcrWriteCount += 1ULL;
                }
            }
        }
        if (KswordARKHvmNestedBitmapL1WantsMsr(Context, msrIndex, isWrite)) {
            nested->L2MsrExitsReflected += 1ULL;
            /* Report the MSR access as L1's. */
            return KSW_L2_OWNER_L1;
        }
        nested->L2MsrExitsHandled += 1ULL;
        /* Report the MSR access as ours and still unserviced. */
        return KSW_L2_OWNER_US_NEEDS_SERVICE;
    }
    default:
        break;
    }
    /*
     * Everything else goes to L1.
     *
     * The conservative direction is deliberate.  Handling an exit that was
     * L1's leaves L1 unaware that its guest did something it asked to see;
     * reflecting one that was ours costs L1 a spurious exit it will handle and
     * resume from.  The first is a silent correctness loss, the second is
     * overhead - so the default is to reflect.
     */
    return KSW_L2_OWNER_L1;
}

/*
 * How many identical L2 exits in a row count as "not going anywhere".
 *
 * High enough that nothing legitimate reaches it once RCX is part of the key,
 * low enough that tripping costs milliseconds rather than the machine.
 */
#define KSW_L2_NO_PROGRESS_LIMIT 1000UL

/*
 * Decide whether this L2 exit is the same one over again.
 *
 * Returns TRUE the moment the fuse trips, and keeps returning TRUE until
 * something resets it - the latch is what stops the caller from re-entering
 * L2 straight back into the same loop.
 */
static BOOLEAN
KswordARKHvmNestedL2FuseTrips(
    _Inout_ KSW_HVM_NESTED_VCPU* Nested,
    _Inout_ KSW_HVM_RUNTIME* Runtime,
    _In_ const struct _KSW_HVM_GPR_FRAME* Frame,
    _In_ ULONG ExitReason
    )
{
    const ULONGLONG rip = KswordARKHvmNestedL2Read(KSW_L2_GUEST_RIP);
    const ULONGLONG rcx = (Frame != NULL) ? Frame->Rcx : 0ULL;
    /*
     * For a memory fault, which address faulted is part of "did anything
     * move".
     *
     * Without it this fuse cannot tell a loop from a loop that is working.  A
     * kernel bringing up its memory runs one instruction over hundreds of
     * thousands of pages, faulting once per page: same RIP, same RCX, same
     * reason 48, and a different address every time.  That tripped the fuse
     * after a thousand pages, and from then on every entry was refused with
     * "invalid control field" - VMware's monitor panicked with error 7 and the
     * guest went away, a hundred and ninety thousand pages short of booting.
     *
     * Measured: three consecutive samples of the refused violation gave
     * 0x2CFE0000, 0x2F8F8000 and 0x02D57FF8.  Three different pages is
     * progress; the fuse saw one RIP repeated.
     *
     * Only for reasons 48 and 49, because the guest-physical address field is
     * only defined for those - reading it after any other exit would key the
     * fuse on a stale value and stop it tripping at all.
     */
    const ULONGLONG faultAddress =
        (ExitReason == 48UL || ExitReason == 49UL)
            ? KswordARKHvmNestedL2Read(KSW_L2_GUEST_PHYSICAL_ADDRESS)
            : 0ULL;

    if (Nested->L2FuseTripped) {
        /* Report the latched trip without re-measuring anything. */
        return TRUE;
    }
    if (rip == Nested->L2ProgressRip &&
        rcx == Nested->L2ProgressRcx &&
        faultAddress == Nested->L2ProgressFaultAddress &&
        ExitReason == Nested->L2ProgressReason) {
        Nested->L2NoProgressCount += 1UL;
    } else {
        Nested->L2ProgressRip = rip;
        Nested->L2ProgressRcx = rcx;
        Nested->L2ProgressFaultAddress = faultAddress;
        Nested->L2ProgressReason = ExitReason;
        Nested->L2NoProgressCount = 1UL;
        /* Report that something moved. */
        return FALSE;
    }
    if (Nested->L2NoProgressCount < KSW_L2_NO_PROGRESS_LIMIT) {
        /* Report that it has not gone on long enough to be a loop. */
        return FALSE;
    }
    /*
     * Latch, and keep what tripped it.
     *
     * These three values are the entire diagnosis: which exit, at which
     * instruction, how many times.  Without them a tripped fuse says only
     * "something looped", which is what we already knew.
     */
    Nested->L2FuseTripped = TRUE;
    Nested->L2FuseRip = rip;
    Nested->L2FuseReason = ExitReason;
    Nested->L2FuseCount = Nested->L2NoProgressCount;
    /* And durably, where a reader who is not our probe can find it. */
    if (Runtime != NULL) {
        InterlockedIncrement(&Runtime->NestedFuseTripCount);
    }
    /* Report the trip. */
    return TRUE;
}

ULONG
KswordARKHvmNestedL2Reflect(
    _Inout_ struct _KSW_HVM_RESIDENT_VCPU* Context,
    _In_ struct _KSW_HVM_GPR_FRAME* Frame,
    _In_ ULONG ExitReason
    )
{
    KSW_HVM_NESTED_VCPU* nested = &Context->Nested;
    KSW_HVM_VMCS12_STATE* vmcs12 = &nested->Vmcs12;
    ULONGLONG vmcs01Physical = nested->Vmcs01Physical;
    ULONG index = 0UL;

    /* Not an L2 exit at all, so there is nothing to route. */
    if (!nested->InL2) {
        /* Report that the caller owns this exit. */
        return KSW_HVM_L2_ROUTE_NOT_L2;
    }
    /*
     * Record where L2 stopped and whether it could have taken an interrupt.
     *
     * vmcs02 is the loaded VMCS here, so these describe L2 itself rather than
     * anything about L1.  Taken before any routing decision, because the
     * routing is what the next hypothesis would be about and this is meant to
     * be evidence that does not depend on one.
     */
    {
        const ULONG slot = nested->L2ExitRingIndex & 0xFUL;

        nested->L2ExitRipRing[slot] =
            KswordARKHvmNestedL2Read(KSW_L2_GUEST_RIP);
        nested->L2ExitReasonRing[slot] = ExitReason;
        nested->L2ExitRingIndex += 1UL;
        nested->L2LastRflags = KswordARKHvmNestedL2Read(0x6820UL);
        nested->L2LastInterruptibility =
            KswordARKHvmNestedL2Read(0x4824UL);
        /*
         * And L2's CR2, which nothing else will keep - see the field comment.
         * Here because this block is the first thing that runs after the exit.
         */
        nested->L2ExitCr2 = (ULONGLONG)__readcr2();
        /*
         * Charge this exit to the vmcs12 it came out of - see the region
         * fields for why a per-processor breakdown is what the question needs.
         */
        {
            ULONG region = 0UL;

            for (region = 0UL; region < 4UL; ++region) {
                if (nested->L2Vmcs12Regions[region] == nested->CurrentVmcs) {
                    nested->L2Vmcs12RegionLastExitReason[region] = ExitReason;
                    nested->L2Vmcs12RegionLastRflags[region] =
                        nested->L2LastRflags;
                    break;
                }
            }
        }
        /*
         * Has L2 ever run with interrupts enabled at all?
         *
         * The ring says what L2 is doing now and is dominated by whatever
         * repeats; every sample in it showed RFLAGS.IF clear, which is exactly
         * what a mode-switch stub looks like and says nothing about the rest
         * of the run.  A running total does: if this stays at zero then no
         * timer tick could ever have been delivered whatever L1 did, and if it
         * does not, then the guest was interruptible and the injection is
         * still the thing to explain.
         */
        if ((nested->L2LastRflags & 0x200ULL) != 0ULL) {
            nested->L2ExitIfSetCount += 1ULL;
        } else {
            nested->L2ExitIfClearCount += 1ULL;
        }
        /* And the reason, so the ring's bias stops standing in for a total. */
        nested->L2ExitReasonCounts[(ExitReason < 63UL) ? ExitReason : 63UL] +=
            1ULL;
        /*
         * And how wide the loop is, keyed by address rather than sampled.
         *
         * A RIP of zero is used as the empty key, which costs nothing here: an
         * L2 exiting at address zero has already gone wrong in a way this
         * table is not needed to see.
         */
        {
            const ULONGLONG rip = nested->L2ExitRipRing[slot];
            ULONG probe = 0UL;

            for (probe = 0UL; probe < 32UL; ++probe) {
                if (nested->L2RipKeys[probe] == rip) {
                    nested->L2RipCounts[probe] += 1ULL;
                    break;
                }
                if (nested->L2RipKeys[probe] == 0ULL) {
                    nested->L2RipKeys[probe] = rip;
                    nested->L2RipCounts[probe] = 1ULL;
                    break;
                }
            }
            if (probe == 32UL) {
                /* Full: count the misses so a spread loop is not read as narrow. */
                nested->L2RipMissCount += 1ULL;
            }
        }
        /*
         * A control-register exit also records which register and whose mask.
         *
         * The ring above showed L2 alternating between two instructions that
         * both take reason 28, forever, with interrupts disabled.  Which
         * register that is decides what the fix is, and there is no way to
         * infer it: CR3 accesses exit on a primary control we merge by union,
         * CR0 and CR4 accesses exit on the guest-host masks we copy from
         * vmcs12, and those two lead to opposite conclusions.  Both sides'
         * masks are taken here so "whose exit is this" is answered from
         * readings rather than from the merge code's intent.
         */
        if (ExitReason == 28UL) {
            ULONGLONG mask = 0ULL;

            nested->L2LastCrQualification =
                KswordARKHvmNestedL2Read(KSW_L2_EXIT_QUALIFICATION);
            /*
             * Which register, and what kind of access, over the whole run.
             *
             * The single last qualification said CR0 / MOV-to / RAX, which is
             * one sample out of a hundred and thirty thousand.  Bucketing says
             * whether that is the whole story or whether the loop also touches
             * CR4 or CR8 - and those route differently.
             *
             * Qualification bits 3:0 are the register number and bits 5:4 the
             * access type (0 MOV to, 1 MOV from, 2 CLTS, 3 LMSW).
             */
            {
                const ULONG number =
                    (ULONG)(nested->L2LastCrQualification & 0xFULL);
                const ULONG access =
                    (ULONG)((nested->L2LastCrQualification >> 4) & 0x3ULL);
                const ULONG bucket =
                    (number == 0UL) ? 0UL :
                    (number == 3UL) ? 1UL :
                    (number == 4UL) ? 2UL :
                    (number == 8UL) ? 3UL : 4UL;

                nested->L2CrCounts[bucket][access] += 1ULL;
                /*
                 * For a MOV to CR0, keep the value beside the result.
                 *
                 * Read through the shared helper rather than indexing the
                 * frame here: the register numbering has a hole where RSP
                 * would be, and a second copy of that mapping is a second
                 * chance to get it wrong.
                 */
                if (bucket == 0UL && access == 0UL && Frame != NULL) {
                    const ULONG number2 =
                        (ULONG)((nested->L2LastCrQualification >> 8) & 0xFULL);
                    const ULONGLONG rip = nested->L2ExitRipRing[slot];
                    ULONGLONG written = 0ULL;
                    ULONG probe = 0UL;

                    if (KswordARKHvmNestedReadGpr(Frame, number2, &written) == 0) {
                        /*
                         * Both companions are read here rather than taken from
                         * the fields below, which are assigned after this block
                         * and would therefore describe the previous exit - the
                         * one difference that would make this row say the write
                         * did take when it did not.
                         */
                        ULONGLONG shadow = 0ULL;

                        (void)KswordARKHvmNestedVmcs12Read(
                            vmcs12, 0x6004UL, &shadow);
                        for (probe = 0UL; probe < 4UL; ++probe) {
                            if (nested->L2CrWriteCount[probe] != 0ULL &&
                                nested->L2CrWriteRip[probe] != rip) {
                                continue;
                            }
                            nested->L2CrWriteRip[probe] = rip;
                            nested->L2CrWriteValue[probe] = written;
                            nested->L2CrWriteGuestCr0[probe] =
                                KswordARKHvmNestedL2Read(KSW_L2_GUEST_CR0);
                            nested->L2CrWriteShadow[probe] = shadow;
                            nested->L2CrWriteCount[probe] += 1ULL;
                            break;
                        }
                    }
                }
            }
            nested->L2Vmcs02Cr0Mask = KswordARKHvmNestedL2Read(0x6000UL);
            nested->L2Vmcs02Cr4Mask = KswordARKHvmNestedL2Read(0x6002UL);
            nested->L2LastGuestCr0 =
                KswordARKHvmNestedL2Read(KSW_L2_GUEST_CR0);
            mask = 0ULL;
            (void)KswordARKHvmNestedVmcs12Read(vmcs12, 0x6000UL, &mask);
            nested->L2Vmcs12Cr0Mask = mask;
            mask = 0ULL;
            (void)KswordARKHvmNestedVmcs12Read(vmcs12, 0x6004UL, &mask);
            nested->L2Vmcs12Cr0Shadow = mask;
            mask = 0ULL;
            (void)KswordARKHvmNestedVmcs12Read(vmcs12, 0x6002UL, &mask);
            nested->L2Vmcs12Cr4Mask = mask;
            nested->L2Vmcs02Primary =
                (ULONG)KswordARKHvmNestedL2Read(KSW_L2_PRIMARY_CONTROLS);
            mask = 0ULL;
            (void)KswordARKHvmNestedVmcs12Read(
                vmcs12,
                KSW_L2_PRIMARY_CONTROLS,
                &mask);
            nested->L2Vmcs12Primary = (ULONG)mask;
            /*
             * Exit controls on both sides, for acknowledge-interrupt-on-exit.
             *
             * vmcs02 carries it - bit 15 of the merged value - and we never
             * request it, so it should be L1's.  "Should be" is the problem:
             * the merge is a union, and if that bit is ours by any route then
             * every external interrupt taken during L2 is removed from the
             * interrupt controller by the processor while L1, which did not
             * ask for that, waits for a delivery that can no longer happen.
             * Four hundred lost interrupts look exactly like a guest whose
             * clock never ticks.
             */
            nested->L2Vmcs02Exit =
                (ULONG)KswordARKHvmNestedL2Read(KSW_L2_EXIT_CONTROLS);
            mask = 0ULL;
            (void)KswordARKHvmNestedVmcs12Read(
                vmcs12,
                KSW_L2_EXIT_CONTROLS,
                &mask);
            nested->L2Vmcs12Exit = (ULONG)mask;
            /*
             * Pin controls from L1, for the VMX-preemption timer.
             *
             * vmcs02 runs with pin = 0x3F, which has bit 6 clear.  A
             * hypervisor that schedules its virtual timer on the preemption
             * timer and does not get it will never be woken to deliver a tick,
             * and the clamp can remove the bit without anything reporting it -
             * the same shape as the VPID defect, in the other direction.
             */
            mask = 0ULL;
            (void)KswordARKHvmNestedVmcs12Read(
                vmcs12,
                KSW_L2_PIN_CONTROLS,
                &mask);
            nested->L2Vmcs12Pin = (ULONG)mask;
        }
    }
    /*
     * The event L1 asked to inject has been delivered, so retire its request.
     *
     * The processor clears the valid bit of the VM-entry interruption
     * information field in the VMCS it loaded - vmcs02 - and nothing clears
     * L1's.  L1 reads its own, sees the request it made still standing, and
     * concludes the injection has not happened yet.  Measured: VMware asked
     * for four events across four minutes and then stopped asking, its guest's
     * BIOS tick never advanced, and its boot menu sat at "60 seconds"
     * indefinitely while the processor ran flat out.  Four thousand seven
     * hundred interrupts had reached this routine in the same window.
     *
     * Cleared here rather than at the copy, because here the entry is a fact:
     * reaching this routine at all means L2 ran.  Clearing at the copy would
     * retire an event on an entry that then failed, which the architecture
     * does not do.
     *
     * Same family as the rest of this module's defects - state L0 has to
     * maintain on L1's behalf, left unmaintained, with every counter on both
     * sides reading healthy.
     */
    {
        ULONGLONG injected = 0ULL;

        if (NT_SUCCESS(KswordARKHvmNestedVmcs12Read(
                vmcs12,
                KSW_L2_ENTRY_INTR_INFO,
                &injected)) &&
            (injected & 0x80000000ULL) != 0ULL) {
            (void)KswordARKHvmNestedVmcs12Write(
                vmcs12,
                KSW_L2_ENTRY_INTR_INFO,
                injected & ~0x80000000ULL);
            nested->L2InjectionRetiredCount += 1ULL;
        }
    }
    /*
     * Count it before deciding whose it is.
     *
     * Every L2 exit reaches this function, so this is the only place the total
     * exists.  Counting after the decision would only ever count the ones that
     * went to L1, which is the number we already had and the one that cannot
     * detect an exit being answered here instead.
     */
    nested->L2ExitTotalCount += 1ULL;
    /*
     * An exit that was never L1's stops here, in one of two ways.
     *
     * RESOLVED means the ownership test itself finished the job - composing a
     * shadow leaf is both the test and the fix - so the caller resumes and the
     * access succeeds on retry.  Letting the ordinary handling run instead
     * would re-evaluate an L2 guest-physical against our own hierarchy, which
     * does not describe it.
     *
     * NEEDS_SERVICE means nothing has happened yet: an MSR or port access that
     * only we intercepted still has to be emulated, and RIP still has to
     * advance.  Resuming without that re-executes the same instruction into
     * the same interception, forever, with no error raised anywhere.
     */
    {
        /*
         * The fuse runs before the ownership test, not after it.
         *
         * The loop that matters resolves its own exit and returns HANDLED, so
         * it never reaches the reflection below - putting the check after the
         * test would leave exactly the failure it exists to catch untouched.
         * Once tripped, the exit is forced down the reflection path so L1 gets
         * control back and the machine keeps running.
         */
        const ULONG owner =
            KswordARKHvmNestedL2FuseTrips(
                nested, Context->Runtime, Frame, ExitReason)
                ? KSW_L2_OWNER_L1
                : KswordARKHvmNestedL2ExitOwner(Context, Frame, ExitReason);

        if (owner == KSW_L2_OWNER_US_RESOLVED) {
            /*
             * Ours to resume, so ours to finish delivering.
             *
             * Both of these returns lead back into L2 on vmcs02 without L1
             * ever seeing the exit, which makes this driver the only VMM that
             * can re-deliver an event the exit interrupted.  Written into
             * vmcs02 here and consumed by the entry that follows - see the
             * routine for what dropping it cost.
             */
            KswordARKHvmNestedL2RedeliverInterruptedEvent(nested);
            /* Report that the exit needs nothing further before resuming. */
            return KSW_HVM_L2_ROUTE_HANDLED;
        }
        if (owner == KSW_L2_OWNER_US_NEEDS_SERVICE) {
            KswordARKHvmNestedL2RedeliverInterruptedEvent(nested);
            /* Report that the ordinary handling must run on vmcs02. */
            return KSW_HVM_L2_ROUTE_SERVICE_LOCALLY;
        }
        /*
         * Reflected: L1 re-delivers, because the field reaches it in vmcs12
         * below.  Counted anyway so the pair says whether an interrupted
         * delivery happened at all before it says who dealt with it.
         */
        {
            const ULONGLONG vectoring =
                KswordARKHvmNestedL2Read(KSW_L2_IDT_VECTORING_INFO);

            if ((vectoring & 0x80000000ULL) != 0ULL) {
                nested->L2IdtVectoringSeenCount += 1ULL;
                nested->L2IdtVectoringReflectedCount += 1ULL;
            }
        }
    }
    /*
     * Fold accessed/dirty back into EPT12 before L1 can look at it.
     *
     * Here rather than at VMXOFF because this is the moment L1 regains
     * control: from its point of view its guest just stopped, and the first
     * thing a hypervisor doing dirty tracking does on a nested exit is read
     * those bits.  Propagating later would hand it a table that is correct
     * only after some event it does not know to wait for.
     *
     * Does nothing unless L1 asked for A/D and we are actually maintaining it.
     */
    (void)KswordARKHvmNestedEptPropagateAccessedDirty(
        &nested->ShadowEpt,
        Context->PhysWindow);
    /* Record where L2 got to so L1 can inspect and later resume it. */
    for (index = 0UL;
         index < RTL_NUMBER_OF(g_KswordL2GuestFields);
         ++index) {
        (void)KswordARKHvmNestedVmcs12Write(
            vmcs12,
            g_KswordL2GuestFields[index],
            KswordARKHvmNestedL2Read(g_KswordL2GuestFields[index]));
    }
    /* Record the exit itself in the fields L1 will read. */
    (void)KswordARKHvmNestedVmcs12Write(
        vmcs12,
        KSW_L2_EXIT_REASON,
        KswordARKHvmNestedL2Read(KSW_L2_EXIT_REASON));
    (void)KswordARKHvmNestedVmcs12Write(
        vmcs12,
        KSW_L2_EXIT_QUALIFICATION,
        KswordARKHvmNestedL2Read(KSW_L2_EXIT_QUALIFICATION));
    (void)KswordARKHvmNestedVmcs12Write(
        vmcs12,
        KSW_L2_EXIT_INTR_INFO,
        KswordARKHvmNestedL2Read(KSW_L2_EXIT_INTR_INFO));
    (void)KswordARKHvmNestedVmcs12Write(
        vmcs12,
        KSW_L2_EXIT_INTR_ERROR,
        KswordARKHvmNestedL2Read(KSW_L2_EXIT_INTR_ERROR));
    (void)KswordARKHvmNestedVmcs12Write(
        vmcs12,
        KSW_L2_IDT_VECTORING_INFO,
        KswordARKHvmNestedL2Read(KSW_L2_IDT_VECTORING_INFO));
    (void)KswordARKHvmNestedVmcs12Write(
        vmcs12,
        KSW_L2_IDT_VECTORING_ERROR,
        KswordARKHvmNestedL2Read(KSW_L2_IDT_VECTORING_ERROR));
    (void)KswordARKHvmNestedVmcs12Write(
        vmcs12,
        KSW_L2_EXIT_INSTRUCTION_LENGTH,
        KswordARKHvmNestedL2Read(KSW_L2_EXIT_INSTRUCTION_LENGTH));
    (void)KswordARKHvmNestedVmcs12Write(
        vmcs12,
        KSW_L2_EXIT_INSTRUCTION_INFO,
        KswordARKHvmNestedL2Read(KSW_L2_EXIT_INSTRUCTION_INFO));
    (void)KswordARKHvmNestedVmcs12Write(
        vmcs12,
        KSW_L2_GUEST_LINEAR_ADDRESS,
        KswordARKHvmNestedL2Read(KSW_L2_GUEST_LINEAR_ADDRESS));
    (void)KswordARKHvmNestedVmcs12Write(
        vmcs12,
        KSW_L2_GUEST_PHYSICAL_ADDRESS,
        KswordARKHvmNestedL2Read(KSW_L2_GUEST_PHYSICAL_ADDRESS));
    /* Go back to the VMCS that runs L1. */
    if (__vmx_vmptrld(&vmcs01Physical) != 0) {
        /*
         * Losing vmcs01 here is unrecoverable by design.
         *
         * There is no VMCS to resume and no state to report through, so the
         * only honest outcome is to stop claiming residency on this processor
         * rather than resume something undefined.
         */
        nested->InL2 = FALSE;
        /* Report it consumed: there is nothing left that could help. */
        return KSW_HVM_L2_ROUTE_HANDLED;
    }
    /*
     * Put L1 at its own VM-exit handler.
     *
     * From L1's point of view its VMLAUNCH produced a VM exit, and a VM exit
     * loads the host state L1 wrote into vmcs12.  Resuming L1 at the
     * instruction after VMLAUNCH instead would be resuming it as though the
     * entry had failed, which is a different architectural event entirely.
     */
    {
        ULONGLONG hostRip = 0ULL;
        ULONGLONG hostRsp = 0ULL;
        ULONGLONG hostCr0 = 0ULL;
        ULONGLONG hostCr3 = 0ULL;
        ULONGLONG hostCr4 = 0ULL;

        (void)KswordARKHvmNestedVmcs12Read(vmcs12, 0x6C16UL, &hostRip);
        (void)KswordARKHvmNestedVmcs12Read(vmcs12, 0x6C14UL, &hostRsp);
        (void)KswordARKHvmNestedVmcs12Read(vmcs12, 0x6C00UL, &hostCr0);
        (void)KswordARKHvmNestedVmcs12Read(vmcs12, 0x6C02UL, &hostCr3);
        (void)KswordARKHvmNestedVmcs12Read(vmcs12, 0x6C04UL, &hostCr4);
        KswordARKHvmNestedL2Write(KSW_L2_GUEST_RIP, hostRip);
        KswordARKHvmNestedL2Write(KSW_L2_GUEST_RSP, hostRsp);
        KswordARKHvmNestedL2Write(KSW_L2_GUEST_CR0, hostCr0);
        KswordARKHvmNestedL2Write(KSW_L2_GUEST_CR3, hostCr3);
        KswordARKHvmNestedL2Write(KSW_L2_GUEST_CR4, hostCr4);
        /* A VM exit always lands with interrupts masked and no shadow. */
        KswordARKHvmNestedL2Write(KSW_L2_GUEST_RFLAGS, 0x2ULL);
        KswordARKHvmNestedL2Write(KSW_L2_GUEST_INTERRUPTIBILITY, 0ULL);
        KswordARKHvmNestedL2Write(KSW_L2_GUEST_ACTIVITY_STATE, 0ULL);
    }
    vmcs12->Launched = TRUE;
    nested->InL2 = FALSE;
    nested->State = KSWORD_ARK_HVM_NESTED_STATE_VMCS12_CURRENT;
    nested->L2ExitReflectedCount += 1ULL;
    /* Report that the exit was delivered and the caller must resume L1. */
    return KSW_HVM_L2_ROUTE_REFLECTED;
}

#else

ULONG
KswordARKHvmNestedL2Enter(
    _Inout_ struct _KSW_HVM_RESIDENT_VCPU* Context,
    _In_opt_ struct _KSW_HVM_GPR_FRAME* Frame,
    _In_ BOOLEAN IsResume
    )
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Frame);
    UNREFERENCED_PARAMETER(IsResume);
    /* Report the explicit unsupported-architecture control failure. */
    return 7UL;
}

ULONG
KswordARKHvmNestedL2Reflect(
    _Inout_ struct _KSW_HVM_RESIDENT_VCPU* Context,
    _In_ struct _KSW_HVM_GPR_FRAME* Frame,
    _In_ ULONG ExitReason
    )
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Frame);
    UNREFERENCED_PARAMETER(ExitReason);
    /* Report that the caller owns this exit. */
    return KSW_HVM_L2_ROUTE_NOT_L2;
}

#endif
