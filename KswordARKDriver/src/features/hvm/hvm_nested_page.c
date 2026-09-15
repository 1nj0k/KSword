/* Transactional control of a single composed-EPT page override. */
#include "hvm_nested_ept.h"
#include "hvm_resident.h"
#include "hvm_metrics.h"
#include "hvm_event.h"
#include "../../platform/pool_compat.h"

#if defined(_M_AMD64)
/* Use one tag for the allocation and every rejection/reclamation path. */
#define KSW_HVM_PAGE_POOL_TAG 'PvHK'
/* Permit only page-aligned architectural physical addresses. */
#define KSW_HVM_PAGE_FRAME_MASK 0x000FFFFFFFFFF000ULL
/* Operation ids survive resource teardown and reset only at driver load. */
static volatile LONG g_PageOperationSequence;

/* Fixed local metadata remains valid after its backing allocation is freed. */
typedef struct _KSW_HVM_PAGE_TRACE {
    ULONG Id, Operation, Flags;
    ULONGLONG EptPointer, GuestPage, BackingPage;
} KSW_HVM_PAGE_TRACE;

static VOID KswordARKHvmPageTrace(const KSW_HVM_PAGE_TRACE* Trace, ULONG Stage, NTSTATUS Status)
{
    /* Retain the existing event ABI and give its new type explicit semantics. */
    KSWORD_ARK_HVM_EVENT_ROW row = { 0 };
    PROCESSOR_NUMBER processor;
    /* Queries do not advance the transaction trace or generate events. */
    if (Trace->Id == 0UL) { return; }
    /* Record the control caller, not a claim of per-CPU invalidation timing. */
    (void)KeGetCurrentProcessorNumberEx(&processor);
    /* Select the separately decoded page transaction event type. */
    row.type = KSWORD_ARK_HVM_EVENT_TYPE_NESTED_PAGE;
    /* Distinguish this transaction from older events in the same ring. */
    row.ruleId = Trace->Id;
    /* Stage and operation occupy fields otherwise used for VM-exit metadata. */
    row.exitReason = Stage;
    /* Preserve map/remove identity, including fault-injection requests. */
    row.access = Trace->Operation;
    /* Keep the exact explicit fault mode in the trace. */
    row.qualification = Trace->Flags;
    /* Record the target descendant GPA. */
    row.guestPhysicalAddress = Trace->GuestPage;
    /* For this event type, this field is the EPT12 root, not a linear address. */
    row.guestLinearAddress = Trace->EptPointer;
    /* For this event type, this field is backing PA, not an instruction pointer. */
    row.guestRip = Trace->BackingPage;
    /* Record the complete operation result at this boundary. */
    row.status = Status;
    /* Preserve the observing processor group. */
    row.processorGroup = processor.Group;
    /* Preserve the observing processor number. */
    row.processorNumber = processor.Number;
    /* The event publisher stamps QPC and reports ring overwrite/drop counts. */
    KswordARKHvmEventPublish(&row);
}

static VOID KswordARKHvmNestedPageFree(KSW_HVM_NESTED_PAGE* Page)
{
    /* Failed allocation and empty removal both permit an empty record. */
    if (Page == NULL) { return; }
    /* Backing may be absent after an allocation failure. */
    if (Page->ShadowVirtual != NULL) {
        /* Caller has either never published it or drained all possible readers. */
        MmFreeContiguousMemory(Page->ShadowVirtual);
        /* Count actual frees independently of mapping-slot occupancy. */
        KswordARKHvmMetricsAllocation(TRUE, TRUE);
    }
    /* Pair the rule's exact tagged allocation. */
    ExFreePoolWithTag(Page, KSW_HVM_PAGE_POOL_TAG);
    /* Keep failed preparations visible in the object ledger. */
    KswordARKHvmMetricsAllocation(FALSE, TRUE);
}

VOID KswordARKHvmNestedPageResetLocked(KSW_HVM_RUNTIME* Runtime)
{
    /* VMXOFF on every CPU is required for teardown without another rendezvous. */
    if (Runtime->ResidentProcessorCount != 0L) { return; }
    /* Resource teardown owns the runtime lock and cannot race a publication. */
    KswordARKHvmNestedPageFree((KSW_HVM_NESTED_PAGE*)InterlockedExchangePointer(
        (PVOID volatile*)&Runtime->NestedPage, NULL));
    /* Reclaim a retained page only after the stopped lifecycle is proven. */
    KswordARKHvmNestedPageFree(Runtime->NestedPageRetired);
    /* Publish the empty retention slot. */
    Runtime->NestedPageRetired = NULL;
    /* Invalidate stale generation-bound user requests. */
    Runtime->NestedPageGeneration += 1UL;
}

