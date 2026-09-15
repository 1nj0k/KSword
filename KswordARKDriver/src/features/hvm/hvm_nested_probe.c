/*++

Module Name:

    hvm_nested_probe.c

Abstract:

    Executes VMX instructions from guest context so the nested dispatch that
    services them can be observed end to end.

Environment:

    Kernel-mode Driver Framework.

--*/

#include "hvm_nested_probe.h"
#include "hvm_resident.h"
#include "hvm_runtime.h"
#include "hvm_vmcs.h"
#include "hvm_ept.h"
#include "driver/KswordArkHvmIoctl.h"
/* For the capability MSR indices the in-guest readout below names. */
#include "driver/KswordArkHvmControls.h"

#if defined(_M_AMD64)
#include <intrin.h>
#include "../../platform/pool_compat.h"

/* Name the CR4 bit that must be set before any VMX instruction is legal. */
#define KSW_PROBE_CR4_VMXE (1ULL << 13)
/* Name the MSR carrying the VMCS revision identifier. */
#define KSW_PROBE_IA32_VMX_BASIC 0x480UL
/* Name the guest RIP field, used as a harmless VMREAD/VMWRITE target. */
#define KSW_PROBE_VMCS_GUEST_RIP 0x681EUL
/* Name a value with no architectural meaning, chosen to be recognizable. */
#define KSW_PROBE_PATTERN 0x00005357444E3142ULL
/*
 * A second, different pattern for the second vmcs12.
 *
 * Different in every byte from the first, so a switch that returns the wrong
 * VMCS's value cannot be mistaken for a partial read or a torn one.
 */
#define KSW_PROBE_PATTERN_B 0x00002A6F6E654D4BULL

/*
 * How many vmcs12 the depth test cycles through.
 *
 * Two more than we claim to hold, so the pool is guaranteed to overflow and
 * the eviction path is guaranteed to run.  Derived from the pool constant
 * rather than written out, so the test follows the thing it tests - a pool
 * resized without touching this file still gets overflowed by exactly two.
 *
 * The depth it reports is measured, not assumed: every region is read back and
 * the survivors counted.  A pool shallower than advertised shows up as fewer
 * survivors, which is the failure this is here to catch.
 */
#define KSW_PROBE_DEPTH_REGIONS (KSW_HVM_VMCS12_POOL_SLOTS + 2UL)
/* Keep the survivor mask able to name every region it reports on. */
C_ASSERT(KSW_PROBE_DEPTH_REGIONS <= 64UL);
/*
 * Distinct per region, and distinct from both patterns above.
 *
 * The low byte carries the region index so a value read back from the wrong
 * slot names the slot it actually came from instead of merely mismatching.
 */
#define KSW_PROBE_DEPTH_PATTERN(index) \
    (0x0000335045454400ULL | (ULONGLONG)((index) + 1UL))

/* vmcs12 encodings for the fields whose propagation is under test. */
#define KSW_PROBE_TSC_OFFSET 0x2010UL
#define KSW_PROBE_EXIT_MSR_STORE_ADDRESS 0x2006UL
#define KSW_PROBE_ENTRY_MSR_LOAD_ADDRESS 0x200AUL
#define KSW_PROBE_EXIT_MSR_STORE_COUNT 0x400EUL
#define KSW_PROBE_ENTRY_MSR_LOAD_COUNT 0x4014UL
/* Primary control bit 3: use TSC offsetting. */
#define KSW_PROBE_PRIMARY_USE_TSC_OFFSET 0x00000008UL
/*
 * The MSR the entry list loads, written back with the value it already holds.
 *
 * IA32_STAR exists on every x64 processor, is not one of the MSRs VM entry
 * handles through a dedicated control, and loading it with what it already
 * contains changes nothing.  The point is to exercise the processor's walk of
 * L1's list, not to alter L2's state - a list that changed something would
 * make a failure look like a state bug instead of a propagation bug.
 */
#define KSW_PROBE_MSR_AREA_INDEX 0xC0000081UL
/* Where in the page each of the two areas starts; both 16-byte aligned. */
#define KSW_PROBE_MSR_LOAD_OFFSET 0x000UL
#define KSW_PROBE_MSR_STORE_OFFSET 0x100UL

/*
 * How many round trips the self-virtualize loop makes.
 *
 * One entry and one exit is not a hypervisor.  A real one goes back and forth
 * continuously, and the return leg is VMRESUME rather than VMLAUNCH - a
 * different instruction, a different launch-state check, and the first thing
 * that has to keep working if vmcs12 state is to survive a round trip at all.
 *
 * Small on purpose: the question is whether the second and third trips behave
 * like the first, which a handful answers as well as a million, without
 * leaving a processor in L2 for any length of time.
 */
#define KSW_PROBE_SELF_ROUND_TRIPS 8L

/* Name the VMCS fields the L2 construction writes by hand. */
#define KSW_PROBE_VMCS_LINK_POINTER 0x2800UL
#define KSW_PROBE_GUEST_EFER 0x2806UL
#define KSW_PROBE_PIN_CONTROLS 0x4000UL
#define KSW_PROBE_PRIMARY_CONTROLS 0x4002UL
#define KSW_PROBE_EXCEPTION_BITMAP 0x4004UL
#define KSW_PROBE_EXIT_CONTROLS 0x400CUL
#define KSW_PROBE_ENTRY_CONTROLS 0x4012UL
#define KSW_PROBE_EXIT_REASON 0x4402UL
#define KSW_PROBE_GUEST_ACTIVITY 0x4826UL
#define KSW_PROBE_GUEST_INTERRUPTIBILITY 0x4824UL
#define KSW_PROBE_GUEST_CR0 0x6800UL
#define KSW_PROBE_GUEST_CR3 0x6802UL
#define KSW_PROBE_GUEST_CR4 0x6804UL
#define KSW_PROBE_GUEST_DR7 0x681AUL
#define KSW_PROBE_GUEST_RSP 0x681CUL
#define KSW_PROBE_GUEST_RFLAGS 0x6820UL
#define KSW_PROBE_HOST_RSP 0x6C14UL
#define KSW_PROBE_HOST_RIP 0x6C16UL
#define KSW_PROBE_EXIT_QUALIFICATION 0x6400UL

/* Name the entry control that runs L2 in 64-bit mode. */
#define KSW_PROBE_ENTRY_IA32E_MODE 0x00000200UL
/* Name the exit control that returns to a 64-bit host. */
#define KSW_PROBE_EXIT_HOST_ADDRESS_SPACE 0x00000200UL
/* Name the MSR holding IA32_EFER. */
#define KSW_PROBE_IA32_EFER 0xC0000080UL
/* Name the unusable marker for a segment with no descriptor. */
#define KSW_PROBE_SEGMENT_UNUSABLE 0x10000UL

/* Name the vmcs12 fields that hand L2 its own EPT. */
#define KSW_PROBE_SECONDARY_CONTROLS 0x401EUL
#define KSW_PROBE_EPT_POINTER 0x201AUL
/* Name the primary control that makes the secondary controls take effect. */
#define KSW_PROBE_PRIMARY_ACTIVATE_SECONDARY 0x80000000UL
/* Name the primary control that makes L1's MSR bitmap take effect. */
#define KSW_PROBE_PRIMARY_USE_MSR_BITMAPS 0x10000000UL
/* Name the vmcs12 field carrying L1's MSR-bitmap address. */
#define KSW_PROBE_MSR_BITMAP 0x2004UL
/*
 * The two MSRs L2 reads, and what each one is for.
 *
 * OPEN is left out of L1's bitmap and must run without exiting; TRAPPED is put
 * into it and must exit.  Both are architecturally present everywhere, so the
 * one that is allowed through reads a real register rather than faulting.
 */
#define KSW_PROBE_L2_OPEN_MSR 0x00000010UL
#define KSW_PROBE_L2_TRAPPED_MSR 0x00000174UL
/*
 * Lay the program out at the offsets the protocol header names.
 *
 * Both sides have to agree: this builds the program, and the tool judges
 * `l2RipOffset` against the same constants.  Writing the numbers twice would
 * let a one-byte encoding change here start producing wrong verdicts there,
 * silently, because nothing would fail to compile.
 */
C_ASSERT(KSWORD_ARK_HVM_NESTED_PROBE_RIP_OPEN_MSR == 5ULL);
C_ASSERT(KSWORD_ARK_HVM_NESTED_PROBE_RIP_TRAPPED_MSR == 12ULL);
C_ASSERT(KSWORD_ARK_HVM_NESTED_PROBE_RIP_CPUID == 14ULL);
/* Name the secondary control that turns on EPT for L2. */
#define KSW_PROBE_SECONDARY_ENABLE_EPT 0x00000002UL
/* Name the four-level walk length and write-back type of an EPT pointer. */
#define KSW_HVM_NEPT_EPTP_WALK 0x18ULL
#define KSW_HVM_NEPT_EPTP_WB 0x6ULL

/* Bound the stack the probe's own L1 exit handler runs on. */
#define KSW_PROBE_L1_STACK_BYTES 8192UL

/*
 * Declared here because the WDK headers this driver includes do not.
 *
 * Both are exported by ntoskrnl and are the only way to get control back from
 * a VM-exit handler that lands on a stack of our choosing with nothing to
 * unwind to.  Declaring them locally is preferable to reaching for a private
 * header: the prototypes are stable and documented, and the alternative is
 * hand-written assembly doing the same thing less clearly.
 */
NTSYSAPI VOID NTAPI RtlCaptureContext(_Out_ PCONTEXT ContextRecord);
NTSYSAPI VOID NTAPI RtlRestoreContext(
    _In_ PCONTEXT ContextRecord,
    _In_opt_ struct _EXCEPTION_RECORD* ExceptionRecord);

/* Carry one probe's working state across the pinned execution. */
typedef struct _KSW_HVM_NESTED_PROBE_CONTEXT
{
    KSWORD_ARK_HVM_NESTED_PROBE_ROW* Response;
    PVOID VmxonVirtual;
    ULONGLONG VmxonPhysical;
    PVOID Vmcs12Virtual;
    ULONGLONG Vmcs12Physical;
    /*
     * A second vmcs12 region, for the one question a single region cannot ask.
     *
     * A hypervisor keeps several VMCSs and switches between them constantly.
     * Whether fields survive that switch is a prerequisite for hosting one,
     * and with one region the dispatcher can model a single vmcs12 and still
     * pass everything.
     */
    PVOID Vmcs12bVirtual;
    ULONGLONG Vmcs12bPhysical;
    /*
     * Enough vmcs12 regions to overflow the pool, in one contiguous block.
     *
     * One allocation rather than N, because contiguous memory gives each page
     * a physical address the previous one plus 0x1000 - and a vmcs12's
     * identity is precisely its physical address, so page granularity is all
     * that distinguishes them.
     */
    PVOID DepthBlockVirtual;
    ULONGLONG DepthBlockPhysical;
    /*
     * One page holding L1's VM-entry MSR-load list.
     *
     * A real list rather than a plausible address: the processor walks it at
     * every VM entry, so a malformed one fails entry outright.  That makes the
     * test say more than "the field was written" - it says the processor
     * accepted L1's list through vmcs02.
     */
    PVOID MsrAreaVirtual;
    ULONGLONG MsrAreaPhysical;
    /* Run L2 as this very context instead of the synthetic code page. */
    BOOLEAN SelfVirtualize;
    /* One page of L2 code, plus the stack L2 runs on. */
    PVOID L2CodeVirtual;
    /* The stack this probe's own L1 VM-exit handler runs on. */
    PVOID L1StackVirtual;
    /* The two pages of EPT12 this probe hands L1's guest. */
    PVOID Ept12Pml4Virtual;
    ULONGLONG Ept12Pml4Physical;
    PVOID Ept12PdptVirtual;
    ULONGLONG Ept12PdptPhysical;
    /* The EPT pointer written into vmcs12, or zero when EPT is not asked for. */
    ULONGLONG Ept12Pointer;
    /*
     * The MSR bitmap this probe's L1 hands its guest.
     *
     * Present so the probe can pose the only question that distinguishes a
     * merge that read L1's page from one that gave up and intercepted
     * everything: one MSR whose bit is clear and must not exit, followed by
     * one whose bit is set and must.  Both answers produce an exit; only where
     * L2 stopped says which page the processor was really consulting.
     */
    PVOID L1MsrBitmapVirtual;
    ULONGLONG L1MsrBitmapPhysical;
} KSW_HVM_NESTED_PROBE_CONTEXT;

