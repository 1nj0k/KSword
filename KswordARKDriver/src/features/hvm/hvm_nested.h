/*++

Module Name:

    hvm_nested.h

Abstract:

    Defines bounded nested-VMX state and VMX-instruction dispatch.

Environment:

    Kernel-mode Driver Framework.

--*/

#pragma once

#include "hvm_internal.h"
#include "hvm_nested_ept.h"
#include "hvm_nested_vmcs.h"

/*
 * How many vmcs12 one processor can hold besides the loaded one.
 *
 * A slot is the full field array, about 16 KiB, so this is roughly 128 KiB per
 * processor that actually runs an L2.  Eight covers a hypervisor with a
 * handful of vCPUs plus its own housekeeping VMCSs; beyond that the eviction
 * counter says so rather than the behaviour quietly degrading back to the
 * single-vmcs12 failure this replaces.
 */
#define KSW_HVM_VMCS12_POOL_SLOTS 8UL

/* Forward declaration; the definition lives in hvm_phys_window.h. */
struct _KSW_HVM_PHYS_WINDOW;

/* Preserve one processor's bounded L1 nested-VMX state. */
typedef struct _KSW_HVM_NESTED_VCPU
{
    /*
     * This processor's physical window, cached from the resident context.
     *
     * Every VMX-instruction operand that lives in memory is read through it,
     * because the operand address belongs to the guest's address space and not
     * to ours.  Kept here rather than reached through the resident context so
     * the dispatch functions, which only ever receive this structure, do not
     * each need a second back-pointer.  NULL is legal and means every memory
     * operand is refused; it is not fatal to residency.
     */
    struct _KSW_HVM_PHYS_WINDOW* PhysWindow;
    /*
     * INVVPID instructions served for L1, per processor.
     *
     * Worth counting on its own because serving it means doing **nothing** —
     * L2 runs under VPID 0000H, which every VM entry and exit already flushes.
     * Without a counter, "we handled hundreds of them correctly" and "the
     * dispatch never saw one" produce identical evidence.
     */
    ULONGLONG InvvpidServedCount;
    /*
     * Which refusal stopped the last L2 entry, one to seven.
     *
     * Seven different conditions return the same architectural error, because
     * the architecture has one number for "invalid control field" and no way to
     * say which.  L1 gets that number and reports it; from outside, all seven
     * look identical, and the only thing anyone actually needs to know is which
     * one fired.  Zero means no entry has been refused on this processor.
     */
    ULONG L2LastRefusalSite;
    /* Publish whether nested instruction dispatch is enabled. */
    BOOLEAN Enabled;
    /* Publish whether L1 executed a valid VMXON transition. */
    BOOLEAN Vmxon;
    /* Publish whether one vmcs12 pointer is current. */
    BOOLEAN VmcsCurrent;
    /* Publish whether L1 attempted an L2 launch. */
    BOOLEAN L2LaunchAttempted;
    /* Preserve the protocol-visible nested state. */
    ULONG State;
    /* Preserve the last Intel VM-instruction error. */
    ULONG LastInstructionError;
    /*
     * Publish whether this processor is executing L2 right now.
     *
     * Load-bearing at every exit: it selects which VMCS is loaded and
     * therefore whose state the exit describes.  Reading an exit as L1's when
     * it was L2's routes it to the wrong hypervisor with no error anywhere.
     */
    BOOLEAN InL2;
    /* Keep the structure explicitly initialized across architectures. */
    UCHAR Reserved1[3];
    /* Preserve a monotonic dispatched instruction count. */
    ULONGLONG InstructionCount;
    /*
     * The no-progress fuse.
     *
     * An L2 that exits, gets resolved, resumes and faults identically forever
     * never reaches L1 and never reaches a bugcheck: the processor is busy, so
     * nothing times out, and the machine simply stops answering with no dump
     * and no host-side event.  That is measured, not hypothetical - it is what
     * self-virtualization does today.
     *
     * Progress is keyed on RIP, exit reason and RCX together.  RIP alone is
     * wrong: a REP string instruction with I/O exiting legitimately exits at
     * the same RIP once per iteration, and RCX is what tells that apart from
     * an access that is genuinely not advancing.
     *
     * This is not only instrumentation.  Hosting an L1 we do not control means
     * a misbehaving one must not be able to wedge the machine, and a fuse is
     * the only thing standing between "L1 has a bug" and "the box is gone".
     */
    ULONGLONG L2ProgressRip;
    ULONGLONG L2ProgressRcx;
    ULONG L2ProgressReason;
    ULONG L2NoProgressCount;
    /* Latch the trip, so the next entry is refused rather than re-looping. */
    BOOLEAN L2FuseTripped;
    UCHAR Reserved2[3];
    /* Preserve what the fuse saw, which is the whole point of tripping. */
    ULONGLONG L2FuseRip;
    ULONG L2FuseReason;
    ULONG L2FuseCount;
    /* Preserve the VMCS that runs L1, so reflection can return to it. */
    ULONGLONG Vmcs01Physical;
    /* Preserve where L1 would resume had its entry instruction merely failed. */
    ULONGLONG L1ResumeRip;
    /* Preserve L1's stack pointer at the moment it attempted entry. */
    ULONGLONG L1ResumeRsp;
    /* Preserve L1's flags at the moment it attempted entry. */
    ULONGLONG L1ResumeRflags;
    /* Count completed L2 entries. */
    ULONGLONG L2EntryCount;
    /* Count L2 exits delivered to L1 rather than handled here. */
    ULONGLONG L2ExitReflectedCount;
    /*
     * Count every L2 exit, whoever ended up owning it.
     *
     * The reflected count alone cannot say whether an exit was consumed here
     * instead of reaching L1 - and that distinction is the whole of nested
     * correctness.  An exit we answer ourselves is us impersonating L1 to its
     * own guest: L1's guest asks something, we reply, and L1 never learns it
     * was asked.  The difference between these two numbers is exactly how
     * often that happened.
     */
    ULONGLONG L2ExitTotalCount;
    /*
     * Preserve what vmcs02 actually carried into the last VM entry.
     *
     * Read back from the loaded vmcs02 immediately before VMLAUNCH, not
     * computed - the point is to see what the processor will act on rather
     * than what the merge intended.  A control bit that survives the union
     * while its companion address never gets written is invisible to every
     * other readout: the entry succeeds, the guest runs, and the processor
     * quietly consults whatever page the stale field names.
     */
    ULONG LastEntryPrimaryControls;
    /* Preserve the secondary controls from the same read-back. */
    ULONG LastEntrySecondaryControls;
    /* Preserve the MSR-bitmap address vmcs02 actually carried. */
    ULONGLONG LastEntryMsrBitmap;
    /* Preserve the two I/O-bitmap addresses vmcs02 actually carried. */
    ULONGLONG LastEntryIoBitmapA;
    ULONGLONG LastEntryIoBitmapB;
    /*
     * The TSC offset and MSR areas vmcs02 actually carried.
     *
     * Read back from the loaded VMCS like the three above, and for the same
     * reason: a field we believe we propagated and a field the processor will
     * act on only differ when the write did not happen, which is precisely the
     * failure that leaves no other trace.
     */
    ULONGLONG LastEntryTscOffset;
    ULONGLONG LastEntryMsrLoadAddress;
    ULONGLONG LastEntryMsrStoreAddress;
    ULONG LastEntryMsrLoadCount;
    ULONG LastEntryMsrStoreCount;
    /*
     * Preserve what L1 itself asked for, as of the last merge.
     *
     * The exit path cannot recover these from vmcs02: its controls are the
     * union of both sides, so "USE_MSR_BITMAPS is set" there says nothing
     * about whether L1 set it.  Routing needs L1's own answer, and this is the
     * only place it survives.
     */
    BOOLEAN L2MsrFilterFromL1;
    BOOLEAN L2IoFilterFromL1;
    BOOLEAN L2UncondIoFromL1;
    /* Publish whether the last merge read every page it needed. */
    BOOLEAN L2BitmapMergeComplete;
    /*
     * Publish that vmcs02 carries L1's own bitmap pages, not copies.
     *
     * The exit path has to know: with a shared page there is no local copy to
     * consult, so routing reads the one byte it needs out of L1's page through
     * the window rather than out of a snapshot.
     */
    BOOLEAN L2MsrBitmapShared;
    BOOLEAN L2IoBitmapsShared;
    /* Retain where L1's pages live, for those per-exit reads. */
    ULONGLONG L2MsrBitmapL1Gpa;
    ULONGLONG L2IoBitmapAL1Gpa;
    ULONGLONG L2IoBitmapBL1Gpa;
    /* Count MSR exits from L2 delivered to L1 rather than serviced here. */
    ULONGLONG L2MsrExitsReflected;
    /* Count MSR exits from L2 serviced here because only we armed them. */
    ULONGLONG L2MsrExitsHandled;
    /* Count port exits from L2 delivered to L1. */
    ULONGLONG L2IoExitsReflected;
    /* Count port exits from L2 serviced here. */
    ULONGLONG L2IoExitsHandled;
    /*
     * Cycles spent merging bitmaps, and cycles spent entering L2 overall.
     *
     * Two numbers rather than one, because the merge's cost only means
     * something as a share.  "Three page copies per entry" is a shape, not a
     * measurement, and deciding whether to cache from a shape is guessing.
     *
     * Read with RDTSC, which under an outer hypervisor is whatever it chose to
     * expose - fine for a ratio taken within one entry, not for absolute time.
     * Both accumulate, so the caller divides by L2EntryCount for the average.
     */
    ULONGLONG L2MergeCycles;
    ULONGLONG L2EntryCycles;
    /* Preserve the L1 VMXON-region physical address. */
    ULONGLONG VmxonRegion;
    /* Preserve the current L1 vmcs12 physical address. */
    ULONGLONG CurrentVmcs;
    /* Preserve bounded vmcs12 identity and fields. */
    KSW_HVM_VMCS12_STATE Vmcs12;
    /*
     * Hold every vmcs12 that is not currently loaded.
     *
     * `Vmcs12` above is the one L1 has current; a hypervisor keeps several and
     * VMPTRLDs between them constantly, so the others have to live somewhere.
     * Without this, switching away and back returned zeroes - measured, and
     * enough on its own to stop any real hypervisor from running underneath.
     *
     * A spill area rather than a replacement, deliberately: every existing
     * reader of `Vmcs12` keeps working unchanged, and all of the new logic
     * sits in the one place that switches pointers.
     *
     * Allocated at residency prepare because a slot is 16 KiB and the
     * per-processor contexts are a static array - embedding these would put
     * megabytes in BSS for processors that never run an L2.
     */
    PVOID Vmcs12PoolBlock;
    KSW_HVM_VMCS12_STATE* Vmcs12Pool;
    ULONG Vmcs12PoolCount;
    /* Order slots by last use, so eviction drops the coldest. */
    ULONGLONG Vmcs12PoolStamp[KSW_HVM_VMCS12_POOL_SLOTS];
    ULONGLONG Vmcs12PoolClock;
    /*
     * Evictions on this processor alone.
     *
     * The runtime keeps a durable total as well, and that one answers "did an
     * L1 ever keep more VMCSs than we hold" after the pools are long gone.
     * This one answers a question that total cannot: the probe runs a worker
     * on every processor at once, so a delta taken from the shared counter
     * includes whatever the other processors did in the same window.  Each
     * asking its own record is the only way a per-processor row means what it
     * says.
     */
    ULONG Vmcs12EvictionCount;
    /* Preserve explicit partial vmcs02 merge state. */
    KSW_HVM_VMCS02_STATE Vmcs02;
    /* Preserve explicit partial shadow-EPT composition state. */
    KSW_HVM_SHADOW_EPT_STATE ShadowEpt;
} KSW_HVM_NESTED_VCPU;