static NTSTATUS KswordARKHvmPageRetire(KSW_HVM_RUNTIME* Runtime,
    KSW_HVM_PAGE_TRACE* Trace, BOOLEAN FailFlush)
{
    NTSTATUS status;
    KSW_HVM_NESTED_PAGE* page;
    BOOLEAN unpublished = FALSE;
    /* A failed earlier attempt is retried without losing its pinned backing. */
    if (Runtime->NestedPageRetired == NULL) {
        /* Stop new root readers from discovering the override. */
        Runtime->NestedPageRetired = (KSW_HVM_NESTED_PAGE*)InterlockedExchangePointer(
            (PVOID volatile*)&Runtime->NestedPage, NULL);
        /* The logical mapping changes even if subsequent invalidation fails. */
        Runtime->NestedPageGeneration += 1UL;
        /* Delay the event until its retained allocation identity is available. */
        unpublished = TRUE;
    }
    /* Copy identities before any possible free. */
    page = Runtime->NestedPageRetired;
    /* Empty removes still validate the invalidation path. */
    if (page != NULL) {
        /* Preserve the actual retained allocation's EPT identity on retries. */
        Trace->EptPointer = page->Ept12Pointer;
        /* Preserve its target GPA for removal requests without an address. */
        Trace->GuestPage = page->GuestPhysicalPage;
        /* Preserve its backing PA beyond reclamation. */
        Trace->BackingPage = page->ShadowPhysicalPage;
    }
    /* Unpublication is distinct from successful hardware invalidation. */
    if (unpublished) { KswordARKHvmPageTrace(Trace, KSW_HVM_PAGE_UNPUBLISHED, STATUS_SUCCESS); }
    /* Bound the all-CPU drain independently of the user-mode command. */
    KswordARKHvmPageTrace(Trace, KSW_HVM_PAGE_ROLLBACK_BEGIN, STATUS_SUCCESS);
    /* Fault injection omits this drain; it never reports an unexecuted INVEPT. */
    status = FailFlush ? STATUS_HV_OPERATION_FAILED : KswordARKHvmResidentInvalidateEpt(Runtime->EptPointer);
    /* A failed drain leaves all possibly referenced allocations pinned. */
    KswordARKHvmPageTrace(Trace, KSW_HVM_PAGE_ROLLBACK_END, status);
    /* Reclamation is permitted only after every participant acknowledged. */
    if (NT_SUCCESS(status) && page != NULL) {
        /* Both the object and backing are now unreachable by resident readers. */
        KswordARKHvmNestedPageFree(page);
        /* Publish successful retirement after actual reclamation. */
        Runtime->NestedPageRetired = NULL;
        /* Timestamp after free, retaining identities in the local trace. */
        KswordARKHvmPageTrace(Trace, KSW_HVM_PAGE_RECLAIMED, STATUS_SUCCESS);
    }
    /* Return actual drain status; slot occupancy alone is not success. */
    return status;
}