/*
 * The probe's L2 result, reached from a different stack.
 *
 * Module scope rather than on the stack because the L1 exit handler below runs
 * on its own stack with no argument: the only channel between it and the code
 * that launched L2 is memory that both can name.
 *
 * Indexed per processor, not shared.  A single set was defensible while the
 * probe ran on one processor at a time, but the whole point of the
 * all-processors mode is that several run at once - and two processors sharing
 * one resume context would each restore the other's stack pointer.
 */
typedef struct _KSW_HVM_PROBE_SLOT
{
    volatile LONG L2Exited;
    /*
     * Which arrival at the self-virtualize capture point this is.
     *
     * One capture point is reached three times - as L1 before entry, as L2
     * after it, and as L1 again once the host handler restores the context -
     * and the code has to take a different branch each time.  L2Exited alone
     * cannot say: it is clear for the first two arrivals.
     */
    volatile LONG SelfStage;
    /*
     * Set by L2 itself, through a pointer it re-derives rather than inherits.
     *
     * L2 starts with L1's registers as of L1's entry instruction, which is a
     * different point in the function from where its RIP resumes - so whatever
     * the compiler was holding in registers there, including the pointer to
     * the response row, is not what this code expects.  Writing the marker
     * through a freshly looked-up slot removes that dependency, and reading it
     * back from the third arrival - where registers are restored wholesale -
     * is what makes "L2 really executed our code" a fact rather than an
     * inference from the exit reason.
     */
    volatile LONG L2SelfMarker;
    /* How many times L1's handler has resumed L2 in this run. */
    volatile LONG L2ResumeCount;
    /*
     * Resumes spent on exits L2 did not ask for.
     *
     * Counted apart from the round-trip budget because they are not round
     * trips: an NMI arriving while L2 runs is our own business - this driver
     * broadcasts one to invalidate TLBs across cores, and the other processor
     * may well be inside L2 at that moment.  Charging those to the loop would
     * silently shorten the test; treating them as the result the probe was
     * waiting for is worse, and is what used to happen.
     */
    volatile LONG L2AsyncResumeCount;
    /*
     * Where the per-vCPU exit counters stood before this run started.
     *
     * Those counters are cumulative for the whole residency, not per probe.
     * Reading them absolutely worked only while nothing else had ever entered
     * L2 - and the moment nested-probe-all ran first in the same residency,
     * a correct self-virtualize run reported one entry too many and failed.
     * The number this test is about is the difference.
     */
    ULONGLONG BaseEntryCount;
    ULONGLONG BaseReflectCount;
    ULONGLONG BaseTotalExitCount;
    volatile ULONGLONG L2ExitReason;
    volatile ULONGLONG L2Qualification;
    volatile ULONGLONG L2GuestRip;
    /* Invalidate the probe's populated EPT12 before the final VMXOFF. */
    KSWORD_ARK_HVM_NESTED_PROBE_ROW* InvalidationResponse;
    volatile ULONGLONG* InvalidationRoot;
    ULONGLONG InvalidationEptp;
    BOOLEAN ConfigurationFailed;
    BOOLEAN MemoryOperandsPassed;
    volatile LONG FxPassed;
    DECLSPEC_ALIGN(16) CONTEXT ResumeContext;
} KSW_HVM_PROBE_SLOT;

static KSW_HVM_PROBE_SLOT g_KswordProbeSlots[
    KSWORD_ARK_HVM_NESTED_PROBE_MAX_ROWS];

EXTERN_C ULONG KswordARKHvmAsmProbeVmcsMemory(
    SIZE_T Field, const VOID* Source, VOID* Destination);

/*
 * The one thing L2 writes that depends on nothing.
 *
 * Every earlier attempt to have L2 report back went through something it
 * inherits rather than computes: a pointer the compiler left in a register, a
 * slot found through GS, a response row addressed off the frame.  Any of those
 * can be wrong in L2 for reasons that have nothing to do with whether its
 * stores work, and all of them read back as "L2 did not run".
 *
 * A module-level global is addressed RIP-relative: no register, no segment
 * base, no call.  If this stays zero while the exit RIP proves L2 executed the
 * instruction after it, then L2's stores genuinely are not reaching the page,
 * and that is a statement about our shadow hierarchy rather than about the
 * probe's plumbing.
 */
static volatile LONG g_KswordProbeL2Marker;

/*
 * Set only if the instruction after L2's CPUID executes, which it must not.
 *
 * A volatile global rather than a field of the response row, because the row
 * is ordinary memory: the compiler is free to schedule a plain store to it
 * above the __cpuid intrinsic, and it did.  That produced a flag reading "CPUID
 * fell through" on a run where the exit count says the CPUID left L2 exactly
 * once - two instruments disagreeing because one of them was measuring the
 * optimizer rather than the processor.
 */
static volatile LONG g_KswordProbeL2PassThrough;

EXTERN_C
/*
 * Launch L2 so that it resumes by returning from this very call.
 *
 * Returns 0 to a caller that is now executing as L2, non-zero when the entry
 * did not happen.  Both answers arrive as an ordinary function return, which
 * is the entire point: L2 never has to read shared memory to work out where
 * it is, and the C compiler's assumptions about registers and stack hold,
 * because the resume point is the launch site.
 */
ULONG
KswordARKHvmAsmProbeLaunchL2(
    _Out_ volatile LONG* FxPassed
    );

EXTERN_C
/* Report the address L2 resumes at, for the within-run RIP comparison. */
ULONGLONG
KswordARKHvmAsmProbeL2ResumePoint(
    VOID
    );

/* Return this processor's slot, or NULL when its index is out of range. */
static KSW_HVM_PROBE_SLOT*
KswordARKHvmNestedProbeSlot(
    VOID
    )
{
    const ULONG index = (ULONG)KeGetCurrentProcessorNumberEx(NULL);

    /* Report no slot rather than index past the table. */
    if (index >= KSWORD_ARK_HVM_NESTED_PROBE_MAX_ROWS) {
        /* Return the explicit absence. */
        return NULL;
    }
    /* Return the slot this processor owns alone. */
    return &g_KswordProbeSlots[index];
}

/*
 * The probe's own L1 VM-exit handler - where vmcs12's host RIP points.
 *
 * Reached only if the whole chain worked: vmcs02 merged, L2 entered, L2 took
 * an exit, and our reflection loaded vmcs01 and set L1's guest state to this
 * function.  So arriving here at all is the positive result; the exit reason
 * read below says what L2 did.
 *
 * The VMREADs here are executed by L1 and go through nested dispatch against
 * vmcs12 - which is the second thing being tested: L1 has to be able to read
 * the exit information our reflection wrote.
 */
DECLSPEC_NOINLINE
static VOID
KswordARKHvmNestedProbeL1Host(
    VOID
    )
{
    KSW_HVM_PROBE_SLOT* slot = KswordARKHvmNestedProbeSlot();
    SIZE_T value = 0U;

    /*
     * Without a slot there is nowhere to report and no context to return to.
     * Leaving VMX operation and spinning is the only bounded thing left: the
     * launcher's timeout is what will notice.
     */
    if (slot == NULL) {
        __vmx_off();
        for (;;) {
            KeStallExecutionProcessor(1000UL);
        }
    }
    if (slot->InvalidationResponse != NULL) {
        KSW_HVM_SEGMENT_SNAPSHOT actual = { 0 };
        SIZE_T expectedGdt = 0U;
        SIZE_T expectedIdt = 0U;
        SIZE_T expectedGs = 0U;
        SIZE_T expectedCs = 0U;
        SIZE_T expectedEsp = 0U;
        SIZE_T expectedEip = 0U;
        ULONG checks = 0UL;

        KswordARKHvmCaptureSegments(&actual);
        if (actual.Gdtr.Limit == 0xFFFFU && actual.Idtr.Limit == 0xFFFFU) {
            checks |= 1UL;
        }
        if (__vmx_vmread(0x6C0CUL, &expectedGdt) == 0 &&
            __vmx_vmread(0x6C0EUL, &expectedIdt) == 0 &&
            expectedGdt != 0U && expectedIdt != 0U &&
            actual.Gdtr.Base == expectedGdt && actual.Idtr.Base == expectedIdt) {
            checks |= 2UL;
        }
        if (__vmx_vmread(0x6C08UL, &expectedGs) == 0 &&
            __readmsr(0xC0000101UL) == expectedGs) {
            checks |= 4UL;
        }
        if (__vmx_vmread(0x4C00UL, &expectedCs) == 0 &&
            __vmx_vmread(0x6C10UL, &expectedEsp) == 0 &&
            __vmx_vmread(0x6C12UL, &expectedEip) == 0 &&
            __readmsr(0x174UL) == expectedCs &&
            __readmsr(0x175UL) == expectedEsp &&
            __readmsr(0x176UL) == expectedEip) {
            checks |= 8UL;
        }
        if (__readdr(7) == 0x400ULL && __readmsr(0x1D9UL) == 0ULL) {
            checks |= 16UL;
        }
        if (slot->MemoryOperandsPassed) { checks |= 32UL; }
        if (slot->FxPassed == 1L) { checks |= 64UL; }
        slot->InvalidationResponse->hostStateChecks = checks;
    }
    if (__vmx_vmread((SIZE_T)KSW_PROBE_EXIT_REASON, &value) == 0) {
        slot->L2ExitReason = (ULONGLONG)value;
    }
    if (__vmx_vmread((SIZE_T)KSW_PROBE_EXIT_QUALIFICATION, &value) == 0) {
        slot->L2Qualification = (ULONGLONG)value;
    }
    if (__vmx_vmread((SIZE_T)KSW_PROBE_VMCS_GUEST_RIP, &value) == 0) {
        slot->L2GuestRip = (ULONGLONG)value;
    }
    /*
     * A CPUID exit means L2 has more to do: put it back.
     *
     * This is what makes the test a round trip rather than a one-way entry.
     * The return leg is VMRESUME, which is a different instruction from the
     * VMLAUNCH that started L2 and is checked against a different launch
     * state, so a vmcs12 that survives the first entry can still fail here.
     * L2's RIP has already been written back into vmcs12 by the reflection,
     * so resuming continues after the CPUID rather than repeating it.
     *
     * Bounded by the loop count, and only for CPUID: L2 ends the run with a
     * VMCALL, which falls through to the VMXOFF below.  An exit of any other
     * reason does too, so an unexpected one ends the run rather than being
     * resumed past.
     */
    /*
     * An asynchronous exit is not the answer: put L2 back and keep waiting.
     *
     * Exit reason 0 is exception-or-NMI, and this driver broadcasts an NMI to
     * invalidate TLBs across cores - so a processor sitting in L2 can be
     * interrupted by work another processor started.  Measured: the probe
     * failed about one run in four with reason 0x0 at RIP offset 0 and not a
     * single shadow leaf composed, because it took that exit for the one it
     * was waiting for.
     *
     * A real hypervisor simply resumes its guest here, which is exactly what
     * this now does.  Budgeted separately from the round-trip count, because
     * spending the loop's own budget on interruptions would quietly shorten
     * the test instead of failing it.
     */
    if (slot->L2ExitReason == 0ULL &&
        InterlockedCompareExchange(&slot->L2AsyncResumeCount, 0L, 0L) <
            KSW_PROBE_SELF_ROUND_TRIPS) {
        InterlockedIncrement(&slot->L2AsyncResumeCount);
        (void)__vmx_vmresume();
    }
    if (slot->L2ExitReason == 10ULL &&
        InterlockedCompareExchange(&slot->L2ResumeCount, 0L, 0L) <
            KSW_PROBE_SELF_ROUND_TRIPS) {
        InterlockedIncrement(&slot->L2ResumeCount);
        (void)__vmx_vmresume();
        /*
         * Only reached when the resume did not happen.  Fall through and end
         * the run rather than spinning: the counts reported afterwards say
         * how far it got.
         */
    }
    if (slot->InvalidationResponse != NULL &&
        slot->InvalidationRoot != NULL && slot->InvalidationEptp != 0ULL) {
        KSW_HVM_RESIDENT_VCPU* vcpu = KswordARKHvmResidentFindCurrent();

        if (vcpu != NULL && vcpu->Nested.ShadowEpt.Active &&
            vcpu->Nested.ShadowEpt.L1EptPointer == slot->InvalidationEptp) {
            const ULONG generation = vcpu->Nested.ShadowEpt.Generation;
            const LONG64 root = InterlockedAnd64(
                (volatile LONG64*)slot->InvalidationRoot, ~2LL);

            /*
             * Remove write permission in a table L2 actually used, then
             * invalidate that exact EPTP. An untouched or foreign hierarchy
             * may correctly be retained, so testing before L2 entered could
             * not establish whether stale composed leaves were discarded.
             */
            slot->InvalidationResponse->inveptResult =
                (ULONG)KswordARKHvmAsmInveptSingle(slot->InvalidationEptp);
            slot->InvalidationResponse->shadowGenerationAdvanced =
                (vcpu->Nested.ShadowEpt.Generation != generation &&
                 !vcpu->Nested.ShadowEpt.Faulted) ? 1UL : 0UL;
            (void)InterlockedExchange64(
                (volatile LONG64*)slot->InvalidationRoot, root);
        }
    }
    InterlockedExchange(&slot->L2Exited, 1L);
    /* Leave emulated VMX operation before abandoning this stack. */
    __vmx_off();
    /*
     * Return to the launcher by restoring the context it captured.
     *
     * A VM exit does not return to its caller - it lands here on a stack this
     * function was given, with nothing to unwind to.  Restoring the captured
     * context is the only way back, and it is exactly a longjmp: the launcher
     * resumes just after its capture, with the flag above already set.
     */
    RtlRestoreContext(&slot->ResumeContext, NULL);
}

