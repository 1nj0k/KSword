/* Bounded transition timing and invalidation/resource accounting. */
#include "hvm_metrics.h"
#include "hvm_internal.h"
#include "hvm_ept.h"

/* Static storage outlives resident contexts and permits queries after stop. */
static KSWORD_ARK_HVM_METRICS_RESPONSE g_HvmTiming;
/* Odd sequences identify a transition whose stamps are still being written. */
static volatile LONG g_HvmTimingSequence;
/* These counters survive resource teardown and reset only at driver load. */
static volatile LONG64 g_HvmInveptAttempts, g_HvmInveptSucceeded, g_HvmInveptFailed;
static volatile LONG64 g_HvmRuleAllocations, g_HvmRuleFrees;
static volatile LONG64 g_HvmReplacementAllocations, g_HvmReplacementFrees;

VOID KswordARKHvmMetricsBegin(ULONG Command)
{
    /* The caller owns runtime control serialization before entering here. */
    KSW_HVM_RUNTIME* runtime = KswordARKHvmGetRuntime();
    LARGE_INTEGER frequency;
    ULONG index;
    /* Mark the snapshot incomplete before changing any measured field. */
    (void)InterlockedIncrement(&g_HvmTimingSequence);
    /* Keep allocator and invalidation counters separate from transition reset. */
    RtlZeroMemory(&g_HvmTiming, sizeof(g_HvmTiming));
    /* Capture the actual platform QPC frequency alongside its first reading. */
    g_HvmTiming.commandBeginQpc = (ULONGLONG)KeQueryPerformanceCounter(&frequency).QuadPart;
    /* Store the frequency rather than assuming Hyper-V's usual 10 MHz clock. */
    g_HvmTiming.qpcFrequency = (ULONGLONG)frequency.QuadPart;
    /* Preserve the exact control verb whose interval is being measured. */
    g_HvmTiming.command = Command;
    /* Clamp every reported processor to the protocol's fixed capacity. */
    g_HvmTiming.processorCount = min(runtime->ProcessorCount, KSWORD_ARK_HVM_MAX_PROCESSORS);
    /* Capture stable processor identities before entering the rendezvous. */
    for (index = 0UL; runtime->Processors != NULL && index < g_HvmTiming.processorCount; ++index) {
        /* Record the Windows processor group without assuming group zero. */
        g_HvmTiming.processors[index].group = runtime->Processors[index].Row.processorGroup;
        /* Record the group-relative processor number used by the runtime. */
        g_HvmTiming.processors[index].number = runtime->Processors[index].Row.processorNumber;
    }
}

VOID KswordARKHvmMetricsEnd(NTSTATUS Status)
{
    /* Preserve failure intervals as well as successful transitions. */
    g_HvmTiming.lastStatus = (ULONG)Status;
    /* Finish the measured control body before advertising completeness. */
    g_HvmTiming.commandEndQpc = (ULONGLONG)KeQueryPerformanceCounter(NULL).QuadPart;
    /* Interlocked publication orders every earlier CPU stamp before readers. */
    (void)InterlockedIncrement(&g_HvmTimingSequence);
}

VOID KswordARKHvmMetricsStamp(ULONG Stage)
{
    /* Ignore automatic callbacks outside an instrumented control operation. */
    if ((InterlockedCompareExchange(&g_HvmTimingSequence, 0L, 0L) & 1L) == 0L ||
        Stage >= KSW_HVM_TIME_GLOBAL_STAGES) { return; }
    /* Keep the first interval boundary when a start needs a rollback rendezvous. */
    if ((g_HvmTiming.globalValidMask & (1UL << Stage)) != 0UL) { return; }
    /* Capture a single QPC reading at this exact boundary. */
    g_HvmTiming.globalQpc[Stage] = (ULONGLONG)KeQueryPerformanceCounter(NULL).QuadPart;
    /* Publish its validity after its complete 64-bit value. */
    g_HvmTiming.globalValidMask |= 1UL << Stage;
}

VOID KswordARKHvmMetricsCpuStamp(ULONG Index, ULONG Stage)
{
    /* Every processor writes only its own fixed row during the rendezvous. */
    KSWORD_ARK_HVM_METRICS_CPU* cpu;
    /* Reject incomplete intervals and indices before touching their storage. */
    if ((InterlockedCompareExchange(&g_HvmTimingSequence, 0L, 0L) & 1L) == 0L ||
        Index >= g_HvmTiming.processorCount || Stage >= KSW_HVM_TIME_CPU_STAGES) { return; }
    /* Resolve this processor's preallocated timing row. */
    cpu = &g_HvmTiming.processors[Index];
    /* Do not overwrite failed-start evidence with the cleanup callback. */
    if ((cpu->validMask & (1UL << Stage)) != 0UL) { return; }
    /* Capture the boundary without allocating or publishing event-ring records. */
    cpu->qpc[Stage] = (ULONGLONG)KeQueryPerformanceCounter(NULL).QuadPart;
    /* Publish availability after storing the timestamp. */
    cpu->validMask |= 1UL << Stage;
}