/* Forward-declare the VM-exit register frame without creating include cycles. */
struct _KSW_HVM_GPR_FRAME;
/* Forward-declare the per-processor resident context for the same reason. */
struct _KSW_HVM_RESIDENT_VCPU;

EXTERN_C_START

/* Initialize one per-processor nested state record. */
VOID
KswordARKHvmNestedInitializeVcpu(
    _Out_ KSW_HVM_NESTED_VCPU* Nested,
    _In_ BOOLEAN Enabled,
    _In_ ULONGLONG L0EptPointer
    );

/* Validate nested dispatch while retaining explicit partial implementation. */
NTSTATUS
KswordARKHvmNestedValidate(
    _Inout_ KSW_HVM_RUNTIME* Runtime
    );

/*
 * Dispatch one VMX instruction exit and publish exact failure semantics.
 *
 * Takes the whole processor context rather than just the nested record because
 * VMLAUNCH and VMRESUME need the VMCS pages, the EPT hierarchy and the mapping
 * window - none of which the nested record owns.
 */
BOOLEAN
KswordARKHvmNestedHandleExit(
    _Inout_ struct _KSW_HVM_RESIDENT_VCPU* Vcpu,
    _Inout_ struct _KSW_HVM_GPR_FRAME* Frame,
    _In_ ULONG ExitReason,
    _In_ ULONG InstructionLength
    );

EXTERN_C_END