/*
 * Build the EPT12 this probe hands L1's guest: an identity map of the low
 * physical address space using one-GiB leaves.
 *
 * Identity because the point is not to relocate anything - it is to make the
 * composition path run.  With EPT12 present, every L2 access has to go through
 * EPT12 and then through our EPT01, which is exactly the two-level walk the
 * shadow hierarchy exists to collapse.  An identity EPT12 means a composition
 * bug shows up as a fault or a wrong page rather than as plausible-looking
 * relocated memory, which is easier to attribute.
 *
 * One-GiB leaves keep the whole thing to two pages.  Returns FALSE when the
 * processor does not support them, in which case the probe runs the no-EPT
 * path instead of quietly testing something else.
 */
static BOOLEAN
KswordARKHvmNestedProbeBuildEpt12(
    _Inout_ KSW_HVM_NESTED_PROBE_CONTEXT* Probe,
    _In_ ULONG GigabytesToMap
    )
{
    volatile ULONGLONG* pml4 = (volatile ULONGLONG*)Probe->Ept12Pml4Virtual;
    volatile ULONGLONG* pdpt = (volatile ULONGLONG*)Probe->Ept12PdptVirtual;
    ULONG index = 0UL;

    /* Require one-GiB EPT leaves before claiming this hierarchy is usable. */
    if ((__readmsr(0x48CUL) & (1ULL << 17)) == 0ULL) {
        /* Report that the identity hierarchy cannot be built this way. */
        return FALSE;
    }
    /* Bound the map to the PDPT a single page holds. */
    if (GigabytesToMap > 512UL) {
        GigabytesToMap = 512UL;
    }
    /* One PML4 entry covers the whole 512-GiB region the PDPT describes. */
    pml4[0] = (Probe->Ept12PdptPhysical & 0x000FFFFFFFFFF000ULL) | 0x7ULL;
    for (index = 0UL; index < GigabytesToMap; ++index) {
        /*
         * Read, write, execute, write-back, and the large-page bit.
         *
         * Full permissions because any narrowing here would be testing L1's
         * policy rather than our composition, and a denial would be
         * indistinguishable from a composition failure.
         */
        pdpt[index] =
            ((ULONGLONG)index << 30) | 0x7ULL | 0x30ULL | 0x80ULL;
    }
    Probe->Ept12Pointer =
        (Probe->Ept12Pml4Physical & 0x000FFFFFFFFFF000ULL) |
        KSW_HVM_NEPT_EPTP_WALK | KSW_HVM_NEPT_EPTP_WB;
    /* Report a complete identity hierarchy. */
    return TRUE;
}

/* Keep a refused configuration write from becoming a destructive VM entry. */
static VOID
KswordARKHvmNestedProbeVmcs12Write(
    _In_ ULONG Field,
    _In_ ULONGLONG Value
    )
{
    if (__vmx_vmwrite((SIZE_T)Field, (SIZE_T)Value) != 0) {
        KSW_HVM_PROBE_SLOT* slot = KswordARKHvmNestedProbeSlot();
        if (slot != NULL) { slot->ConfigurationFailed = TRUE; }
    }
}

/* Program one segment's four guest-state fields from a resolved descriptor. */
static VOID
KswordARKHvmNestedProbeWriteSegment(
    _In_ ULONG SelectorField,
    _In_ const KSW_HVM_SEGMENT_STATE* Segment
    )
{
    KswordARKHvmNestedProbeVmcs12Write(
        SelectorField,
        Segment->Selector);
    /* Limit, access rights and base sit at fixed strides from the selector. */
    KswordARKHvmNestedProbeVmcs12Write(
        0x4800UL + (SelectorField - 0x0800UL),
        Segment->Limit);
    KswordARKHvmNestedProbeVmcs12Write(
        0x4814UL + (SelectorField - 0x0800UL),
        Segment->AccessRights);
    KswordARKHvmNestedProbeVmcs12Write(
        0x6806UL + (SelectorField - 0x0800UL),
        Segment->Base);
}

/*
 * Fill vmcs12 with a complete description of one minimal L2.
 *
 * L2 runs in 64-bit mode on the *current* address space: same CR0/CR3/CR4 and
 * EFER, same descriptor tables.  That is deliberate - building an independent
 * address space for L2 would mean building page tables, and none of what is
 * under test here depends on L2 having its own.  What it does depend on is
 * guest state that VM entry accepts, and the surest source of state that a
 * processor accepts is the state that processor is running right now.
 */