VOID KswordARKHvmMetricsAllocation(BOOLEAN Replacement, BOOLEAN Free)
{
    /* Count each successful allocation or actual free exactly once. */
    volatile LONG64* counter = Replacement
        ? (Free ? &g_HvmReplacementFrees : &g_HvmReplacementAllocations)
        : (Free ? &g_HvmRuleFrees : &g_HvmRuleAllocations);
    /* Pair ledger updates even on pre-publication failure cleanup. */
    (void)InterlockedIncrement64(counter);
}

NTSTATUS KswordARKHvmMetricsQuery(KSWORD_ARK_HVM_METRICS_RESPONSE* Response)
{
    /* Snapshot boundaries describe the observation's own duration. */
    ULONGLONG begin;
    LONG before, after;
    LARGE_INTEGER frequency;
    /* Never copy a fixed response to a missing validated output buffer. */
    if (Response == NULL) { return STATUS_INVALID_PARAMETER; }
    /* Timestamp the beginning before reading any counters or transition data. */
    begin = (ULONGLONG)KeQueryPerformanceCounter(&frequency).QuadPart;
    /* Read the writer sequence before copying its state. */
    before = InterlockedCompareExchange(&g_HvmTimingSequence, 0L, 0L);
    /* Copy fixed storage; the subsequent sequence check decides coherence. */
    RtlCopyMemory(Response, &g_HvmTiming, sizeof(*Response));
    /* Re-read after the copy to detect an intervening transition. */
    after = InterlockedCompareExchange(&g_HvmTimingSequence, 0L, 0L);
    /* Identify this independently versioned response. */
    Response->version = KSWORD_ARK_HVM_METRICS_VERSION;
    /* Report the exact buffer contract used by the driver. */
    Response->size = sizeof(*Response);
    /* Queries before the first transition still carry the counter's units. */
    Response->qpcFrequency = (ULONGLONG)frequency.QuadPart;
    /* A never-started or concurrently changing record is explicitly incomplete. */
    Response->transitionCoherent = before != 0L && before == after && (after & 1L) == 0L;
    /* Allow callers to require the same measured transition across queries. */
    Response->transitionSequence = (ULONG)after;
    /* Read independently atomic counters without claiming a simultaneous snapshot. */
    Response->inveptAttempts = (ULONGLONG)InterlockedCompareExchange64(&g_HvmInveptAttempts, 0LL, 0LL);
    /* Count instructions that actually returned success. */
    Response->inveptSucceeded = (ULONGLONG)InterlockedCompareExchange64(&g_HvmInveptSucceeded, 0LL, 0LL);
    /* Include failed instructions and exceptional returns. */
    Response->inveptFailed = (ULONGLONG)InterlockedCompareExchange64(&g_HvmInveptFailed, 0LL, 0LL);
    /* Export the rule-object allocation ledger independently of active slots. */
    Response->ruleAllocations = (ULONGLONG)InterlockedCompareExchange64(&g_HvmRuleAllocations, 0LL, 0LL);
    /* Export actual rule-object frees, including rejected preparations. */
    Response->ruleFrees = (ULONGLONG)InterlockedCompareExchange64(&g_HvmRuleFrees, 0LL, 0LL);
    /* Export actual successful replacement-page allocations. */
    Response->replacementAllocations = (ULONGLONG)InterlockedCompareExchange64(&g_HvmReplacementAllocations, 0LL, 0LL);
    /* Export actual replacement-page reclamation. */
    Response->replacementFrees = (ULONGLONG)InterlockedCompareExchange64(&g_HvmReplacementFrees, 0LL, 0LL);
    /* Preserve the first timestamp and close the snapshot's observation interval. */
    Response->snapshotBeginQpc = begin;
    /* Capture the final timestamp after every independently sampled value. */
    Response->snapshotEndQpc = (ULONGLONG)KeQueryPerformanceCounter(NULL).QuadPart;
    /* Incoherent timing remains a successful query with explicit validity. */
    return STATUS_SUCCESS;
}

#if defined(_M_AMD64)
/* The assembly leaf alone executes INVEPT; every caller uses this wrapper. */
extern UCHAR KswordARKHvmAsmInveptSingleRaw(ULONGLONG EptPointer);

UCHAR KswordARKHvmAsmInveptSingle(ULONGLONG EptPointer)
{
    /* An exception is a failed attempt even if no VM-instruction code returns. */
    UCHAR result = 0xFFU;
    /* Count attempts before entering the instruction, including exceptions. */
    (void)InterlockedIncrement64(&g_HvmInveptAttempts);
    /* Preserve the original caller's exception handling and return contract. */
    __try {
        /* Execute precisely one single-context INVEPT. */
        result = KswordARKHvmAsmInveptSingleRaw(EptPointer);
    } __finally {
        /* Finalize accounting on both regular and abnormal instruction returns. */
        (void)InterlockedIncrement64(result == 0U ? &g_HvmInveptSucceeded : &g_HvmInveptFailed);
    }
    /* Forward the architecture's success/failure result unchanged. */
    return result;
}
#endif
