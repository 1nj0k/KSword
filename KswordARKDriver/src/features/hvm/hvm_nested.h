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
     * Events this exit interrupted mid-delivery, and what became of them.
     *
     * An exit can happen while the processor is still delivering an event -
     * reading the IDT, pushing the frame - and then the event was not
     * delivered.  The processor reports that in the IDT-vectoring information
     * field and expects whoever handles the exit to deliver it again.
     *
     * Two destinations, and only one of them used to work.  An exit reflected
     * to L1 carries the field into vmcs12, so L1 re-injects.  An exit this
     * driver answers and resumes from has no L1 in the loop: we are the only
     * VMM that can re-deliver it, and we did not.
     *
     * Measured cost of that gap: the guest's master PIC sat at ISR = 0x03 with
     * IRR = 0x41 for the whole run.  Two interrupts - IRQ 0 and IRQ 1 - had
     * been taken off the controller by L1's acknowledge and then destroyed
     * here, so no handler ever ran and no EOI was ever sent.  An 8259 will not
     * assert INTR again while a same-or-lower priority interrupt is in
     * service, and IRQ 0 is the highest, so that wedged every interrupt in the
     * machine.  The guest's BIOS tick never moved again.  Nothing in this
     * driver or in L1 reported an error: L1 had asked, we had "delivered",
     * and both counters read healthy.
     *
     * Reflected is counted too, so the pair answers "did this even happen"
     * before it answers "did we handle it".
     */
    ULONGLONG L2IdtVectoringSeenCount;
    ULONGLONG L2IdtVectoringReinjectedCount;
    ULONGLONG L2IdtVectoringReflectedCount;
    /* The last one re-delivered, for when the counts alone are not enough. */
    ULONG L2IdtVectoringLastInfo;
    /*
     * Which exit the last re-delivery rode on, counted in exits.
     *
     * Re-injecting writes an event into vmcs02 that the next entry will
     * deliver, so if a guest dies right after an exit, "was an event put on
     * that entry" is the first question - and a count of re-injections cannot
     * answer it, only an ordinal can.  Compared against the exit total at the
     * moment of death: equal means this exit, one less means the one before.
     */
    ULONGLONG L2IdtVectoringLastExitOrdinal;
    /*
     * L2's CR2 as it was when this exit happened.
     *
     * CR2 is not a VMCS field.  VM exit does not save it and VM entry does not
     * load it, so between the two it is simply the processor's CR2 - shared
     * with every line of this driver that runs in between.  A page fault taken
     * in root mode, or the emulator arming its own #PF, overwrites it.
     *
     * That costs nothing until something re-delivers a #PF, because a #PF is
     * the one event whose payload lives in CR2 rather than in the VMCS.  Then
     * the guest's handler is handed an address that belongs to us, fixes the
     * wrong page, returns, and faults again - or takes the wrong branch
     * entirely and dies with no diagnosis anywhere.
     *
     * Saved at the top of the exit, before anything of ours has run.
     */
    ULONGLONG L2ExitCr2;
    /*
     * Which vmcs12 regions this processor has actually entered, and how often.
     *
     * One region is one virtual processor: L1 keeps a VMCS per vCPU, so the
     * number of distinct regions seen here is the number of L1 guest CPUs that
     * ever reached hardware virtualization under us.
     *
     * The question it exists to answer: L1 brings up a second processor, that
     * processor never reports in, and its guest waits for it forever.  From
     * outside there is no way to tell "L1 never launched it" from "we launched
     * it and it died" - both are silence.  One region means the first, two
     * regions with a stalled RIP in the second means the second, and the last
     * guest RIP per region says where.
     *
     * Four slots, first-come: a two-processor guest needs two and this table
     * is meant to answer how many, not to survive a machine that churns them.
     */
    ULONGLONG L2Vmcs12Regions[4];
    ULONGLONG L2Vmcs12RegionEntries[4];
    ULONGLONG L2Vmcs12RegionLastRip[4];
    ULONGLONG L2Vmcs12RegionMissCount;
    /*
     * Per region, the three numbers that say why one of L1's processors
     * stopped while the other kept running.
     *
     * The bootstrap processor's region has been frozen for the whole
     * observation while the application processor's climbs at five entries a
     * second, and from outside that has three different explanations that look
     * identical: it is waiting for an inter-processor interrupt that never
     * arrives, it is waiting for its own timer that never fires, or it halted
     * with interrupts disabled and is simply dead.
     *
     * Last exit reason and the flags at that exit separate them.  A halt
     * (reason 12) with RFLAGS.IF set is a processor waiting to be woken; the
     * same halt with IF clear is one that will never wake.  Injections say
     * whether anything was ever handed to it.
     */
    ULONG L2Vmcs12RegionLastExitReason[4];
    ULONGLONG L2Vmcs12RegionLastRflags[4];
    ULONGLONG L2Vmcs12RegionInjections[4];
    /*
     * A four-deep trail per region, and the mode each one last ran in.
     *
     * The exit ring this driver already had is per *physical* processor, so
     * with two L1 virtual processors migrating across two cores it interleaves
     * them and cannot say where either one stopped.  Per region it can.
     *
     * Needed because "the application processor stops" has two very different
     * shapes and the last address alone has shown both across runs: a
     * real-mode address means it never left its startup stub, a 64-bit kernel
     * address means it got all the way to long mode and stopped there.  CR0
     * and the CS access rights say which without having to guess from the
     * value of the address.
     */
    ULONGLONG L2Vmcs12RegionTrailRip[4][4];
    ULONG L2Vmcs12RegionTrailReason[4][4];
    ULONG L2Vmcs12RegionTrailIndex[4];
    ULONGLONG L2Vmcs12RegionLastCr0[4];
    ULONG L2Vmcs12RegionLastCsAr[4];
    /*
     * And the event each region's last entry carried.
     *
     * The per-processor copy of this cannot answer the question it is needed
     * for.  Both of L1's virtual processors run on the same physical one and
     * interleave, so "the last entry on this processor" may belong to either -
     * which is how a vector was once attributed to the wrong one, acted on,
     * and cost a change that made VMware's monitor panic.
     */
    ULONG L2Vmcs12RegionLastEntryIntrInfo[4];
    /*
     * Who wrote that event, and whether the guest could have taken it.
     *
     * Two different defects end in the same triple fault and nothing else
     * separates them.  If L1 asked for the injection, the entry is doing what
     * L1 requested and the question is why L1 thought its guest was ready.  If
     * *we* wrote it - the re-delivery of an event an exit interrupted - then it
     * is ours, and re-delivering into a guest that has moved on is a defect
     * with our name on it.
     *
     * The flag is set where the re-delivery writes the field and cleared where
     * a fresh merge overwrites it from vmcs12, so it always describes the
     * event the next entry will actually carry.
     *
     * RFLAGS and the interruptibility state ride along because event injection
     * through the VM-entry field **ignores RFLAGS.IF**: an interrupt handed to
     * a guest that had interrupts masked is delivered anyway, into whatever
     * the IDT holds at that moment.
     */
    BOOLEAN L2Vmcs12RegionEntryWasRedeliver[4];
    ULONGLONG L2Vmcs12RegionLastEntryRflags[4];
    ULONG L2Vmcs12RegionLastEntryIntbl[4];
    /*
     * And the interrupt-command register writes, which is how one of L1's
     * processors wakes another.
     *
     * A ring rather than a count: the value carries the destination, the
     * delivery mode and the vector, and what matters is the last few - an IPI
     * sent to wake the stalled processor looks different from the ones sent
     * during bring-up.  x2APIC puts the whole thing in one MSR write, so one
     * 64-bit value is the whole message.
     */
    ULONGLONG L2IcrRing[4];
    ULONG L2IcrRingIndex;
    ULONGLONG L2IcrWriteCount;
    /*
     * The last eight exceptions L2 took, with where they happened.
     *
     * L1 traps exceptions on purpose - without unrestricted guest it runs real
     * mode by trapping - so a count of them says nothing.  Which vector, at
     * which address, in which segment does: L1's application processor sits in
     * its real-mode startup stub taking these, having never reached 64-bit
     * code and having been handed zero injections, while the bootstrap
     * processor waits for it in the kernel.
     *
     * Interruption information as the processor reported it (vector in 7:0,
     * type in 10:8, error-code-valid at 11), the error code beside it, and
     * CS:RIP so the stub can be located.
     */
    ULONG L2ExceptionInfoRing[8];
    ULONG L2ExceptionErrorRing[8];
    ULONGLONG L2ExceptionRipRing[8];
    ULONG L2ExceptionCsRing[8];
    ULONG L2ExceptionRingIndex;
    /*
     * The last EPT violation, in full, and what was decided about it.
     *
     * The exit ring answers "which instruction" and the histogram answers "how
     * often".  Neither answers the question an unresolvable violation raises:
     * *which address*, *which access*, and *who was supposed to fix it*.
     * Measured need: L2 stopped with sixteen identical reason-48 exits at one
     * kernel RIP and the fuse tripped, while the shadow pool reported no
     * exhaustion and a hundred thousand successful fills - three readings that
     * together say only "something loops".
     *
     * Disposition is a small number rather than a flag because there are three
     * different outcomes and two of them look alike from outside:
     *   1 composed and resumed   2 refused, reflected to L1
     *   3 no shadow armed, our own hierarchy   0 nothing recorded yet
     */
    ULONGLONG L2LastEptGuestPhysical;
    ULONGLONG L2LastEptQualification;
    ULONG L2LastEptDisposition;
    /*
     * The last eight MSRs L2 touched, and the last value it wrote.
     *
     * A ring, deliberately, and this is the case a ring is actually for: L2 sat
     * alternating RDMSR and WRMSR between two kernel addresses with interrupts
     * disabled, and the question is "which MSR is it spinning on *now*", not
     * "which MSR has it used most since power-on".  The fuse cannot answer it
     * either - alternating reasons reset its consecutive count every exit, so
     * a two-instruction loop is invisible to it by construction.
     *
     * High bit marks a write, so one slot carries both which MSR and which
     * direction.
     */
    ULONG L2MsrRing[8];
    ULONG L2MsrRingIndex;
    ULONGLONG L2LastMsrWriteValue;
    ULONG L2LastMsrWriteIndex;
    /*
     * The same, for guest-physical addresses above where this guest's RAM
     * ends - which is to say for device registers.
     *
     * An MMIO access has to reach L1: only L1 has the device model.  If we
     * compose a leaf for it instead, the access lands on whatever page EPT12
     * happens to name and the device is never touched - the guest programs an
     * interrupt controller that does not exist, then waits forever for the
     * interrupt.  Measured symptom: the boot stops at the first device whose
     * initialization needs an interrupt, and the stopping point *moves with
     * the device* - EHCI at IRQ 17 with USB on, i8042 at IRQ 1 with USB off.
     *
     * Kept apart from the RAM case because the two have opposite correct
     * answers: for RAM, composing is the fix; for MMIO, composing is the bug.
     * Mixed together they are one counter that cannot say which happened.
     *
     * The threshold is a probe, not an architectural boundary - this guest has
     * 768 MiB and its devices sit at 0xFD5EF000 and 0xFEC00000.  It exists to
     * answer one question on one machine, and the counts say plainly if it is
     * catching the wrong thing.
     */
    ULONGLONG L2LastMmioGuestPhysical;
    ULONGLONG L2LastMmioQualification;
    ULONG L2LastMmioDisposition;
    ULONGLONG L2MmioComposedCount;
    ULONGLONG L2MmioReflectedCount;
    /*
     * The scene at the first triple fault, kept whole.
     *
     * A triple fault is the one exit that says nothing about itself: no
     * qualification, no vector, no address.  Everything that can be known
     * about it is the state the guest was in and how it got there, and both
     * are gone the moment the exit is reflected and L1 resets the processor.
     *
     * Measured need: L1 brings up its application processor, that processor
     * triple-faults, L1 resets it and tries again - three times in the log -
     * and the boot stops with the second CPU never started.  The bootstrap
     * processor runs the same kernel image without trouble, so what differs is
     * the state an AP starts from: it comes out of SIPI in real mode and
     * climbs through protected mode to long mode in a few hundred
     * instructions, touching CR0, CR4, EFER and its own page tables.
     *
     * The first one only.  A retry runs the same code from the same state, so
     * later faults add nothing, and keeping the first avoids the scene being
     * overwritten by a reset that has already destroyed the evidence.
     */
    ULONGLONG L2TripleFaultRip;
    ULONGLONG L2TripleFaultCr0;
    ULONGLONG L2TripleFaultCr3;
    ULONGLONG L2TripleFaultCr4;
    ULONGLONG L2TripleFaultEfer;
    ULONG L2TripleFaultCsAr;
    ULONG L2TripleFaultActivity;
    ULONGLONG L2TripleFaultCount;
    /* And the four exits before it, copied out of the ring at the moment. */
    ULONGLONG L2TripleFaultPrevRip[4];
    ULONG L2TripleFaultPrevReason[4];
    /* Where this exit sits in the run, and where the last re-delivery sat. */
    ULONGLONG L2TripleFaultExitOrdinal;
    ULONGLONG L2TripleFaultReinjectOrdinal;
    ULONG L2TripleFaultLastVectoringInfo;
    /*
     * What the entry before the fault carried, and what the fault itself says.
     *
     * The measured scene is a guest that halts, is woken, and triple-faults on
     * the instruction after the HLT - which is where an interrupt is delivered,
     * not where code runs.  So the question is entirely about that delivery:
     * which event was put on the entry (L2LastEntryIntrInfo), whether the
     * processor was still delivering something when it gave up
     * (IdtVectoring), and whether the stack it was pushing onto was usable
     * (Rsp).  None of the three survives the reflection that follows.
     */
    ULONG L2LastEntryIntrInfo;
    ULONG L2TripleFaultEntryIntrInfo;
    ULONG L2TripleFaultIdtVectoring;
    ULONGLONG L2TripleFaultRsp;
    ULONGLONG L2TripleFaultSsAr;
    /* Whose injection it was, and what the guest's state was when it landed. */
    ULONG L2TripleFaultEntryWasRedeliver;
    ULONGLONG L2TripleFaultEntryRflags;
    ULONG L2TripleFaultEntryIntbl;
    /*
     * The three tables interrupt delivery has to read, and whether vmcs02
     * carries what vmcs12 said.
     *
     * Delivering an interrupt in 64-bit mode reads the gate out of the IDT
     * through IDTR, loads the code segment through GDTR, and - if the gate
     * names an IST - reads the stack pointer out of the TSS through TR.  A
     * base or limit that does not match what L1 wrote makes every one of those
     * reads land somewhere else, and the fault that follows is handled by a
     * handler found the same wrong way: #DF, then shutdown.  It leaves no exit
     * behind, which is the same signature the shadow-mapping hypothesis had
     * and the reason this is what remains after that one was ruled out.
     *
     * The mask is what makes it a criterion rather than a pile of numbers: one
     * bit per field that differs from vmcs12, so "all zero" is a clean answer
     * and anything else names the field.
     */
    ULONGLONG L2TripleFaultIdtrBase;
    ULONGLONG L2TripleFaultGdtrBase;
    ULONGLONG L2TripleFaultTrBase;
    ULONG L2TripleFaultIdtrLimit;
    ULONG L2TripleFaultGdtrLimit;
    ULONG L2TripleFaultTrLimit;
    ULONG L2TripleFaultTrAr;
    ULONG L2TripleFaultDescMismatch;
    /*
     * What L1 ever wrote to the guest IDTR base, and how often.
     *
     * The cache has no "never written" state, so a field reading back as zero
     * is indistinguishable from one L1 never set - and the two point at
     * opposite halves of the code.  This field specifically, because the
     * triple fault happens while the processor reads a gate out of the IDT and
     * vmcs02 carried base zero with limit 0x0FFF: a correct limit and an
     * address of nothing.
     *
     * Linux puts its IDT at the entry area, 0xFFFFFE0000000000, whose low
     * thirty-two bits are all zero - so "truncated somewhere" and "never
     * written" produce the same zero, and only the count and the value L1
     * actually passed can separate them.
     */
    ULONGLONG L2IdtrBaseLastWritten;
    ULONG L2IdtrBaseWriteCount;
    /*
     * The two fields beside it, as the control for that count.
     *
     * A count of zero on its own does not say "L1 never writes descriptor
     * tables" - it could equally say the counter is looking at the wrong
     * thing.  The IDTR limit and the GDTR base are written by the same kind of
     * code at the same time, and both were correct in the scene, so if they
     * count and the base does not, the base really is the odd one out.
     */
    ULONG L2IdtrLimitWriteCount;
    ULONG L2GdtrBaseWriteCount;
    /*
     * And what our own two copy loops moved, which is the other way in.
     *
     * L2 loads its own IDT with LIDT, and nothing intercepts that: the
     * processor updates the base in whichever VMCS is current, with no exit
     * and no VMWRITE for the count above to see.  So the value has to survive
     * two copies that are ours - vmcs02 out to vmcs12 when the exit is handed
     * to L1, and vmcs12 back into vmcs02 when L1 resumes.  Recording both ends
     * says which copy lost it, or that it was never there to lose.
     *
     * The non-zero counts matter more than the last value: a single zero at
     * the end is what both a working round trip and a broken one look like
     * once the guest has already been reset by the triple fault.
     */
    ULONGLONG L2IdtrBaseSavedLast;
    ULONGLONG L2IdtrBaseLoadedLast;
    ULONG L2IdtrBaseSaveCount;
    ULONG L2IdtrBaseLoadCount;
    ULONG L2IdtrBaseSavedNonZeroCount;
    ULONG L2IdtrBaseLoadedNonZeroCount;
    /*
     * The same base per vmcs12 region, and the moment it goes backwards.
     *
     * The totals above mix two virtual processors on one physical core, so a
     * fourteen-percent non-zero rate means nothing on its own: it reads the
     * same whether one processor never had an IDT or both keep losing one.
     * Per region it separates, and the transition is the whole question - a
     * region that is zero from its first exit is carrying the guest's own
     * early state, while one that held 0xFFFFFE0000000000 and then reads zero
     * lost it somewhere between two of our entries, and the RIP and reason
     * recorded at that transition say where to look.
     */
    ULONGLONG L2RegionIdtrBase[4];
    ULONGLONG L2RegionIdtrLostRip[4];
    ULONG L2RegionIdtrLostReason[4];
    ULONG L2RegionIdtrLostCount[4];
    /*
     * And which of the two put the zero there, because the count above does
     * not say and the two are opposite findings.
     *
     * A transition happens either because the entry wrote a zero out of
     * vmcs12 over a base the processor already had - the cache lost it, and
     * that is ours - or because L2 itself loaded an IDT with a zero base
     * between two of our exits, which is the guest's own business and happens
     * legitimately all through early boot.  Counting them apart makes the
     * total decomposable: lost should be the sum of these two.
     */
    ULONGLONG L2RegionIdtrLoaded[4];
    ULONG L2RegionIdtrCacheLost[4];
    ULONG L2RegionIdtrGuestZeroed[4];
    /*
     * The scene at the last one of those our entry caused.
     *
     * Fifteen of them against a hundred and sixty thousand round trips, so a
     * counter alone leaves nothing to act on: it says the cache handed back a
     * zero for a field the previous exit still had, and every candidate
     * mechanism - a spill that could not map its page, a restore from a
     * region written before that save, a pooled copy evicted and reloaded
     * short - produces exactly that count and nothing else.  The backing
     * store's own health has never been published anywhere, so it is captured
     * here at the instant it matters rather than read as a total afterwards.
     */
    /*
     * How often L2 is entered in the shape the triple fault was found in.
     *
     * Sixty-four-bit mode with an IDTR base of zero and a limit of 0x0FFF is
     * a real state Linux passes through - its bring-up descriptor is declared
     * with exactly that size and an address filled in later - and it is
     * harmless there only because interrupts are off.  So the state alone is
     * not the defect and counting it is not enough: what matters is whether
     * an entry ever carries an event into it.  One counter for the state and
     * one for the state with an injection separates "the guest lives here all
     * the time" from "this happened once, and that once was fatal".
     */
    ULONGLONG L2Idt0In64Count;
    ULONGLONG L2Idt0In64InjectedCount;
    ULONGLONG L2Idt0In64Rip;
    ULONGLONG L2Idt0In64Vmcs;
    ULONG L2Idt0In64Entry;
    ULONG L2Idt0In64Rflags;
    /*
     * The same field read out of all three places it lives, at that entry.
     *
     * L2 halts in its idle loop at that address with the entry area's GDT and
     * TSS loaded, which is a fully running kernel: a base of zero there is not
     * a state Linux has, so the value is lost rather than absent.  The working
     * copy, the pooled copy and the region page are the three stages it passes
     * through, and reading all three at the moment the entry goes wrong says
     * which stage still had it - one reading instead of another round of
     * narrowing.
     */
    ULONGLONG L2Idt0In64FromCache;
    ULONGLONG L2Idt0In64FromPool;
    ULONGLONG L2Idt0In64FromRegion;
    ULONG L2Idt0In64RegionEntries;
    /*
     * The other end of it: a sixty-four-bit L2 that had a base when we
     * entered it and has none when it comes back.
     *
     * A whole boot went by with three triple faults and not one entry in the
     * faulting shape, which says the base is not arriving zero - it goes to
     * zero while L2 runs, and the only thing that can do that is L2's own
     * LIDT.  Keyed on what this processor loaded at the entry that is now
     * exiting, which is exact whichever virtual processor it was: the pairing
     * is entry-then-exit on one processor, with no migration in between.
     */
    ULONGLONG L2Idt64ZeroedRip;
    ULONGLONG L2Idt64ZeroedLoaded;
    ULONG L2Idt64ZeroedReason;
    ULONG L2Idt64ZeroedCount;
    ULONG L2Idt64ZeroedLimit;
    ULONG L2Idt64ZeroedCsAr;
    /*
     * And the transition upwards, which is the one nothing has counted.
     *
     * The processor writes the guest IDTR base into the VMCS on every exit -
     * the limit it does not, which is why L1 writes the limit twelve hundred
     * times and the base never.  So an L2 that executes LIDT shows up here as
     * an exit carrying a base we did not put there, and its absence for one
     * virtual processor would mean that processor's LIDT never reached vmcs02
     * at all.  That is the fork the downward counters cannot resolve: "never
     * had one" and "had one and lost it" both end at zero.
     */
    ULONGLONG L2IdtrGainedValue;
    ULONGLONG L2IdtrGainedRip;
    ULONGLONG L2IdtrGainedVmcs;
    ULONG L2IdtrGainedReason;
    ULONG L2IdtrGainedCount;
    /*
     * Per region, because the processor-wide count lumps L1's two virtual
     * processors together and the whole question is that they differ.
     *
     * One region carries 0xFFFFFE0000000000 from its first sixty-four-bit exit
     * to its last, the other carries zero the whole way, and both run kernel
     * code with the same entry-area GDT and TSS loaded.  If the second one's
     * count here is zero while the first one's is in the thousands, then its
     * LIDT never reached vmcs02 - which is a different defect from losing a
     * value that did.
     */
    ULONG L2RegionIdtrGained[4];
    /* What this processor last saved for the region the bad entry names. */
    ULONGLONG L2Idt0In64LastSaved;
    ULONGLONG L2IdtrLostEntryRip;
    ULONGLONG L2IdtrLostVmcs;
    ULONGLONG L2IdtrLostHeader;
    ULONG L2IdtrLostSerial;
    ULONG L2IdtrLostStoreFail;
    ULONG L2IdtrLostLoadMiss;
    ULONG L2IdtrLostRefused;
    ULONG L2IdtrLostEntries;
    ULONG L2IdtrLostEvictions;
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
    /*
     * What L1 actually asked to inject, not just how often.
     *
     * The count is three to eight across a whole run, which is nearly nothing
     * - but "nearly nothing" reads the same whether L1 tried the timer a few
     * times and gave up or never tried it at all.  The vector separates those:
     * 0x08 is IRQ0 through the PIC, 0x21 is IRQ1, and an interruption type of
     * 3 would mean these were exceptions and never device interrupts.
     *
     * Eight entries, oldest kept: there are only a handful of events in the
     * whole run, so keeping the first ones is keeping all of them.
     */
    ULONG L2InjectRequests[8];
    ULONG L2InjectRequestIndex;
    /*
     * The guest's state at the instant an injection is actually delivered.
     *
     * Proven by keypress: L1 raises IRQ1, asks to inject vector 0x09, and we
     * deliver it - nine asked, nine delivered - and the guest does not react.
     * So the interrupt arrives and is then wasted, and the only thing that
     * decides whether it lands on the right handler is the mode the guest is
     * in when it arrives.
     *
     * Vector 0x09 is the BIOS keyboard service through the real-mode vector
     * table, and exception 9 through a protected-mode IDT.  This guest spends
     * its life bouncing between the two - 12,200 writes to CR0 a second - so
     * which half of that bounce the delivery lands in is the whole question.
     * CR0 bit 0 answers it, and the CS access rights say the same thing a
     * second way.
     *
     * Captured at entry, where the processor is about to act on it, rather
     * than at L1's request, which is a different instant.
     */
    ULONG L2InjectStateVector[8];
    ULONG L2InjectStateCr0[8];
    ULONG L2InjectStateRflags[8];
    ULONG L2InjectStateCsAr[8];
    ULONG L2InjectStateIndex;
    /*
     * Every write L2 makes to the interrupt controller, value included.
     *
     * The injection record shows several IRQ1s, then one IRQ0, then nothing
     * ever again - which is the exact signature of an interrupt left in
     * service.  IRQ0 is the highest priority line, so a PIC whose in-service
     * bit for it is never cleared blocks every subsequent interrupt, not just
     * that one.
     *
     * Clearing it is what the BIOS timer handler's end-of-interrupt does:
     * `mov al, 0x20 ; out 0x20, al`.  The port counters already say the guest
     * wrote to 0x20 twelve times - but ICW1 during initialisation goes to the
     * same port, so a count cannot tell an EOI from a reset.  Only the value
     * can, and nothing was recording it.
     *
     * Sixteen entries, oldest kept: the interesting writes are all in the
     * first moments, and the question is whether a 0x20 ever appears at all.
     */
    ULONG L2PicWrites[16];
    ULONG L2PicWriteIndex;
    ULONGLONG L2PicWriteTotal;
    /*
     * The mask as it stands now, which is the value that decides everything.
     *
     * The table above keeps the first sixteen writes, and the first sixteen
     * are the initialisation sequence - they show the master being programmed
     * to base 0x08 and two end-of-interrupt writes, which is exactly what a
     * healthy PIC looks like.  What they cannot show is where the mask ended
     * up, because there are thirty-three writes and the table stopped at
     * sixteen.  Same shape of mistake as the RIP table: a first-come record
     * answers "did this ever happen", never "what is it now".
     *
     * In OCW1 a set bit masks its line, so bit 0 set means the timer is off.
     * Steady-state port I/O is zero, so whatever was written last is what the
     * guest is still living with - a mask is not re-asserted, it persists.
     */
    ULONG L2PicLastMaster;
    ULONG L2PicLastSlave;
    ULONGLONG L2PicMaskWrites;
    /*
     * Every byte L2 sends the interval timer, in order.
     *
     * The last device in the chain that has not been looked at.  The guest's
     * controller is programmed correctly and its timer line is unmasked, and
     * L1 still injected about a dozen interrupts and then stopped - which is
     * also exactly what a timer programmed for a single shot would produce.
     *
     * Port 0x43 is the command register: bits 5:4 select the access pattern
     * and bits 3:1 the mode, where mode 0 fires once and modes 2 and 3 are
     * the periodic ones a tick needs.  0x40 is counter zero's data port, the
     * divisor, written low byte then high.
     */
    ULONG L2PitWrites[16];
    ULONG L2PitWriteIndex;
    ULONGLONG L2PitWriteTotal;
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
     * The same "did the shape change" test, kept per region.
     *
     * The single-region version compared against whatever was spilled last,
     * which works only while there is one of them.  With two virtual
     * processors L1 alternates regions on every spill, so "the physical
     * address differs from last time" is true every single time and the row
     * fires on every spill: measured 1,254 of 1,280 rows in one window, with
     * 54,897 rows dropped off the back of the ring and every diagnostic row
     * buried with them.  A guard that stops guarding as soon as the guest gets
     * a second CPU is worse than no guard - it silently takes the instrument
     * away exactly when the thing being investigated needs two processors.
     */
    ULONGLONG RegionStoreSlotPhysical[4];
    ULONG RegionStoreSlotEntries[4];
    /*
     * A bound on the rows a fifth region can produce.
     *
     * The four slots above exist so a spill that changed nothing stays quiet.
     * When L1 has more regions than that, the fallback was "report every spill
     * rather than none" - and once the guest started triple-faulting, VMware
     * allocated a fresh VMCS per reset and that branch took over the ring
     * completely: a two-thousand-row sample came back a hundred percent this
     * one row, with every other reading aged out behind it.  Bounded here,
     * with the suppressed count carried on the row so the truncation is
     * visible rather than silent.
     */
    ULONG RegionStoreOverflowRows;
    ULONGLONG RegionStoreOverflowSuppressed;
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
    /*
     * And which address faulted, for the exits where that is what moves.
     *
     * One instruction faulting its way across hundreds of thousands of pages
     * has one RIP and one RCX; only the guest-physical address says it is
     * getting somewhere.  Zero for every exit reason that does not define
     * that field.
     */
    ULONGLONG L2ProgressFaultAddress;
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