static BOOLEAN
KswordARKHvmNestedProbeBuildVmcs12(
    _In_ const KSW_HVM_NESTED_PROBE_CONTEXT* Probe
    )
{
    KSW_HVM_SEGMENT_SNAPSHOT snapshot = { 0 };
    KSW_HVM_SEGMENT_STATE segment = { 0 };
    ULONGLONG efer = __readmsr(KSW_PROBE_IA32_EFER);
    KSW_HVM_PROBE_SLOT* slot = KswordARKHvmNestedProbeSlot();

    if (slot == NULL) { return FALSE; }
    slot->ConfigurationFailed = FALSE;
    slot->MemoryOperandsPassed = FALSE;
    KswordARKHvmCaptureSegments(&snapshot);
    if (snapshot.Gdtr.Base == 0ULL || snapshot.Idtr.Base == 0ULL) { return FALSE; }
    {
        const ULONG_PTR boundary =
            ((ULONG_PTR)Probe->L1StackVirtual + PAGE_SIZE) & ~(ULONG_PTR)(PAGE_SIZE - 1UL);
        ULONG index;
        /* Exercise both memory operands at every alignment and across a page. */
        for (index = 0UL; index < 16UL; ++index) {
            PUCHAR crossing = (PUCHAR)(boundary - 8UL + (index & 7UL));
            PUCHAR local = (PUCHAR)Probe->L1StackVirtual + 128UL + (index & 7UL);
            PUCHAR source = index < 8UL ? crossing : local;
            PUCHAR destination = index < 8UL ? local : crossing;
            const ULONGLONG expected = 0xA591736BC024E80FULL ^ index;
            ULONGLONG actual = 0ULL;
            RtlCopyMemory(source, &expected, sizeof(expected));
            RtlZeroMemory(destination, sizeof(actual));
            if (KswordARKHvmAsmProbeVmcsMemory(
                    KSW_PROBE_VMCS_GUEST_RIP, source, destination) != 0UL) {
                return FALSE;
            }
            RtlCopyMemory(&actual, destination, sizeof(actual));
            if (actual != expected) { return FALSE; }
        }
        slot->MemoryOperandsPassed = TRUE;
    }
    /* Controls: 64-bit entry and exit, plus EPT when one was built. */
    KswordARKHvmNestedProbeVmcs12Write(KSW_PROBE_PIN_CONTROLS, 0ULL);
    /*
     * L1 always asks for MSR filtering here.
     *
     * Without it every L2 MSR access belongs to L1 by architecture and the
     * merge has nothing to decide, so the interesting half of the routing
     * would never be exercised.  With it, the bitmap L1 supplies is the only
     * thing that can tell the two reads below apart.
     */
    KswordARKHvmNestedProbeVmcs12Write(
        KSW_PROBE_MSR_BITMAP,
        Probe->L1MsrBitmapPhysical);
    if (Probe->Ept12Pointer != 0ULL) {
        KswordARKHvmNestedProbeVmcs12Write(
            KSW_PROBE_PRIMARY_CONTROLS,
            KSW_PROBE_PRIMARY_ACTIVATE_SECONDARY |
                KSW_PROBE_PRIMARY_USE_MSR_BITMAPS |
                KSW_PROBE_PRIMARY_USE_TSC_OFFSET);
        KswordARKHvmNestedProbeVmcs12Write(
            KSW_PROBE_SECONDARY_CONTROLS,
            KSW_PROBE_SECONDARY_ENABLE_EPT);
        KswordARKHvmNestedProbeVmcs12Write(
            KSW_PROBE_EPT_POINTER,
            Probe->Ept12Pointer);
    } else {
        KswordARKHvmNestedProbeVmcs12Write(
            KSW_PROBE_PRIMARY_CONTROLS,
            KSW_PROBE_PRIMARY_USE_MSR_BITMAPS |
                KSW_PROBE_PRIMARY_USE_TSC_OFFSET);
    }
    /*
     * The fields this version newly propagates, set to something recognizable.
     *
     * The TSC offset is pure readback: a value that cannot be confused with
     * the zero an unwritten field holds.  Its control bit goes on above, since
     * without it the processor would ignore the offset and the field would
     * propagate with nothing depending on it.
     *
     * The entry MSR-load list is a real one-entry list, so the processor walks
     * it at VM entry.  The exit MSR-store count stays zero on purpose: a
     * malformed store area is a VMX abort rather than a clean entry failure,
     * and an abort takes the machine down instead of reporting a result.  Its
     * address still propagates and is still checked, so the field write is
     * covered even though the processor's use of it is not.
     */
    KswordARKHvmNestedProbeVmcs12Write(
        KSW_PROBE_TSC_OFFSET,
        KSWORD_ARK_HVM_NESTED_PROBE_TSC_OFFSET);
    if (Probe->MsrAreaPhysical != 0ULL) {
        KswordARKHvmNestedProbeVmcs12Write(
            KSW_PROBE_ENTRY_MSR_LOAD_ADDRESS,
            Probe->MsrAreaPhysical + KSW_PROBE_MSR_LOAD_OFFSET);
        KswordARKHvmNestedProbeVmcs12Write(
            KSW_PROBE_ENTRY_MSR_LOAD_COUNT,
            1ULL);
        KswordARKHvmNestedProbeVmcs12Write(
            KSW_PROBE_EXIT_MSR_STORE_ADDRESS,
            Probe->MsrAreaPhysical + KSW_PROBE_MSR_STORE_OFFSET);
        KswordARKHvmNestedProbeVmcs12Write(
            KSW_PROBE_EXIT_MSR_STORE_COUNT,
            0ULL);
    }
    KswordARKHvmNestedProbeVmcs12Write(KSW_PROBE_EXCEPTION_BITMAP, 0ULL);
    KswordARKHvmNestedProbeVmcs12Write(
        KSW_PROBE_EXIT_CONTROLS,
        KSW_PROBE_EXIT_HOST_ADDRESS_SPACE);
    KswordARKHvmNestedProbeVmcs12Write(
        KSW_PROBE_ENTRY_CONTROLS,
        KSW_PROBE_ENTRY_IA32E_MODE);
    KswordARKHvmNestedProbeVmcs12Write(
        KSW_PROBE_VMCS_LINK_POINTER,
        ~0ULL);
    /* Guest control registers and mode. */
    KswordARKHvmNestedProbeVmcs12Write(KSW_PROBE_GUEST_CR0, __readcr0());
    KswordARKHvmNestedProbeVmcs12Write(KSW_PROBE_GUEST_CR3, __readcr3());
    KswordARKHvmNestedProbeVmcs12Write(KSW_PROBE_GUEST_CR4, __readcr4());
    KswordARKHvmNestedProbeVmcs12Write(KSW_PROBE_GUEST_EFER, efer);
    KswordARKHvmNestedProbeVmcs12Write(KSW_PROBE_GUEST_DR7, 0x400ULL);
    KswordARKHvmNestedProbeVmcs12Write(KSW_PROBE_GUEST_ACTIVITY, 0ULL);
    KswordARKHvmNestedProbeVmcs12Write(
        KSW_PROBE_GUEST_INTERRUPTIBILITY,
        0ULL);
    /* Guest segments, resolved from the descriptor tables we are using. */
    (void)KswordARKHvmReadSegment(&snapshot, snapshot.Es, &segment);
    KswordARKHvmNestedProbeWriteSegment(0x0800UL, &segment);
    (void)KswordARKHvmReadSegment(&snapshot, snapshot.Cs, &segment);
    KswordARKHvmNestedProbeWriteSegment(0x0802UL, &segment);
    (void)KswordARKHvmReadSegment(&snapshot, snapshot.Ss, &segment);
    KswordARKHvmNestedProbeWriteSegment(0x0804UL, &segment);
    (void)KswordARKHvmReadSegment(&snapshot, snapshot.Ds, &segment);
    KswordARKHvmNestedProbeWriteSegment(0x0806UL, &segment);
    (void)KswordARKHvmReadSegment(&snapshot, snapshot.Fs, &segment);
    KswordARKHvmNestedProbeWriteSegment(0x0808UL, &segment);
    (void)KswordARKHvmReadSegment(&snapshot, snapshot.Gs, &segment);
    /* GS base lives in an MSR, not in the descriptor the selector names. */
    segment.Base = __readmsr(0xC0000101UL);
    KswordARKHvmNestedProbeWriteSegment(0x080AUL, &segment);
    (void)KswordARKHvmReadSegment(&snapshot, snapshot.Ldtr, &segment);
    KswordARKHvmNestedProbeWriteSegment(0x080CUL, &segment);
    (void)KswordARKHvmReadSegment(&snapshot, snapshot.Tr, &segment);
    KswordARKHvmNestedProbeWriteSegment(0x080EUL, &segment);
    /* Descriptor tables. */
    KswordARKHvmNestedProbeVmcs12Write(0x4810UL, snapshot.Gdtr.Limit);
    KswordARKHvmNestedProbeVmcs12Write(0x6816UL, snapshot.Gdtr.Base);
    KswordARKHvmNestedProbeVmcs12Write(0x4812UL, snapshot.Idtr.Limit);
    KswordARKHvmNestedProbeVmcs12Write(0x6818UL, snapshot.Idtr.Base);
    /* Where L2 starts, and the stack it starts on. */
    KswordARKHvmNestedProbeVmcs12Write(
        KSW_PROBE_VMCS_GUEST_RIP,
        (ULONGLONG)(ULONG_PTR)Probe->L2CodeVirtual);
    KswordARKHvmNestedProbeVmcs12Write(
        KSW_PROBE_GUEST_RSP,
        (ULONGLONG)(ULONG_PTR)Probe->L2CodeVirtual + (PAGE_SIZE - 256ULL));
    KswordARKHvmNestedProbeVmcs12Write(KSW_PROBE_GUEST_RFLAGS, 0x2ULL);
    /* Host state: where L1 wants control back, and on which stack. */
    KswordARKHvmNestedProbeVmcs12Write(0x0C00UL, snapshot.Es & ~7U);
    KswordARKHvmNestedProbeVmcs12Write(0x0C02UL, snapshot.Cs & ~7U);
    KswordARKHvmNestedProbeVmcs12Write(0x0C04UL, snapshot.Ss & ~7U);
    KswordARKHvmNestedProbeVmcs12Write(0x0C06UL, snapshot.Ds & ~7U);
    KswordARKHvmNestedProbeVmcs12Write(0x0C08UL, snapshot.Fs & ~7U);
    KswordARKHvmNestedProbeVmcs12Write(0x0C0AUL, snapshot.Gs & ~7U);
    KswordARKHvmNestedProbeVmcs12Write(0x0C0CUL, snapshot.Tr & ~7U);
    KswordARKHvmNestedProbeVmcs12Write(0x6C00UL, __readcr0());
    KswordARKHvmNestedProbeVmcs12Write(0x6C02UL, __readcr3());
    KswordARKHvmNestedProbeVmcs12Write(0x6C04UL, __readcr4());
    KswordARKHvmNestedProbeVmcs12Write(0x6C06UL, __readmsr(0xC0000100UL));
    KswordARKHvmNestedProbeVmcs12Write(0x6C08UL, __readmsr(0xC0000101UL));
    (void)KswordARKHvmReadSegment(&snapshot, snapshot.Tr, &segment);
    KswordARKHvmNestedProbeVmcs12Write(0x6C0AUL, segment.Base);
    KswordARKHvmNestedProbeVmcs12Write(0x4C00UL, __readmsr(0x174UL));
    KswordARKHvmNestedProbeVmcs12Write(0x6C10UL, __readmsr(0x175UL));
    KswordARKHvmNestedProbeVmcs12Write(0x6C12UL, __readmsr(0x176UL));
    KswordARKHvmNestedProbeVmcs12Write(0x6C0CUL, snapshot.Gdtr.Base);
    KswordARKHvmNestedProbeVmcs12Write(0x6C0EUL, snapshot.Idtr.Base);
    KswordARKHvmNestedProbeVmcs12Write(
        KSW_PROBE_HOST_RIP,
        (ULONGLONG)(ULONG_PTR)&KswordARKHvmNestedProbeL1Host);
    KswordARKHvmNestedProbeVmcs12Write(
        KSW_PROBE_HOST_RSP,
        (((ULONGLONG)(ULONG_PTR)Probe->L1StackVirtual +
            KSW_PROBE_L1_STACK_BYTES - 256ULL) & ~0xFULL) - 8ULL);
    return !slot->ConfigurationFailed;
}

/*
 * Run the instruction sequence on the processor this thread is pinned to.
 *
 * Every step records the architectural result rather than stopping at the
 * first failure: a VMPTRLD that fails after a VMXON that succeeded is a
 * different and more interesting report than "the probe failed".
 */