NTSTATUS KswordARKHvmNestedPageControl(const KSWORD_ARK_HVM_NESTED_PAGE_REQUEST* Request,
    KSWORD_ARK_HVM_NESTED_PAGE_RESPONSE* Response)
{
    KSW_HVM_RUNTIME* runtime = KswordARKHvmGetRuntime();
    KSW_HVM_NESTED_PAGE* page;
    KSW_HVM_PAGE_TRACE trace = { 0 };
    NTSTATUS status = STATUS_SUCCESS;
    ULONG index, fault;
    /* Validate fixed input/output pointers before any state access. */
    if (Request == NULL || Response == NULL || runtime == NULL) { return STATUS_INVALID_PARAMETER; }
    /* Clear every response field, including inactive-page identities. */
    RtlZeroMemory(Response, sizeof(*Response));
    /* Preserve the existing wire layout and version. */
    Response->version = KSWORD_ARK_HVM_NESTED_PAGE_VERSION;
    /* Report the exact compatible fixed buffer length. */
    Response->size = sizeof(*Response);
    /* Prevent asynchronous kernel APCs while owning the push lock. */
    KeEnterCriticalRegion();
    /* Serialize publication, retirement, lifecycle changes, and retries. */
    ExAcquirePushLockExclusive(&runtime->Lock);
    /* Enumerate roots under the same control serialization. */
    KswordARKHvmResidentNestedRoots(Response);
    /* Decode a request-local fault; no global fault switch remains armed. */
    fault = (Request->flags & KSWORD_ARK_HVM_NESTED_PAGE_FAULT_MASK) >> KSWORD_ARK_HVM_NESTED_PAGE_FAULT_SHIFT;
    /* Reject unknown versions, reserved bits and unsupported fault combinations. */
    if (Request->version != KSWORD_ARK_HVM_NESTED_PAGE_VERSION ||
        Request->size != sizeof(*Request) || Request->reserved != 0UL ||
        Request->operation > KSWORD_ARK_HVM_NESTED_PAGE_REMOVE ||
        (Request->flags & ~(KSWORD_ARK_HVM_NESTED_PAGE_CONFIRMED | KSWORD_ARK_HVM_NESTED_PAGE_FAULT_MASK)) != 0UL ||
        fault > KSWORD_ARK_HVM_NESTED_PAGE_FAULT_REMOVE_FLUSH ||
        (fault != 0UL && ((Request->operation == KSWORD_ARK_HVM_NESTED_PAGE_QUERY) ||
         (Request->operation == KSWORD_ARK_HVM_NESTED_PAGE_MAP && fault == KSWORD_ARK_HVM_NESTED_PAGE_FAULT_REMOVE_FLUSH) ||
         (Request->operation == KSWORD_ARK_HVM_NESTED_PAGE_REMOVE && fault != KSWORD_ARK_HVM_NESTED_PAGE_FAULT_REMOVE_FLUSH)))) {
        /* Return a semantic rejection without changing mappings or generations. */
        status = STATUS_INVALID_PARAMETER;
        /* Fill the complete query-compatible response below. */
        goto complete;
    }
    /* A query never emits operation events or mutates the page policy. */
    if (Request->operation == KSWORD_ARK_HVM_NESTED_PAGE_QUERY) { goto complete; }
    /* Preserve explicit confirmation on every mutation, including lab faults. */
    if (Request->confirmationToken != KSWORD_ARK_HVM_CONTROL_CONFIRMATION_TOKEN ||
        (Request->flags & KSWORD_ARK_HVM_NESTED_PAGE_CONFIRMED) == 0UL) {
        /* Deny a request that lacks the existing write contract. */
        status = STATUS_ACCESS_DENIED;
        /* No allocation or publication has occurred. */
        goto complete;
    }
    /* Allocate a durable correlation id for an authorized operation. */
    trace.Id = (ULONG)InterlockedIncrement(&g_PageOperationSequence);
    /* Preserve the requested operation. */
    trace.Operation = Request->operation;
    /* Preserve confirmation and fault flags for attribution. */
    trace.Flags = Request->flags;
    /* Preserve the requested root. */
    trace.EptPointer = Request->ept12Pointer;
    /* Preserve the requested descendant physical page. */
    trace.GuestPage = Request->guestPhysicalPage;
    /* Timestamp the start before lifecycle and generation validation. */
    KswordARKHvmPageTrace(&trace, KSW_HVM_PAGE_BEGIN, STATUS_SUCCESS);
    /* Reject overlap with an incomplete lifecycle operation. */
    if (!runtime->Initialized || runtime->Busy) { status = STATUS_DEVICE_BUSY; goto complete; }
    /* Do not mutate a runtime whose CPU ownership is already uncertain. */
    if ((runtime->StateFlags & (KSWORD_ARK_HVM_STATE_FAULTED | KSWORD_ARK_HVM_STATE_ROLLBACK_REQUIRED)) != 0UL) {
        /* Keep any outstanding backing pinned for stopped resource teardown. */
        status = STATUS_INVALID_DEVICE_STATE;
        /* Report the fault without starting another rendezvous. */
        goto complete;
    }
    /* Stale requests cannot remove or replace another transaction's mapping. */
    if (Request->expectedGeneration != runtime->NestedPageGeneration) { status = STATUS_REVISION_MISMATCH; goto complete; }
    /* Removal and retry share the same drain-before-free implementation. */
    if (Request->operation == KSWORD_ARK_HVM_NESTED_PAGE_REMOVE) {
        /* Only the explicit removal test is permitted to omit this drain. */
        status = KswordARKHvmPageRetire(runtime, &trace, fault == KSWORD_ARK_HVM_NESTED_PAGE_FAULT_REMOVE_FLUSH);
        /* Return active/retired occupancy as observed after the attempt. */
        goto complete;
    }
    /* A map needs a running nested monitor and exclusive ownership of the slot. */
    if (runtime->ResidentProcessorCount == 0L || runtime->NestedPage != NULL || runtime->NestedPageRetired != NULL) {
        /* Never overwrite an allocation that may remain visible in a CPU cache. */
        status = STATUS_DEVICE_BUSY;
        /* Keep the existing mapping intact. */
        goto complete;
    }
    /* Reject alignment errors and addresses wider than the architectural field. */
    if ((Request->guestPhysicalPage & ~KSW_HVM_PAGE_FRAME_MASK) != 0ULL) { status = STATUS_INVALID_PARAMETER; goto complete; }
    /* Require a root actually observed by the resident nested runtime. */
    for (index = 0UL; index < Response->rootCount; ++index) {
        /* Exact EPTP matching preserves its translation configuration bits. */
        if (Response->ept12Roots[index] == Request->ept12Pointer) { break; }
    }
    /* Unknown roots never allocate replacement memory. */
    if (index == Response->rootCount) { status = STATUS_NOT_FOUND; goto complete; }
    /* Timestamp before either allocation. */
    KswordARKHvmPageTrace(&trace, KSW_HVM_PAGE_ALLOCATE_BEGIN, STATUS_SUCCESS);
    /* Allocate the rule independently of its contiguous backing. */
    page = (KSW_HVM_NESTED_PAGE*)KswordARKAllocateNonPagedPool(sizeof(*page), KSW_HVM_PAGE_POOL_TAG);
    /* An object allocation failure never publishes a partial record. */
    if (page == NULL) { status = STATUS_INSUFFICIENT_RESOURCES; goto complete; }
    /* Count actual object allocation. */
    KswordARKHvmMetricsAllocation(FALSE, FALSE);
    /* Initialize the optional backing before any cleanup can inspect it. */
    RtlZeroMemory(page, sizeof(*page));
    /* Allocation injection deliberately exercises cleanup of the allocated object. */
    if (fault != KSWORD_ARK_HVM_NESTED_PAGE_FAULT_ALLOCATE) {
        PHYSICAL_ADDRESS low = { 0 }, high, boundary = { 0 };
        /* Accept any allocatable backing PA supported by the current system. */
        high.QuadPart = MAXLONGLONG;
        /* Allocate one ordinary WB RAM page for the replacement. */
        page->ShadowVirtual = MmAllocateContiguousMemorySpecifyCache(PAGE_SIZE, low, high, boundary, MmCached);
    }
    /* An absent backing takes the same cleanup branch as a real allocation failure. */
    if (page->ShadowVirtual == NULL) {
        /* Reclaim the never-published rule. */
        KswordARKHvmNestedPageFree(page);
        /* Preserve an allocation-specific error. */
        status = STATUS_INSUFFICIENT_RESOURCES;
        /* Record the failure boundary without inventing an allocated page. */
        KswordARKHvmPageTrace(&trace, KSW_HVM_PAGE_ALLOCATE_END, status);
        /* Return with generations and active mappings unchanged. */
        goto complete;
    }
    /* Account for backing as soon as allocation succeeds. */
    KswordARKHvmMetricsAllocation(TRUE, FALSE);
    /* Initialize all content before root readers can discover it. */
    RtlCopyMemory(page->ShadowVirtual, Request->shadow, PAGE_SIZE);
    /* Resolve the actual backing PA for EPT and evidence. */
    page->ShadowPhysicalPage = (ULONGLONG)MmGetPhysicalAddress(page->ShadowVirtual).QuadPart;
    /* Store the root identity used by the composition path. */
    page->Ept12Pointer = Request->ept12Pointer;
    /* Store the target page used by the composition path. */
    page->GuestPhysicalPage = Request->guestPhysicalPage;
    /* Retain backing identity independently of object lifetime. */
    trace.BackingPage = page->ShadowPhysicalPage;
    /* Close allocation and initialization timing. */
    KswordARKHvmPageTrace(&trace, KSW_HVM_PAGE_ALLOCATE_END, STATUS_SUCCESS);
    /* Cancellation before publication requires no remote invalidation. */
    if (fault == KSWORD_ARK_HVM_NESTED_PAGE_FAULT_CANCEL) {
        /* Reclaim both allocations while no CPU can reference the rule. */
        KswordARKHvmNestedPageFree(page);
        /* Record completed reclamation with the retained backing identity. */
        KswordARKHvmPageTrace(&trace, KSW_HVM_PAGE_RECLAIMED, STATUS_SUCCESS);
        /* Distinguish a requested cancellation from normal mapping success. */
        status = STATUS_CANCELLED;
        /* Preserve the original generation. */
        goto complete;
    }
    /* Publish only a fully initialized rule with one release-ordered pointer swap. */
    (void)InterlockedExchangePointer((PVOID volatile*)&runtime->NestedPage, page);
    /* Invalidate stale control requests once publication occurs. */
    runtime->NestedPageGeneration += 1UL;
    /* Record publication separately from eventual cache coherence. */
    KswordARKHvmPageTrace(&trace, KSW_HVM_PAGE_PUBLISHED, STATUS_SUCCESS);
    /* Bound the commit invalidation operation. */
    KswordARKHvmPageTrace(&trace, KSW_HVM_PAGE_FLUSH_BEGIN, STATUS_SUCCESS);
    /* Simulate a failed call boundary without reporting an actual INVEPT failure. */
    status = fault == KSWORD_ARK_HVM_NESTED_PAGE_FAULT_COMMIT_FLUSH ?
        STATUS_HV_OPERATION_FAILED : KswordARKHvmResidentInvalidateEpt(runtime->EptPointer);
    /* Record real or explicitly injected commit status. */
    KswordARKHvmPageTrace(&trace, KSW_HVM_PAGE_FLUSH_END, status);
    /* Post-commit cancellation follows successful publication and invalidation. */
    if (NT_SUCCESS(status) && fault == KSWORD_ARK_HVM_NESTED_PAGE_FAULT_ROLLBACK) { status = STATUS_CANCELLED; }
    /* A failed commit must withdraw the override instead of silently leaving it active. */
    if (!NT_SUCCESS(status)) {
        /* Keep the original error; failed rollback remains visible as retired backing. */
        (void)KswordARKHvmPageRetire(runtime, &trace, FALSE);
    }
complete:
    /* A query reports the newest id; an operation reports its own correlation id. */
    Response->operationId = trace.Id != 0UL ? trace.Id : (ULONG)InterlockedCompareExchange(&g_PageOperationSequence, 0L, 0L);
    /* Retained memory is reported even when logical publication has ended. */
    page = runtime->NestedPage != NULL ? runtime->NestedPage : runtime->NestedPageRetired;
    /* Keep semantic operation failures independent from IOCTL transport success. */
    Response->status = NT_SUCCESS(status) ? 0UL : 1UL;
    /* Preserve the precise original operation status. */
    Response->lastStatus = (ULONG)status;
    /* Return the current control generation. */
    Response->generation = runtime->NestedPageGeneration;
    /* Report logical mapping occupancy. */
    Response->active = runtime->NestedPage != NULL;
    /* Report potentially referenced, unreclaimed backing. */
    Response->retired = runtime->NestedPageRetired != NULL;
    /* Preserve resident participant count for the caller's preconditions. */
    Response->residentProcessors = (ULONG)runtime->ResidentProcessorCount;
    /* Only dereference an allocation still owned by the runtime. */
    if (page != NULL) {
        /* Return its exact translation identity. */
        Response->ept12Pointer = page->Ept12Pointer;
        /* Return its exact descendant page. */
        Response->guestPhysicalPage = page->GuestPhysicalPage;
        /* Return its replacement backing. */
        Response->shadowPhysicalPage = page->ShadowPhysicalPage;
        /* Sample the original backing discovered during composition. */
        Response->originalPhysicalPage = (ULONGLONG)InterlockedCompareExchange64(&page->OriginalPhysicalPage, 0LL, 0LL);
        /* Sample actual composition hits independently. */
        Response->composedCount = (ULONGLONG)InterlockedCompareExchange64(&page->ComposedCount, 0LL, 0LL);
    }
    /* Finish the correlated trace before releasing the control lock. */
    KswordARKHvmPageTrace(&trace, KSW_HVM_PAGE_END, status);
    /* Release mutation serialization. */
    ExReleasePushLockExclusive(&runtime->Lock);
    /* Restore normal kernel APC delivery. */
    KeLeaveCriticalRegion();
    /* A complete semantic response was produced even when the request failed. */
    return STATUS_SUCCESS;
}
#endif
