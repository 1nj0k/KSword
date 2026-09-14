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

/*
 * Hold vmcs12 contents where they outlive one processor's VMX operation.
 *
 * Shared by every processor, because a VMCS is a region of memory and not a
 * per-processor object.  Intel requires VMCLEAR on the processor that holds a
 * VMCS current before VMPTRLD of the same region elsewhere, and that pair is
 * exactly how a hypervisor moves a vCPU between host processors - so a
 * per-processor store loses the whole configuration on the first migration
 * while every counter still reads healthy.
 *
 * Measured, not reasoned about: VMware's VMCS at 0x7e6f000 held thirty-six
 * fields in processor 1's store and six in processor 0's at the same instant.
 * The launch it eventually issued ran on processor 0 and carried none of the
 * controls it had configured, and the entry failed on guest state.
 *
 * A slot may be accessed concurrently only while two processors race to claim
 * a free one; the fields themselves cannot be, because the architecture lets
 * one VMCS be current on one processor at a time.  So the claim is interlocked
 * and the copies are plain.
 */
typedef struct _KSW_HVM_VMCS12_POOL
{
    /* Order slots by last use, so eviction drops the coldest. */
    volatile LONG64 Stamp[KSW_HVM_VMCS12_POOL_SLOTS];
    /* Hand out monotonic use stamps across every processor. */
    volatile LONG64 Clock;
    /* Retain how many slots this allocation actually holds. */
    ULONG Count;
    /* Keep the structure explicitly initialized across architectures. */
    ULONG Reserved0;
    /* Retain the slots themselves, keyed by vmcs12 physical address. */
    KSW_HVM_VMCS12_STATE Slots[KSW_HVM_VMCS12_POOL_SLOTS];
} KSW_HVM_VMCS12_POOL;

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
     * NMIs of our own that arrived while this processor was running L2.
     *
     * Worth its own number because the alternative to claiming them is not a
     * missing optimization but a bugcheck in the machine above: they used to
     * be reflected, and L1 forwards a physical NMI to its host.
     */
    ULONGLONG L2NmiClaimedCount;
    /*
     * Whether interrupts reach L2 at all, counted at the two places they can
     * stop.
     *
     * A guest hypervisor's BIOS sat at its boot menu with the countdown frozen
     * and the keyboard dead while the processor stayed pegged - the shape of a
     * guest polling a timer tick that never advances.  Both halves of that
     * depend on interrupts: the tick on IRQ 0, the keystroke on IRQ 1, and
     * neither reaches the guest without passing through here first.  From
     * outside, "no interrupt ever exited L2", "it exited and L1 was not told"
     * and "L1 injected and we dropped it" are one frozen screen.
     */
    ULONGLONG L2ExternalInterruptCount;
    ULONGLONG L2InjectionCount;
    /* Injection requests retired on L1's behalf after the entry delivered them. */
    ULONGLONG L2InjectionRetiredCount;
    /*
     * Where L2 actually is, and whether it can take an interrupt there.
     *
     * Three times now a mechanism has been reasoned about, found genuinely
     * broken, fixed, and the guest has gone on sitting at the same frozen boot
     * menu.  That is a method failing, not a mechanism hiding: every one of
     * those started from "what could stop a timer tick" instead of from what
     * the guest is doing.  A ring of exit addresses says whether it is looping
     * and where; RFLAGS says whether an interrupt could be delivered there at
     * all, which no amount of correct injection can substitute for.
     */
    ULONGLONG L2ExitRipRing[16];
    ULONG L2ExitReasonRing[16];
    ULONG L2ExitRingIndex;
    ULONGLONG L2LastRflags;
    ULONGLONG L2LastInterruptibility;
    /*
     * The control-register exit itself, and both sides' masks.
     *
     * L2 turned out to be looping on two instructions that both take a
     * control-register exit, with interrupts disabled - so the timer had
     * nothing to do with it.  Whose exit it is decides everything: our own
     * CR0/CR4 guest-host masks are unioned into vmcs02, so a bit only we care
     * about produces an exit L1 never armed, and the routing reflects it to L1
     * anyway.  L1 has no case for it, its guest reads the register back
     * unchanged, and it tries again forever.  This is the same defect the MSR
     * and port-I/O paths already route around by asking whose mask armed the
     * exit; control registers were left on the default path.
     *
     * Recorded rather than assumed: the qualification says which register and
     * which access, and the two masks say who armed it.
     */
    ULONGLONG L2LastCrQualification;
    ULONGLONG L2Vmcs12Cr0Mask;
    ULONGLONG L2Vmcs12Cr0Shadow;
    ULONGLONG L2Vmcs02Cr0Mask;
    ULONGLONG L2Vmcs12Cr4Mask;
    ULONGLONG L2Vmcs02Cr4Mask;
    ULONGLONG L2LastGuestCr0;
    /*
     * The primary controls on both sides, for the CR3 half of the same
     * question: CR3-load and CR3-store exiting live here, not in a mask, and
     * the merge takes the union - so a bit set in vmcs02 and clear in vmcs12
     * names an exit L1 never asked for.
     */
    ULONG L2Vmcs12Primary;
    ULONG L2Vmcs02Primary;
    /*
     * The injection question, from both ends.
     *
     * L2InjectionCount already says how many entries carried an event, read
     * back out of vmcs02 - six, against five hundred and thirty-six external
     * interrupts reflected to L1.  That number alone cannot say whose fault it
     * is: L1 may never have asked, or it may have asked and we may have failed
     * to carry the request across.  This counts the asking, at the only place
     * it happens - L1's VMWRITE to the VM-entry interruption-information
     * field - so the two numbers can be compared.
     *
     * The interruptibility pair is the other half.  A hypervisor will not
     * inject into a guest that cannot take an interrupt, so "L1 never asked"
     * has two very different explanations, and whether L2 ever runs with
     * interrupts enabled separates them.  Sampled on every L2 exit rather than
     * kept in the ring, because the ring holds sixteen entries and answers
     * "what is L2 doing now" - not "has it ever".
     */
    ULONGLONG L2InjectRequestCount;
    ULONGLONG L2ExitIfSetCount;
    ULONGLONG L2ExitIfClearCount;
    ULONG L2Vmcs12Exit;
    ULONG L2Vmcs02Exit;
    /*
     * Every L2 exit, counted by reason.
     *
     * The sixteen-entry ring answers "where is L2 right now" and is dominated
     * by whatever repeats fastest, so reading a diagnosis out of it is reading
     * a sampling bias.  Three of this session's wrong turns started that way.
     * A running histogram is the reading that separates "the guest is stuck in
     * this loop" from "the guest is fine and this loop is merely the most
     * frequent thing in it", and nothing else here can.
     *
     * Sixty-four buckets covers every basic exit reason the architecture
     * defines; anything larger is folded into the last one rather than
     * dropped, so the totals still add up.
     */
    ULONGLONG L2ExitReasonCounts[64];
    ULONG L2Vmcs12Pin;
    /*
     * Which ports, and which control registers.
     *
     * The reason histogram says port I/O and control-register access are
     * ninety percent of what L2 does, and neither of those names a device or a
     * register.  Ninety-five thousand of the port accesses fall in the "some
     * other port" bucket, which is the largest single unexplained reading on
     * this line - a bucket that big is not a summary, it is a place things go
     * to stop being measured.
     *
     * A ring rather than a histogram for the ports: sixty-four thousand
     * counters is not worth it to identify what is plainly a handful of ports
     * repeating, and sixteen consecutive samples name them.
     */
    ULONG L2PortRing[16];
    ULONG L2PortRingIndex;
    /*
     * The unnamed ports again, counted rather than sampled.
     *
     * The ring named 0xCF8 but a ring cannot say whether that is all of the
     * ninety-three thousand or merely the last sixteen of them - and the last
     * time a ring was read as a distribution it produced three wrong
     * diagnoses in a row.  Distinct unnamed ports are a handful, so a
     * first-come table of thirty-two is exact for this, and the miss counter
     * says so rather than leaving it assumed.
     */
    ULONG L2PortKeys[32];
    ULONGLONG L2PortKeyCounts[32];
    ULONGLONG L2PortKeyMissCount;
    /*
     * What the CR0 loop is actually writing, against what it gets back.
     *
     * The steady state is one instruction: 732,171 MOV-to-CR0 exits a minute
     * across both processors, with zero port I/O, zero CPUID and zero EPT
     * violations beside them.  A guest that writes a control register and does
     * nothing else is a guest whose write is not taking - and the only way to
     * say that is to record the value it asked for next to the value it can
     * read afterwards.
     *
     * Four slots keyed by address: the loop alternates between two addresses,
     * so four holds both halves and shows whether either changes over time.
     */
    ULONGLONG L2CrWriteRip[4];
    ULONGLONG L2CrWriteValue[4];
    ULONGLONG L2CrWriteGuestCr0[4];
    ULONGLONG L2CrWriteShadow[4];
    ULONGLONG L2CrWriteCount[4];
    /*
     * Distinct L2 exit addresses, with how often each one exits.
     *
     * The sixteen-slot RIP ring holds the tail, and the tail showed the same
     * two addresses across three separate boots - which says the hang is
     * deterministic but not how wide the loop is.  A small keyed table says
     * both: if two entries carry nearly every exit the guest is going nowhere,
     * and if the counts are spread over thirty-two addresses it is running and
     * the ring was merely showing the busiest instruction.
     *
     * Thirty-two entries, linear probe, first-come and never evicted - a
     * replacement policy would let the loop push out the evidence of anything
     * rarer, which is the reading that says whether there *is* anything rarer.
     */
    ULONGLONG L2RipKeys[32];
    ULONGLONG L2RipCounts[32];
    ULONGLONG L2RipMissCount;
    /* CR number (0,3,4,8 land at 0..3, anything else at 4) by access type. */
    ULONGLONG L2CrCounts[5][4];
    /*
     * Whether the VMCS region is actually working as the backing store.
     *
     * Storing into the region and reading it back are two steps that both
     * fail silently: a window that will not map, a region that never carried
     * our header, a header that did but with a count we refuse.  Without these
     * "L1 did not move the VMCS", "we never wrote the page" and "we wrote it
     * and the page did not travel" are one indistinguishable outcome - the
     * vmcs12 simply comes up empty, exactly as it did before the region
     * existed.  LastLoadHeader is the raw eight bytes that decided it.
     */
    ULONG RegionStoreOkCount;
    ULONG RegionStoreFailCount;
    ULONG RegionStoreEntries;
    ULONG RegionLoadOkCount;
    ULONG RegionLoadMissCount;
    /*
     * Fields the region held that the vmcs12 would not take.
     *
     * Separate from the miss count on purpose: a miss means the region was not
     * ours, this means it was ours and the restore still lost data.  The first
     * version of this path produced zero misses and lost every field.
     */
    ULONG RegionLoadRefusedFields;
    ULONGLONG RegionLastStorePhysical;
    /*
     * What the region already holds, so an unchanged spill can be skipped.
     *
     * Both halves are needed.  The serial says whether any field moved; the
     * launch flag lives in the region header and moves on its own, and a stale
     * one would make a VMCS that was copied to a new page and relaunched come
     * back reading "already launched".
     */
    ULONG RegionStoredSerial;
    BOOLEAN RegionStoredLaunched;
    UCHAR RegionReserved[3];
    ULONGLONG RegionStoreSkippedCount;
    /*
     * Which devices L2 actually talked to, counted by port range.
     *
     * A guest hypervisor's BIOS reaches every device through port I/O, so this
     * is the only place the conversation is visible to us at all.  The reason
     * it is needed: the BIOS completes its whole power-on test and then says
     * it found no operating system, on a CD whose boot catalog is verified
     * good - and "it never asked the drive anything" and "it asked and did not
     * like the answer" are the same picture from outside, while pointing at
     * completely different defects.  One counts as evidence, the other as a
     * device-detection failure that happened minutes earlier.
     */
    ULONGLONG L2PortCounts[8];
    ULONGLONG RegionLastLoadPhysical;
    ULONGLONG RegionLastLoadHeader;
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
     * The rest of what vmcs02 ran with, read back from the loaded VMCS.
     *
     * The controls above were enough while the question was "did we propagate
     * the field".  It stopped being enough the moment a guest hypervisor got an
     * entry to succeed and its guest died on the first instruction: from
     * outside, "we built the wrong vmcs02" and "L1 asked for something that
     * cannot run" are the same picture.  These are the fields that separate
     * them, and they are read back rather than copied for the same reason the
     * ones above are.
     */
    ULONGLONG LastEntryVmcs12Physical;
    ULONG LastEntryPinControls;
    ULONG LastEntryExitControls;
    ULONG LastEntryEntryControls;
    ULONGLONG LastEntryEptPointer;
    ULONGLONG LastEntryGuestCr0;
    ULONGLONG LastEntryGuestCr4;
    ULONGLONG LastEntryGuestRip;
    ULONG LastEntryGuestCsAr;
    ULONG LastEntryGuestActivity;
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
     * One allocation shared by every processor, made at residency prepare:
     * a slot is 16 KiB, the per-processor contexts are a static array, and the
     * contents have to follow the VMCS rather than the processor.  NULL is
     * legal and falls back to modelling one vmcs12 per processor.
     */
    KSW_HVM_VMCS12_POOL* Vmcs12Pool;
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