static VOID
KswordARKHvmNestedProbeExecute(
    _Inout_ KSW_HVM_NESTED_PROBE_CONTEXT* Probe
    )
{
    KSWORD_ARK_HVM_NESTED_PROBE_ROW* response = Probe->Response;
    /*
     * Taken after the affinity is already set, so it is this processor's own.
     * NULL means an index past the slot table, in which case the L2 section is
     * skipped rather than run without a way to come back.
     */
    KSW_HVM_PROBE_SLOT* slot = KswordARKHvmNestedProbeSlot();
    KSW_HVM_RESIDENT_VCPU* vcpu = KswordARKHvmResidentFindCurrent();
    ULONGLONG originalCr4 = __readcr4();
    ULONGLONG vmxonPhysical = Probe->VmxonPhysical;
    ULONGLONG vmcs12Physical = Probe->Vmcs12Physical;
    ULONGLONG readBack = 0ULL;
    ULONGLONG storedPointer = 0ULL;
    ULONGLONG startingCount = 0ULL;
    UCHAR clearResult = 0U;

    response->processorIndex =
        (ULONG)KeGetCurrentProcessorNumberEx(NULL);
    /*
     * Refuse without an armed nested dispatch on *this* processor.
     *
     * Not caution - necessity.  With dispatch disabled the exit handler
     * injects #UD for a VMX instruction, and that #UD lands on the kernel code
     * three lines below.  There is no recovering from it, so the only place to
     * stop is before the first instruction.
     */
    if (vcpu == NULL ||
        !vcpu->Nested.Enabled ||
        InterlockedCompareExchange(&vcpu->Active, 0L, 0L) == 0L) {
        response->status = KSWORD_ARK_HVM_NESTED_PROBE_STATUS_NOT_ARMED;
        /* Return without executing anything. */
        return;
    }
    startingCount = vcpu->Nested.InstructionCount;
    /*
     * Set CR4.VMXE and read it back.
     *
     * A write that our own CR policy declines leaves the bit clear, and VMXON
     * with it clear is #UD - again on our own kernel code.  The read-back is
     * the gate, not the write.
     */
    __writecr4(originalCr4 | KSW_PROBE_CR4_VMXE);
    if ((__readcr4() & KSW_PROBE_CR4_VMXE) == 0ULL) {
        response->status =
            KSWORD_ARK_HVM_NESTED_PROBE_STATUS_VMXE_REFUSED;
        /* Return without executing anything. */
        return;
    }
    response->vmwriteValue = KSW_PROBE_PATTERN;
    /* Enter emulated L1 VMX operation. */
    response->vmxonResult = (ULONG)__vmx_on(&vmxonPhysical);
    if (response->vmxonResult == 0UL) {
        /* Make our probe vmcs12 current. */
        response->vmptrldResult =
            (ULONG)__vmx_vmptrld(&vmcs12Physical);
        if (response->vmptrldResult == 0UL) {
            /*
             * VMXOFF preserves launch state. An allocator can reuse the same
             * physical page on the next probe, so a first VMLAUNCH requires
             * VMCLEAR even when our newly allocated buffer was zeroed.
             * Load first so VMCLEAR also flushes the current cached copy.
             */
            clearResult = __vmx_vmclear(&vmcs12Physical);
            if (clearResult == 0U) {
                response->vmptrldResult =
                    (ULONG)__vmx_vmptrld(&vmcs12Physical);
            } else {
                SIZE_T clearError = 0U;

                if (__vmx_vmread(0x4400U, &clearError) == 0) {
                    response->lastInstructionError = (ULONG)clearError;
                }
            }
        }
        if (response->vmptrldResult == 0UL && clearResult == 0U) {
            /* Write a recognizable value into a field and read it back. */
            response->vmwriteResult = (ULONG)__vmx_vmwrite(
                (SIZE_T)KSW_PROBE_VMCS_GUEST_RIP,
                (SIZE_T)KSW_PROBE_PATTERN);
            response->vmreadResult = (ULONG)__vmx_vmread(
                (SIZE_T)KSW_PROBE_VMCS_GUEST_RIP,
                (SIZE_T*)&readBack);
            response->vmreadValue = readBack;
            response->vmreadMatched =
                (response->vmwriteResult == 0UL &&
                 response->vmreadResult == 0UL &&
                 readBack == KSW_PROBE_PATTERN) ? 1UL : 0UL;
            /*
             * Ask which VMCS is current and compare with what we loaded.
             *
             * VMPTRST has no failure encoding - it always stores - so the
             * result slot records that it ran, and the comparison below is
             * the actual judgement.
             */
            __vmx_vmptrst(&storedPointer);
            response->vmptrstResult = 0UL;
            response->vmptrstMatched =
                (storedPointer == Probe->Vmcs12Physical) ? 1UL : 0UL;
            /*
             * Two vmcs12 regions, alternated.
             *
             * Everything above uses one VMCS and therefore cannot see whether
             * a second one exists.  A real hypervisor keeps several - one per
             * vCPU at minimum - and VMPTRLDs between them constantly, so
             * "fields survive a switch" is a prerequisite for hosting one, not
             * an optimization.
             *
             * The sequence is deliberately the smallest that can fail:
             * write A, write B, read A, read B.  A dispatcher that models a
             * single vmcs12 returns B's value (or zero) for A, and nothing
             * else in this probe would notice.
             */
            {
                ULONGLONG secondPhysical = Probe->Vmcs12bPhysical;
                ULONGLONG firstPhysical = Probe->Vmcs12Physical;
                ULONGLONG backA = 0ULL;
                ULONGLONG backB = 0ULL;

                response->vmcsSwitchResult =
                    KSWORD_ARK_HVM_NESTED_PROBE_STEP_SKIPPED;
                response->vmcsSwitchMatched = 0UL;
                if (secondPhysical != 0ULL) {
                    ULONG step = 0UL;

                    /* A already holds KSW_PROBE_PATTERN from the write above. */
                    step |= (ULONG)__vmx_vmptrld(&secondPhysical);
                    step |= (ULONG)__vmx_vmwrite(
                        (SIZE_T)KSW_PROBE_VMCS_GUEST_RIP,
                        (SIZE_T)KSW_PROBE_PATTERN_B);
                    step |= (ULONG)__vmx_vmptrld(&firstPhysical);
                    step |= (ULONG)__vmx_vmread(
                        (SIZE_T)KSW_PROBE_VMCS_GUEST_RIP,
                        (SIZE_T*)&backA);
                    step |= (ULONG)__vmx_vmptrld(&secondPhysical);
                    step |= (ULONG)__vmx_vmread(
                        (SIZE_T)KSW_PROBE_VMCS_GUEST_RIP,
                        (SIZE_T*)&backB);
                    /* Leave the first one current for everything downstream. */
                    step |= (ULONG)__vmx_vmptrld(&firstPhysical);
                    response->vmcsSwitchResult = step;
                    response->vmcsSwitchValueA = backA;
                    response->vmcsSwitchValueB = backB;
                    response->vmcsSwitchMatched =
                        (step == 0UL &&
                         backA == KSW_PROBE_PATTERN &&
                         backB == KSW_PROBE_PATTERN_B) ? 1UL : 0UL;
                }
            }
            /*
             * How deep the pool really is, and whether overflow is recorded.
             *
             * The two-region test above proves only "more than one".  The
             * number we actually hold has been an assertion in a header
             * comment with nothing behind it, and the eviction counter has
             * never been seen to move - a counter nobody has observed moving
             * is not a verified readout, it is a hope.
             *
             * Write a distinct value into each of N regions, then read them
             * back **most recently used first**.  The order is load-bearing:
             * reading oldest-first makes each read evict the next region it
             * was about to check, so everything reads back zero and a working
             * pool is indistinguishable from no pool at all.  Reading newest
             * first only ever re-saves a region the pool already holds, which
             * costs no slot.
             *
             * Runs before the vmcs12 is built for L2, because it will very
             * likely evict that vmcs12 - by design, since overflow is the
             * point - and the build rewrites every field it needs afterwards.
             */
            if (Probe->DepthBlockPhysical != 0ULL) {
                /*
                 * This processor's own count, not the runtime's.
                 *
                 * Every processor runs a worker at the same time, so a delta
                 * taken from the shared total would report the other cores'
                 * evictions in this core's row - a number that looks precise
                 * and means something else.
                 */
                const ULONG evictionsBefore =
                    vcpu->Nested.Vmcs12EvictionCount;
                ULONGLONG mask = 0ULL;
                ULONG survived = 0UL;
                ULONG index = 0UL;

                for (index = 0UL; index < KSW_PROBE_DEPTH_REGIONS; ++index) {
                    ULONGLONG region = Probe->DepthBlockPhysical +
                        ((ULONGLONG)index * PAGE_SIZE);

                    if (__vmx_vmptrld(&region) != 0) { break; }
                    (void)__vmx_vmwrite(
                        (SIZE_T)KSW_PROBE_VMCS_GUEST_RIP,
                        (SIZE_T)KSW_PROBE_DEPTH_PATTERN(index));
                }
                /* Walk back down, newest first, for the reason above. */
                for (index = KSW_PROBE_DEPTH_REGIONS; index > 0UL; --index) {
                    const ULONG slotIndex = index - 1UL;
                    ULONGLONG region = Probe->DepthBlockPhysical +
                        ((ULONGLONG)slotIndex * PAGE_SIZE);
                    ULONGLONG back = 0ULL;

                    if (__vmx_vmptrld(&region) != 0) { continue; }
                    if (__vmx_vmread(
                            (SIZE_T)KSW_PROBE_VMCS_GUEST_RIP,
                            (SIZE_T*)&back) != 0) {
                        continue;
                    }
                    if (back == KSW_PROBE_DEPTH_PATTERN(slotIndex)) {
                        mask |= (1ULL << slotIndex);
                        survived += 1UL;
                    }
                }
                /* Put the probe's own vmcs12 back before anything else runs. */
                (void)__vmx_vmptrld(&Probe->Vmcs12Physical);
                response->vmcs12DepthRegions = KSW_PROBE_DEPTH_REGIONS;
                response->vmcs12DepthSurvived = survived;
                response->vmcs12DepthMask = mask;
                response->vmcs12EvictionDelta =
                    vcpu->Nested.Vmcs12EvictionCount - evictionsBefore;
                /*
                 * Take this test's own evictions back out of the durable total.
                 *
                 * That counter exists to say "some L1 kept more VMCSs than we
                 * hold" - it is what turns a hypervisor misbehaving under us
                 * into something readable afterwards.  This test overflows the
                 * pool on purpose, so leaving its evictions in would raise that
                 * alarm on a machine where nothing is wrong, every time the
                 * probe runs.  A warning that fires on its own test is worth
                 * nothing the first time someone has to decide whether to
                 * believe it.
                 *
                 * Subtracted rather than suppressed at the source: the exit
                 * path stays free of any notion of who triggered it, and each
                 * processor removes exactly what it added.  The row above
                 * keeps the real number, because there the eviction is the
                 * result being reported rather than an alarm.
                 */
                if (response->vmcs12EvictionDelta != 0UL) {
                    (void)InterlockedExchangeAdd(
                        (volatile LONG*)&vcpu->Runtime
                            ->NestedVmcs12EvictionCount,
                        -(LONG)response->vmcs12EvictionDelta);
                }
            }
            /*
             * Only attempt L2 once the field plumbing demonstrably works.
             *
             * A VMLAUNCH built on a vmcs12 whose writes are not landing would
             * fail on guest state and report a field problem, which is a true
             * statement about the wrong layer.
             */
            if (response->vmreadMatched == 1UL && slot != NULL) {
                if (!KswordARKHvmNestedProbeBuildVmcs12(Probe)) {
                    __vmx_off();
                    __writecr4(originalCr4);
                    response->status = KSWORD_ARK_HVM_NESTED_PROBE_STATUS_CONFIGURATION_FAILED;
                    response->vmxoffResult = 0UL;
                    return;
                }
                slot->L2ExitReason = 0ULL;
                slot->L2Qualification = 0ULL;
                slot->L2GuestRip = 0ULL;
                slot->InvalidationResponse = response;
                slot->InvalidationRoot =
                    (volatile ULONGLONG*)Probe->Ept12Pml4Virtual;
                slot->InvalidationEptp = Probe->Ept12Pointer;
                InterlockedExchange(&slot->L2Exited, 0L);
                InterlockedExchange(&slot->SelfStage, 0L);
                InterlockedExchange(&slot->L2SelfMarker, 0L);
                InterlockedExchange(&slot->L2ResumeCount, 0L);
                InterlockedExchange(&slot->L2AsyncResumeCount, 0L);
                InterlockedExchange(&slot->FxPassed, 0L);
                /* Baseline the cumulative counters, so the run reports a delta. */
                slot->BaseEntryCount = vcpu->Nested.L2EntryCount;
                slot->BaseReflectCount = vcpu->Nested.L2ExitReflectedCount;
                slot->BaseTotalExitCount = vcpu->Nested.L2ExitTotalCount;
                g_KswordProbeL2Marker = 0L;
                g_KswordProbeL2PassThrough = 0L;
                RtlCaptureContext(&slot->ResumeContext);
                /*
                 * Self-virtualization: L1 makes *this* context its guest.
                 *
                 * This is the difference between hosting a hypervisor and
                 * hosting a test program.  A real one - our own resident path,
                 * VMware's VMM - captures the processor's state, points the
                 * VMCS's guest RIP back at its own next instruction, and
                 * launches, so that it becomes its own guest.  The synthetic
                 * L2 above runs on a made-up RIP and stack where the segments,
                 * CR3 and page tables never have to be real.
                 *
                 * L2 is never asked to work out where it is.
                 *
                 * An earlier shape had one capture point reached three times,
                 * with L2 reading two shared flags to decide which arrival it
                 * was.  That cannot work: L2 resumes with L1's registers as of
                 * L1's VMLAUNCH, so the pointers those reads go through are
                 * not the ones the source assumes, and the branch is decided
                 * by whatever happened to be in a register.  Measured - a
                 * store and the adjacent load of one volatile global
                 * disagreed, which only happens when the store is not the
                 * instruction that ran.
                 *
                 * Now the launcher returns 0 to a caller that has become L2
                 * and non-zero when the entry failed, so "am I L2" is answered
                 * by the calling convention.  The capture point is still here,
                 * but only L1 ever evaluates it: once before the launch, and
                 * once more when the host handler restores the context.
                 */
                if (Probe->SelfVirtualize) {
                    response->selfVirtAttempted = 1UL;
                    response->selfVirtEntryRip =
                        KswordARKHvmAsmProbeL2ResumePoint();
                    if (InterlockedCompareExchange(
                            &slot->L2Exited, 0L, 0L) != 0L) {
                        /* Third arrival: L1 handled L2's exit and returned. */
                        response->selfVirtReturnedToL1 = 1UL;
                        response->vmlaunchResult = 0UL;
                        response->l2Reached = 1UL;
                        /*
                         * Report what L2 left behind, not what L2 tried to
                         * write into this row.  Registers are whole again
                         * here, so this write is the one that can be trusted.
                         */
                        /*
                         * Two independent witnesses, reported separately.
                         *
                         * The global says whether L2's stores reach memory at
                         * all; the slot says whether the path L2 took to find
                         * its own slot worked.  Folding them together would
                         * turn two different failures into one zero.
                         */
                        response->selfVirtReachedL2 =
                            (g_KswordProbeL2Marker != 0L) ? 1UL : 0UL;
                        response->selfVirtSlotMarker =
                            (InterlockedCompareExchange(
                                &slot->L2SelfMarker, 0L, 0L) != 0L)
                                ? 1UL : 0UL;
                        response->selfVirtCpuidPassedThrough =
                            (g_KswordProbeL2PassThrough != 0L) ? 1UL : 0UL;
                    } else {
                        /*
                         * First arrival: still L1, about to enter.
                         *
                         * Enter with interrupts masked.  The captured RFLAGS
                         * has IF set because this runs at PASSIVE_LEVEL, and
                         * carrying that into L2 would let the first clock
                         * interrupt be delivered there - handler, scheduler
                         * and the next thread all one level deeper, with
                         * nothing ever reaching the VMXOFF.  That is what
                         * residency does on purpose and what a transient L1
                         * must not do.  Bit 1 is the reserved always-one bit
                         * VM entry requires.
                         *
                         * RIP and RSP are the launcher's business now: it is
                         * the only place that knows where L2 has to resume.
                         */
                        KswordARKHvmNestedProbeVmcs12Write(
                            KSW_PROBE_GUEST_RFLAGS,
                            ((ULONGLONG)slot->ResumeContext.EFlags &
                                ~0x200ULL) | 0x2ULL);
                    if (KswordARKHvmAsmProbeLaunchL2(&slot->FxPassed) == 0UL) {
                        /*
                         * The launcher returned zero, so this code is now
                         * executing as L2 - same instructions, same stack,
                         * same registers, one privilege domain lower.
                         */
                        int registers[4] = { 0 };

                        /*
                         * Leave a mark, then leave L2.
                         *
                         * The mark is RIP-relative, so it depends on nothing
                         * inherited; CPUID exits unconditionally and the
                         * routing default hands it to L1.  Whether the mark
                         * survives is read by L1 afterwards - which is a
                         * question about L2's stores, now that "did L2 run
                         * this code" is answered by the return value instead
                         * of by the mark itself.
                         */
                        g_KswordProbeL2Marker = 1L;
                        /*
                         * Second witness, now that it means something.
                         *
                         * Finding the slot goes through GS, so this tests the
                         * segment base L2 inherited rather than just its
                         * stores.  It was pointless while L2's branching was
                         * unreliable; with the launcher it is a real check of
                         * a different thing.
                         */
                        {
                            KSW_HVM_PROBE_SLOT* live =
                                KswordARKHvmNestedProbeSlot();

                            if (live != NULL) {
                                InterlockedExchange(&live->L2SelfMarker, 1L);
                            }
                        }
                        /*
                         * Go round the loop, then leave for good.
                         *
                         * Each CPUID exits and is reflected; L1's handler
                         * resumes L2 with VMRESUME and execution continues
                         * here, one iteration further on.  The loop counter
                         * lives in L2's own registers and stack, so surviving
                         * eight trips is also a statement that the guest state
                         * vmcs12 carries back and forth is intact.
                         *
                         * The run ends on the last CPUID, not on the VMCALL
                         * below: the handler stops resuming once the trip
                         * count is spent, so that exit falls through to its
                         * VMXOFF.  The VMCALL is only reachable if CPUID
                         * stopped leaving L2, which is why it sits after the
                         * marker rather than instead of it.
                         */
                        {
                            LONG trip = 0L;

                            for (trip = 0L;
                                 trip <= KSW_PROBE_SELF_ROUND_TRIPS;
                                 ++trip) {
                                __cpuid(registers, 0);
                            }
                        }
                        /*
                         * Reached only if CPUID stopped leaving L2, which is
                         * architecturally impossible - recorded through a
                         * volatile so it cannot be scheduled above the
                         * instruction it is about.
                         */
                        g_KswordProbeL2PassThrough = 1L;
                        (void)KswordARKHvmAsmResidentHypercall(0ULL, 0ULL);
                    } else {
                        /* The entry did not happen; say why. */
                        SIZE_T launchError = 0U;

                        response->vmlaunchResult = 1UL;
                        if (__vmx_vmread(0x4400U, &launchError) == 0) {
                            response->lastInstructionError =
                                (ULONG)launchError;
                        }
                    }
                    }
                    response->selfVirtEntryCount =
                        (ULONG)(vcpu->Nested.L2EntryCount -
                            slot->BaseEntryCount);
                    response->selfVirtReflectCount =
                        (ULONG)(vcpu->Nested.L2ExitReflectedCount -
                            slot->BaseReflectCount);
                    response->selfVirtTotalExitCount =
                        (ULONG)(vcpu->Nested.L2ExitTotalCount -
                            slot->BaseTotalExitCount);
                    response->selfVirtResumeCount =
                        (ULONG)InterlockedCompareExchange(
                            &slot->L2ResumeCount, 0L, 0L);
                    response->selfVirtExitReason = slot->L2ExitReason;
                    response->selfVirtGuestRip = slot->L2GuestRip;
                    /*
                     * The fuse's verdict, read whether or not it tripped.
                     *
                     * Reported on every arrival rather than only the third:
                     * if the fuse is what ended the run, the third arrival is
                     * exactly the one that happens, and if it did not trip the
                     * zeroes say so.
                     */
                    response->l2FuseTripped =
                        vcpu->Nested.L2FuseTripped ? 1UL : 0UL;
                    response->l2FuseReason = vcpu->Nested.L2FuseReason;
                    response->l2FuseCount = vcpu->Nested.L2FuseCount;
                    response->l2FuseRip = vcpu->Nested.L2FuseRip;
                } else if (InterlockedCompareExchange(
                        &slot->L2Exited, 0L, 0L) == 0L) {
                    response->vmlaunchResult =
                        (ULONG)__vmx_vmlaunch();
                    /*
                     * A failed VMLAUNCH leaves its reason in vmcs12's
                     * VM-instruction-error field, and reading it back through
                     * VMREAD is both the architectural way to ask and a second
                     * check that our dispatch delivered the number.  Read it
                     * here rather than from the driver's own record, which the
                     * VMXOFF below would overwrite before anyone looks.
                     */
                    if (response->vmlaunchResult != 0UL) {
                        SIZE_T launchError = 0U;

                        if (__vmx_vmread(0x4400U, &launchError) == 0) {
                            response->lastInstructionError =
                                (ULONG)launchError;
                        }
                    }
                } else {
                    /* Reached only by the restore: L2 ran and came back. */
                    response->vmlaunchResult = 0UL;
                    response->l2Reached = 1UL;
                }
                response->l2ExitReason = slot->L2ExitReason;
                response->l2Qualification = slot->L2Qualification;
                response->l2GuestRip = slot->L2GuestRip;
                /*
                 * The handler already executed VMXOFF on its own stack, so
                 * the sequence below must not do it twice.
                 */
                if (response->l2Reached != 0UL) {
                    response->vmxoffResult = 0UL;
                    __writecr4(originalCr4);
                    response->dispatchedInstructions =
                        vcpu->Nested.InstructionCount - startingCount;
                    response->nestedStateAfter = vcpu->Nested.State;
                    response->lastInstructionError =
                        vcpu->Nested.LastInstructionError;
                    response->shadowFillCount =
                        vcpu->Nested.ShadowEpt.FillCount;
                    response->shadowDenyCount =
                        vcpu->Nested.ShadowEpt.DenyCount;
                    response->shadowExhaustionCount =
                        vcpu->Nested.ShadowEpt.ExhaustionCount;
                    /*
                     * Report what vmcs02 actually carried into VM entry.
                     *
                     * Captured by L2Enter from the loaded VMCS, so a field the
                     * merge never wrote shows up as whatever the VMCS already
                     * held rather than as the value the merge believed it set.
                     */
                    response->vmcs02PrimaryControls =
                        vcpu->Nested.LastEntryPrimaryControls;
                    response->vmcs02SecondaryControls =
                        vcpu->Nested.LastEntrySecondaryControls;
                    response->vmcs02MsrBitmap =
                        vcpu->Nested.LastEntryMsrBitmap;
                    response->vmcs02IoBitmapA =
                        vcpu->Nested.LastEntryIoBitmapA;
                    response->vmcs02IoBitmapB =
                        vcpu->Nested.LastEntryIoBitmapB;
                    /* The newly propagated fields, from the same readback. */
                    response->vmcs02TscOffset =
                        vcpu->Nested.LastEntryTscOffset;
                    response->vmcs02EntryMsrLoadAddress =
                        vcpu->Nested.LastEntryMsrLoadAddress;
                    response->vmcs02EntryMsrLoadCount =
                        vcpu->Nested.LastEntryMsrLoadCount;
                    response->vmcs02ExitMsrStoreAddress =
                        vcpu->Nested.LastEntryMsrStoreAddress;
                    response->vmcs02ExitMsrStoreCount =
                        vcpu->Nested.LastEntryMsrStoreCount;
                    /*
                     * Where L2 stopped, relative to its own code page.
                     *
                     * Reported as an offset rather than an address because the
                     * criterion is positional: the page moves every run, but
                     * "stopped on the second RDMSR" does not.
                     */
                    response->l2RipOffset =
                        (slot->L2GuestRip >=
                            (ULONGLONG)(ULONG_PTR)Probe->L2CodeVirtual)
                            ? (slot->L2GuestRip -
                                (ULONGLONG)(ULONG_PTR)Probe->L2CodeVirtual)
                            : 0ULL;
                    response->l2MsrExitsReflected =
                        vcpu->Nested.L2MsrExitsReflected;
                    response->l2MsrExitsHandled =
                        vcpu->Nested.L2MsrExitsHandled;
                    response->l2IoExitsReflected =
                        vcpu->Nested.L2IoExitsReflected;
                    response->l2IoExitsHandled =
                        vcpu->Nested.L2IoExitsHandled;
                    response->bitmapMergeComplete =
                        vcpu->Nested.L2BitmapMergeComplete ? 1UL : 0UL;
                    response->l1UsesMsrBitmap =
                        vcpu->Nested.L2MsrFilterFromL1 ? 1UL : 0UL;
                    response->l2MergeCycles =
                        vcpu->Nested.L2MergeCycles;
                    response->l2EntryCycles =
                        vcpu->Nested.L2EntryCycles;
                    response->l2EntryCount =
                        vcpu->Nested.L2EntryCount;
                    /*
                     * The A/D readout has to be filled on **this** path too.
                     *
                     * It was only in the fall-through below, which is the path
                     * taken when L2 did not run. So every successful run -
                     * exactly the runs the A/D case cares about - reported
                     * zeroes, and the feature looked broken when the
                     * instrumentation was.
                     */
                    response->l1RequestedAccessedDirty =
                        vcpu->Nested.ShadowEpt.L1RequestedAccessedDirty
                            ? 1UL : 0UL;
                    response->accessedDirtyActive =
                        vcpu->Nested.ShadowEpt.AccessedDirtyActive ? 1UL : 0UL;
                    response->adPropagatedCount =
                        vcpu->Nested.ShadowEpt.AdPropagatedCount;
                    response->adOverflowCount =
                        vcpu->Nested.ShadowEpt.AdOverflowCount;
                    response->status =
                        KSWORD_ARK_HVM_NESTED_PROBE_STATUS_OK;
                    /* Return without a second VMXOFF. */
                    return;
                }
            }
        }
        /* Leave VMX operation however far the sequence got. */
        __vmx_off();
        response->vmxoffResult = 0UL;
    }
    /* Put CR4 back exactly as it was found. */
    __writecr4(originalCr4);
    response->dispatchedInstructions =
        vcpu->Nested.InstructionCount - startingCount;
    response->nestedStateAfter = vcpu->Nested.State;
    /*
     * Name the refusal, since the error code cannot.
     *
     * L1 sees a generic control-field failure, which is architecturally
     * correct and says nothing about which control. This is the only place
     * that distinguishes "accessed/dirty is not offered here" from "the EPT
     * pointer was malformed" - both arrive as the same number.
     */
    response->l1RequestedAccessedDirty =
        vcpu->Nested.ShadowEpt.L1RequestedAccessedDirty ? 1UL : 0UL;
    response->accessedDirtyActive =
        vcpu->Nested.ShadowEpt.AccessedDirtyActive ? 1UL : 0UL;
    response->adPropagatedCount =
        vcpu->Nested.ShadowEpt.AdPropagatedCount;
    response->adOverflowCount =
        vcpu->Nested.ShadowEpt.AdOverflowCount;
    /*
     * Do not overwrite an error already captured at the failing instruction.
     *
     * The per-VCPU record holds only the *last* error, and the VMXOFF above
     * succeeds - so by the time this runs the record says zero, which would
     * turn a reported failure into "failed, no reason given".
     */
    if (response->lastInstructionError == 0UL) {
        response->lastInstructionError =
            vcpu->Nested.LastInstructionError;
    }
    response->status = KSWORD_ARK_HVM_NESTED_PROBE_STATUS_OK;
}

/*
 * Run one complete probe on one processor and fill one row.
 *
 * Every resource is allocated here rather than shared, because in the
 * all-processors mode several of these run at once and a shared VMXON region
 * or vmcs12 would be two processors writing one VMX structure.
 */
static NTSTATUS
KswordARKHvmNestedProbeRunOne(
    _Out_ KSWORD_ARK_HVM_NESTED_PROBE_ROW* Response,
    _In_ const PROCESSOR_NUMBER* TargetProcessor,
    _In_ BOOLEAN RequestAccessedDirty,
    _In_ BOOLEAN SelfVirtualize
    )
{
    const BOOLEAN requestAccessedDirty = RequestAccessedDirty;
    const BOOLEAN selfVirtualize = SelfVirtualize;
    KSW_HVM_NESTED_PROBE_CONTEXT probe = { 0 };
    PHYSICAL_ADDRESS lowest = { 0 };
    PHYSICAL_ADDRESS highest = { 0 };
    PHYSICAL_ADDRESS boundary = { 0 };
    PHYSICAL_ADDRESS physical = { 0 };
    ULONG revision = 0UL;
    KAFFINITY affinity = 0;
    GROUP_AFFINITY target = { 0 };
    GROUP_AFFINITY previous = { 0 };
    PROCESSOR_NUMBER processorNumber = *TargetProcessor;

    RtlZeroMemory(Response, sizeof(*Response));
    /* Start every step at "did not execute" so silence is never success. */
    Response->vmxonResult = KSWORD_ARK_HVM_NESTED_PROBE_STEP_SKIPPED;
    Response->vmptrldResult = KSWORD_ARK_HVM_NESTED_PROBE_STEP_SKIPPED;
    Response->vmwriteResult = KSWORD_ARK_HVM_NESTED_PROBE_STEP_SKIPPED;
    Response->vmreadResult = KSWORD_ARK_HVM_NESTED_PROBE_STEP_SKIPPED;
    Response->vmptrstResult = KSWORD_ARK_HVM_NESTED_PROBE_STEP_SKIPPED;
    Response->vmxoffResult = KSWORD_ARK_HVM_NESTED_PROBE_STEP_SKIPPED;
    Response->vmlaunchResult = KSWORD_ARK_HVM_NESTED_PROBE_STEP_SKIPPED;
    Response->inveptResult = KSWORD_ARK_HVM_NESTED_PROBE_STEP_SKIPPED;
    revision = (ULONG)(__readmsr(KSW_PROBE_IA32_VMX_BASIC) & 0x7FFFFFFFULL);
    /*
     * Record what a guest reading the capability MSRs gets right now.
     *
     * This runs in guest context with residency up, so these two RDMSRs take
     * the same path any other guest's would: bitmap, exit, filter.  The query
     * IOCTL reports the same MSRs sampled at driver load, before any of that -
     * so the pair of readouts is a before/after with the filter in between,
     * and the filter being inert shows up as the two being identical.
     *
     * Chosen because they carry the features most likely to be enabled by an
     * L1 and least likely to be implemented by us: secondary controls holds
     * VPID, VMFUNC and VMCS shadowing; the EPT/VPID capability holds the
     * INVVPID forms.
     */
    Response->guestVmxProcbased2 =
        __readmsr(KSWORD_ARK_HVM_VMX_MSR_PROCBASED2);
    Response->guestVmxEptVpidCap =
        __readmsr(KSWORD_ARK_HVM_VMX_MSR_EPT_VPID_CAP);
    highest.QuadPart = MAXLONGLONG;
    probe.VmxonVirtual = MmAllocateContiguousMemorySpecifyCache(
        PAGE_SIZE, lowest, highest, boundary, MmCached);
    probe.Vmcs12Virtual = MmAllocateContiguousMemorySpecifyCache(
        PAGE_SIZE, lowest, highest, boundary, MmCached);
    probe.Vmcs12bVirtual = MmAllocateContiguousMemorySpecifyCache(
        PAGE_SIZE, lowest, highest, boundary, MmCached);
    probe.DepthBlockVirtual = MmAllocateContiguousMemorySpecifyCache(
        (SIZE_T)KSW_PROBE_DEPTH_REGIONS * PAGE_SIZE,
        lowest, highest, boundary, MmCached);
    probe.MsrAreaVirtual = MmAllocateContiguousMemorySpecifyCache(
        PAGE_SIZE, lowest, highest, boundary, MmCached);
    probe.L2CodeVirtual = MmAllocateContiguousMemorySpecifyCache(
        PAGE_SIZE, lowest, highest, boundary, MmCached);
    probe.Ept12Pml4Virtual = MmAllocateContiguousMemorySpecifyCache(
        PAGE_SIZE, lowest, highest, boundary, MmCached);
    probe.Ept12PdptVirtual = MmAllocateContiguousMemorySpecifyCache(
        PAGE_SIZE, lowest, highest, boundary, MmCached);
    probe.L1MsrBitmapVirtual = MmAllocateContiguousMemorySpecifyCache(
        PAGE_SIZE, lowest, highest, boundary, MmCached);
    probe.L1StackVirtual = KswordARKAllocateNonPagedPool(
        KSW_PROBE_L1_STACK_BYTES,
        'pnHK');
    if (probe.VmxonVirtual == NULL || probe.Vmcs12Virtual == NULL ||
        probe.L2CodeVirtual == NULL || probe.L1StackVirtual == NULL ||
        probe.Ept12Pml4Virtual == NULL || probe.Ept12PdptVirtual == NULL ||
        probe.L1MsrBitmapVirtual == NULL) {
        if (probe.L1MsrBitmapVirtual != NULL) {
            MmFreeContiguousMemory(probe.L1MsrBitmapVirtual);
        }
        if (probe.Ept12Pml4Virtual != NULL) {
            MmFreeContiguousMemory(probe.Ept12Pml4Virtual);
        }
        if (probe.Ept12PdptVirtual != NULL) {
            MmFreeContiguousMemory(probe.Ept12PdptVirtual);
        }
        if (probe.VmxonVirtual != NULL) {
            MmFreeContiguousMemory(probe.VmxonVirtual);
        }
        if (probe.Vmcs12Virtual != NULL) {
            MmFreeContiguousMemory(probe.Vmcs12Virtual);
        }
        if (probe.L2CodeVirtual != NULL) {
            MmFreeContiguousMemory(probe.L2CodeVirtual);
        }
        /*
         * The optional regions too.  They are not in the condition above -
         * losing them only costs a test, not the probe - but this path still
         * owns them, and the one for the second vmcs12 was being leaked here.
         */
        if (probe.Vmcs12bVirtual != NULL) {
            MmFreeContiguousMemory(probe.Vmcs12bVirtual);
        }
        if (probe.DepthBlockVirtual != NULL) {
            MmFreeContiguousMemory(probe.DepthBlockVirtual);
        }
        if (probe.MsrAreaVirtual != NULL) {
            MmFreeContiguousMemory(probe.MsrAreaVirtual);
        }
        if (probe.L1StackVirtual != NULL) {
            ExFreePool(probe.L1StackVirtual);
        }
        Response->status =
            KSWORD_ARK_HVM_NESTED_PROBE_STATUS_NO_RESOURCES;
        /* Return the exact allocation failure. */
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(probe.VmxonVirtual, PAGE_SIZE);
    RtlZeroMemory(probe.Vmcs12Virtual, PAGE_SIZE);
    RtlZeroMemory(probe.L1StackVirtual, KSW_PROBE_L1_STACK_BYTES);
    /*
     * L1's MSR bitmap: everything clear except the read bit for the MSR the
     * program below is supposed to be stopped on.
     *
     * Region 0 (bytes 0x000-0x3FF) is the read bitmap for MSRs 0x0-0x1FFF, one
     * bit per MSR.  Clearing the page means L1 intercepts nothing; setting one
     * bit means it intercepts exactly that read.  Both halves matter: without
     * the cleared bits there is no evidence the merge honours a "do not
     * intercept", and without the set bit none that it honours an "intercept".
     */
    RtlZeroMemory(probe.L1MsrBitmapVirtual, PAGE_SIZE);
    ((volatile UCHAR*)probe.L1MsrBitmapVirtual)
        [KSW_PROBE_L2_TRAPPED_MSR / 8U] |=
            (UCHAR)(1U << (KSW_PROBE_L2_TRAPPED_MSR % 8U));
    physical = MmGetPhysicalAddress(probe.L1MsrBitmapVirtual);
    probe.L1MsrBitmapPhysical = (ULONGLONG)physical.QuadPart;
    /*
     * L2's program, chosen so that where it stops is itself the answer.
     *
     *   +0   mov ecx, IA32_TIME_STAMP_COUNTER
     *   +5   rdmsr                 <- L1's bit is clear: must NOT exit
     *   +7   mov ecx, IA32_SYSENTER_CS
     *   +12  rdmsr                 <- L1's bit is set: must exit
     *   +14  cpuid                 <- only reached if the exit never happened
     *   +16  jmp $
     *
     * Three outcomes, all distinguishable by the reflected RIP alone:
     * stopping at +12 means the processor consulted a bitmap that really was
     * L1's; stopping at +5 means it consulted one that intercepts everything,
     * which is what the merge falls back to when it cannot read L1's page;
     * reaching +14 means MSR interception did not happen at all.
     *
     * Both MSRs are architecturally present on every processor this driver
     * runs on, so the read that is allowed through cannot fault.
     */
    RtlZeroMemory(probe.L2CodeVirtual, PAGE_SIZE);
    {
        volatile UCHAR* code = (volatile UCHAR*)probe.L2CodeVirtual;

        /* mov ecx, imm32 */
        code[0] = 0xB9U;
        code[1] = (UCHAR)(KSW_PROBE_L2_OPEN_MSR & 0xFFU);
        code[2] = (UCHAR)((KSW_PROBE_L2_OPEN_MSR >> 8) & 0xFFU);
        code[3] = (UCHAR)((KSW_PROBE_L2_OPEN_MSR >> 16) & 0xFFU);
        code[4] = (UCHAR)((KSW_PROBE_L2_OPEN_MSR >> 24) & 0xFFU);
        /* rdmsr */
        code[5] = 0x0FU;
        code[6] = 0x32U;
        /* mov ecx, imm32 */
        code[7] = 0xB9U;
        code[8] = (UCHAR)(KSW_PROBE_L2_TRAPPED_MSR & 0xFFU);
        code[9] = (UCHAR)((KSW_PROBE_L2_TRAPPED_MSR >> 8) & 0xFFU);
        code[10] = (UCHAR)((KSW_PROBE_L2_TRAPPED_MSR >> 16) & 0xFFU);
        code[11] = (UCHAR)((KSW_PROBE_L2_TRAPPED_MSR >> 24) & 0xFFU);
        /* rdmsr */
        code[12] = 0x0FU;
        code[13] = 0x32U;
        /* cpuid */
        code[14] = 0x0FU;
        code[15] = 0xA2U;
        /* jmp $ */
        code[16] = 0xEBU;
        code[17] = 0xFEU;
    }
    RtlZeroMemory(probe.Ept12Pml4Virtual, PAGE_SIZE);
    RtlZeroMemory(probe.Ept12PdptVirtual, PAGE_SIZE);
    physical = MmGetPhysicalAddress(probe.Ept12Pml4Virtual);
    probe.Ept12Pml4Physical = (ULONGLONG)physical.QuadPart;
    physical = MmGetPhysicalAddress(probe.Ept12PdptVirtual);
    probe.Ept12PdptPhysical = (ULONGLONG)physical.QuadPart;
    /*
     * Map enough physical memory for L2 to run: its code page, and every page
     * of the page-table hierarchy the processor walks to reach it.  Those live
     * wherever Windows put them, so the map covers the machine's whole range
     * rather than trying to enumerate them.
     */
    if (!KswordARKHvmNestedProbeBuildEpt12(&probe, 512UL)) {
        /* No EPT12: the probe still runs, on the hierarchy we already own. */
        probe.Ept12Pointer = 0ULL;
    }
    /*
     * The negative case: L1 asks for accessed/dirty and must be refused.
     *
     * Set here rather than inside the builder so the positive path keeps
     * exactly the pointer it had, and the only difference between the two runs
     * is this one bit.
     */
    probe.SelfVirtualize = selfVirtualize;
    if (requestAccessedDirty && probe.Ept12Pointer != 0ULL) {
        probe.Ept12Pointer |= (1ULL << 6);
    }
    Response->ept12Armed = (probe.Ept12Pointer != 0ULL) ? 1UL : 0UL;
    /*
     * Both regions start with the revision identifier.
     *
     * Our own dispatch does not read them - it cannot, because they are named
     * by physical addresses - but writing them keeps the probe honest: if the
     * check is ever added, this probe must still pass.
     */
    *(volatile ULONG*)probe.VmxonVirtual = revision;
    *(volatile ULONG*)probe.Vmcs12Virtual = revision;
    if (probe.Vmcs12bVirtual != NULL) {
        RtlZeroMemory(probe.Vmcs12bVirtual, PAGE_SIZE);
        *(volatile ULONG*)probe.Vmcs12bVirtual = revision;
        physical = MmGetPhysicalAddress(probe.Vmcs12bVirtual);
        probe.Vmcs12bPhysical = (ULONGLONG)physical.QuadPart;
    }
    /* Every depth region is its own VMCS, so each page gets the revision. */
    if (probe.DepthBlockVirtual != NULL) {
        ULONG depthIndex = 0UL;

        RtlZeroMemory(
            probe.DepthBlockVirtual,
            (SIZE_T)KSW_PROBE_DEPTH_REGIONS * PAGE_SIZE);
        for (depthIndex = 0UL;
             depthIndex < KSW_PROBE_DEPTH_REGIONS;
             ++depthIndex) {
            *(volatile ULONG*)((PUCHAR)probe.DepthBlockVirtual +
                ((SIZE_T)depthIndex * PAGE_SIZE)) = revision;
        }
        physical = MmGetPhysicalAddress(probe.DepthBlockVirtual);
        probe.DepthBlockPhysical = (ULONGLONG)physical.QuadPart;
    }
    /*
     * Build L1's VM-entry MSR-load list: one entry, written back unchanged.
     *
     * Layout is architectural - index, four reserved bytes that must be zero,
     * then the value.  The reserved word is the easy one to get wrong: a
     * non-zero there fails VM entry with an MSR-loading failure that names the
     * entry number and nothing else.
     */
    if (probe.MsrAreaVirtual != NULL) {
        volatile ULONG* entry = NULL;

        RtlZeroMemory(probe.MsrAreaVirtual, PAGE_SIZE);
        entry = (volatile ULONG*)((PUCHAR)probe.MsrAreaVirtual +
            KSW_PROBE_MSR_LOAD_OFFSET);
        entry[0] = KSW_PROBE_MSR_AREA_INDEX;
        entry[1] = 0UL;
        *(volatile ULONGLONG*)&entry[2] =
            __readmsr(KSW_PROBE_MSR_AREA_INDEX);
        /* The store area only needs its index; nothing walks it at count 0. */
        *(volatile ULONG*)((PUCHAR)probe.MsrAreaVirtual +
            KSW_PROBE_MSR_STORE_OFFSET) = KSW_PROBE_MSR_AREA_INDEX;
        physical = MmGetPhysicalAddress(probe.MsrAreaVirtual);
        probe.MsrAreaPhysical = (ULONGLONG)physical.QuadPart;
    }
    physical = MmGetPhysicalAddress(probe.VmxonVirtual);
    probe.VmxonPhysical = (ULONGLONG)physical.QuadPart;
    physical = MmGetPhysicalAddress(probe.Vmcs12Virtual);
    probe.Vmcs12Physical = (ULONGLONG)physical.QuadPart;
    probe.Response = Response;
    /*
     * Pin to one processor for the whole sequence.
     *
     * VMX operation is per-processor state.  Migrating between the VMXON and
     * the VMXOFF would leave one processor in emulated VMX operation with
     * nothing to take it out, and execute the rest against a nested record
     * that never saw the VMXON.
     */
    affinity = (KAFFINITY)1 << processorNumber.Number;
    target.Group = processorNumber.Group;
    target.Mask = affinity;
    KeSetSystemGroupAffinityThread(&target, &previous);
    KswordARKHvmNestedProbeExecute(&probe);
    KeRevertToUserGroupAffinityThread(&previous);
    MmFreeContiguousMemory(probe.VmxonVirtual);
    MmFreeContiguousMemory(probe.Vmcs12Virtual);
    if (probe.Vmcs12bVirtual != NULL) {
        MmFreeContiguousMemory(probe.Vmcs12bVirtual);
    }
    if (probe.DepthBlockVirtual != NULL) {
        MmFreeContiguousMemory(probe.DepthBlockVirtual);
    }
    if (probe.MsrAreaVirtual != NULL) {
        MmFreeContiguousMemory(probe.MsrAreaVirtual);
    }
    MmFreeContiguousMemory(probe.L2CodeVirtual);
    MmFreeContiguousMemory(probe.Ept12Pml4Virtual);
    MmFreeContiguousMemory(probe.Ept12PdptVirtual);
    MmFreeContiguousMemory(probe.L1MsrBitmapVirtual);
    ExFreePool(probe.L1StackVirtual);
    /* Return a completed probe whatever the individual steps reported. */
    return STATUS_SUCCESS;
}

/* Carry one worker's target and result across thread creation. */
typedef struct _KSW_HVM_PROBE_WORKER
{
    PROCESSOR_NUMBER Target;
    KSWORD_ARK_HVM_NESTED_PROBE_ROW* Row;
    PVOID Thread;
    /* Carry the negative-case selector to the worker that will use it. */
    BOOLEAN RequestAccessedDirty;
    /* Carry the self-virtualize selector the same way. */
    BOOLEAN SelfVirtualize;
} KSW_HVM_PROBE_WORKER;

/* Run one worker's probe and exit.  One thread per processor. */
static VOID
KswordARKHvmNestedProbeWorker(
    _In_ PVOID StartContext
    )
{
    KSW_HVM_PROBE_WORKER* worker = (KSW_HVM_PROBE_WORKER*)StartContext;

    (void)KswordARKHvmNestedProbeRunOne(
        worker->Row,
        &worker->Target,
        worker->RequestAccessedDirty,
        worker->SelfVirtualize);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS
KswordARKHvmNestedProbeRun(
    _In_ const KSWORD_ARK_HVM_NESTED_PROBE_REQUEST* Request,
    _Out_ KSWORD_ARK_HVM_NESTED_PROBE_RESPONSE* Response
    )
{
    KSW_HVM_PROBE_WORKER* workers = NULL;
    PROCESSOR_NUMBER processorNumber = { 0 };
    ULONG processorCount = 0UL;
    ULONG index = 0UL;
    ULONG started = 0UL;
    NTSTATUS status = STATUS_SUCCESS;

    RtlZeroMemory(Response, sizeof(*Response));
    Response->version = KSWORD_ARK_HVM_NESTED_PROBE_PROTOCOL_VERSION;
    Response->size = sizeof(*Response);
    /* Reject a request that does not match the compiled contract. */
    if (Request->version !=
            KSWORD_ARK_HVM_NESTED_PROBE_PROTOCOL_VERSION ||
        Request->size != sizeof(*Request)) {
        Response->status =
            KSWORD_ARK_HVM_NESTED_PROBE_STATUS_INVALID_REQUEST;
        /* Return the exact contract failure. */
        return STATUS_INVALID_PARAMETER;
    }
    /* Require the explicit confirmation this control class shares. */
    if ((Request->flags &
            KSWORD_ARK_HVM_CONTROL_FLAG_UI_CONFIRMED) == 0UL ||
        Request->confirmationToken !=
            KSWORD_ARK_HVM_CONTROL_CONFIRMATION_TOKEN) {
        Response->status =
            KSWORD_ARK_HVM_NESTED_PROBE_STATUS_CONFIRMATION_REQUIRED;
        /* Return the exact confirmation failure. */
        return STATUS_SUCCESS;
    }
    /* One processor unless the caller asked for all of them. */
    if ((Request->flags &
            KSWORD_ARK_HVM_NESTED_PROBE_FLAG_ALL_PROCESSORS) == 0UL) {
        KeGetCurrentProcessorNumberEx(&processorNumber);
        Response->returnedRows = 1UL;
        status = KswordARKHvmNestedProbeRunOne(
            &Response->rows[0],
            &processorNumber,
            ((Request->flags &
                KSWORD_ARK_HVM_NESTED_PROBE_FLAG_REQUEST_AD) != 0UL)
                ? TRUE
                : FALSE,
            ((Request->flags &
                KSWORD_ARK_HVM_NESTED_PROBE_FLAG_SELF_VIRTUALIZE) != 0UL)
                ? TRUE
                : FALSE);
        Response->status = Response->rows[0].status;
        /* Return the single-processor result. */
        return status;
    }
    processorCount =
        (ULONG)KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    /* Bound the run to the rows the response can carry. */
    if (processorCount > KSWORD_ARK_HVM_NESTED_PROBE_MAX_ROWS) {
        processorCount = KSWORD_ARK_HVM_NESTED_PROBE_MAX_ROWS;
    }
    workers = (KSW_HVM_PROBE_WORKER*)KswordARKAllocateNonPagedPool(
        (SIZE_T)processorCount * sizeof(KSW_HVM_PROBE_WORKER),
        'wnHK');
    if (workers == NULL) {
        Response->status =
            KSWORD_ARK_HVM_NESTED_PROBE_STATUS_NO_RESOURCES;
        /* Return the exact allocation failure. */
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(
        workers,
        (SIZE_T)processorCount * sizeof(KSW_HVM_PROBE_WORKER));
    /*
     * One thread per processor, all started before any is waited on.
     *
     * Starting and joining one at a time would run them in sequence, which is
     * the thing this mode exists to not do: the question is whether several
     * processors can hold L2 *at the same time*, and a sequential run cannot
     * distinguish that from them taking turns.
     */
    for (index = 0UL; index < processorCount; ++index) {
        HANDLE threadHandle = NULL;
        OBJECT_ATTRIBUTES attributes;

        if (!NT_SUCCESS(KeGetProcessorNumberFromIndex(
                index,
                &workers[index].Target))) {
            continue;
        }
        workers[index].Row = &Response->rows[index];
        /* Every processor runs the same case, positive or negative. */
        workers[index].RequestAccessedDirty =
            ((Request->flags &
                KSWORD_ARK_HVM_NESTED_PROBE_FLAG_REQUEST_AD) != 0UL)
                ? TRUE
                : FALSE;
        workers[index].SelfVirtualize =
            ((Request->flags &
                KSWORD_ARK_HVM_NESTED_PROBE_FLAG_SELF_VIRTUALIZE) != 0UL)
                ? TRUE
                : FALSE;
        Response->rows[index].processorIndex = index;
        Response->rows[index].status =
            KSWORD_ARK_HVM_NESTED_PROBE_STATUS_NOT_ARMED;
        InitializeObjectAttributes(
            &attributes,
            NULL,
            OBJ_KERNEL_HANDLE,
            NULL,
            NULL);
        if (!NT_SUCCESS(PsCreateSystemThread(
                &threadHandle,
                THREAD_ALL_ACCESS,
                &attributes,
                NULL,
                NULL,
                KswordARKHvmNestedProbeWorker,
                &workers[index]))) {
            continue;
        }
        if (NT_SUCCESS(ObReferenceObjectByHandle(
                threadHandle,
                THREAD_ALL_ACCESS,
                *PsThreadType,
                KernelMode,
                &workers[index].Thread,
                NULL))) {
            started += 1UL;
        }
        ZwClose(threadHandle);
    }
    /* Join every worker that actually started before reading its row. */
    for (index = 0UL; index < processorCount; ++index) {
        if (workers[index].Thread == NULL) {
            continue;
        }
        (void)KeWaitForSingleObject(
            workers[index].Thread,
            Executive,
            KernelMode,
            FALSE,
            NULL);
        ObDereferenceObject(workers[index].Thread);
        workers[index].Thread = NULL;
    }
    ExFreePool(workers);
    Response->returnedRows = processorCount;
    /*
     * The overall status is the worst row, not the first.
     *
     * A run where one processor worked and another did not is a failure of the
     * thing this mode tests, and reporting the first row would hide exactly
     * that case.
     */
    Response->status = KSWORD_ARK_HVM_NESTED_PROBE_STATUS_OK;
    for (index = 0UL; index < processorCount; ++index) {
        if (Response->rows[index].status !=
                KSWORD_ARK_HVM_NESTED_PROBE_STATUS_OK) {
            Response->status = Response->rows[index].status;
            break;
        }
    }
    /* Report no run at all rather than an empty success. */
    if (started == 0UL) {
        Response->status =
            KSWORD_ARK_HVM_NESTED_PROBE_STATUS_NO_RESOURCES;
    }
    /* Return a completed run whatever the individual rows reported. */
    return STATUS_SUCCESS;
}

#else

NTSTATUS
KswordARKHvmNestedProbeRun(
    _In_ const KSWORD_ARK_HVM_NESTED_PROBE_REQUEST* Request,
    _Out_ KSWORD_ARK_HVM_NESTED_PROBE_RESPONSE* Response
    )
{
    UNREFERENCED_PARAMETER(Request);
    /* Zero the output so no caller reads uninitialized probe state. */
    if (Response != NULL) {
        RtlZeroMemory(Response, sizeof(*Response));
        Response->status =
            KSWORD_ARK_HVM_NESTED_PROBE_STATUS_NOT_ARMED;
    }
    /* Return the explicit unsupported-architecture boundary. */
    return STATUS_NOT_SUPPORTED;
}

#endif
