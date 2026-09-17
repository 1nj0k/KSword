/* Snapshot sparse AMD exit telemetry without reinterpreting Intel exit numbers. */
#include "hvm_svm.h"

/* Runtime lifetime is protected by the metrics query's existing shared lock. */
VOID KswordSvmMetrics(KSW_HVM_RUNTIME* Runtime, KSWORD_ARK_HVM_METRICS_RESPONSE* Response)
{
    /* No processor-private buffer is valid before preparation. */
    KSW_SVM_STATE* state = Runtime->BackendContext;
    /* Iterate the exact prepared set, bounded by the shared response capacity. */
    ULONG index;
    /* Always identify the selected architecture, including before prepare. */
    Response->backend = Runtime->BackendId;
    /* Intel shadow counters do not apply to AMD. */
    Response->shadowProcessorCount = 0;
    /* A failed or absent preparation leaves zero AMD rows. */
    if (state == NULL || state->Cpus == NULL) { return; }
    /* CPU identity remains meaningful even before the first recorded exit. */
    Response->svmProcessorCount = state->Count;
    /* Copy processor-local snapshots with bounded retries. */
    for (index = 0; index < state->Count; ++index) {
        /* Query storage is separate from the live ring. */
        KSWORD_ARK_HVM_SVM_METRICS* output = &Response->svmProcessors[index];
        /* Select this processor's private state. */
        KSW_SVM_CPU* cpu = &state->Cpus[index];
        /* Limit retry effort under a high exit rate. */
        ULONG attempt;
        /* Stable identity does not imply the exit snapshot is valid. */
        output->group = cpu->Resource->Row.processorGroup; output->number = cpu->Resource->Row.processorNumber;
        /* Preserve lifecycle stage independently from any exit record. */
        output->stage = (ULONG)cpu->Stage;
        /* Cleanup does not overwrite the cause that required it. */
        output->failureStatus = (ULONG)cpu->FailureStatus; output->failureStage = cpu->FailureStage;
        /* The initial implementation reserves ASID one per processor. */
        output->asid = 1;
        /* MSR validity is independent of whether a VMEXIT record exists. */
        output->msrValidMask = cpu->Caps.Valid; output->svmFeatures = cpu->Caps.Features;
        /* Preserve enumeration used by allocation and ASID selection. */
        output->asidCount = cpu->Caps.AsidCount; output->physicalBits = cpu->Caps.PhysicalBits;
        /* Preserve each raw ownership observation. */
        output->observedVmCr = cpu->Caps.VmCr; output->observedEfer = cpu->Caps.Efer; output->observedHsave = cpu->Caps.Hsave;
        /* Tag this snapshot with the public lifecycle generation. */
        output->generation = Runtime->Generation;
        /* Prepared immutable resource addresses aid dump attribution. */
        output->vmcbPa = cpu->GuestPa; output->hsavePa = cpu->HsavePa; output->nptRootPa = state->Npt.RootPa;
        /* Observed VMRUN completions requested the baseline full flush. */
        output->tlbRequests = *(volatile ULONGLONG*)&cpu->TlbRequests;
        /* Read at most three times; an invalid snapshot is preferable to a root stall. */
        for (attempt = 0; attempt < 3; ++attempt) {
            /* Observe the last completely published ring position. */
            ULONG position = (ULONG)InterlockedCompareExchange((volatile LONG*)&cpu->TracePosition, 0, 0);
            /* No exit has been observed yet. */
            KSW_SVM_TRACE* row;
            /* Sequence values bracket the entire copy. */
            LONG before, after;
            /* Preserve empty-ring validity explicitly. */
            if (position == 0) { break; }
            /* Select the last published row; wrap is intentional and counted. */
            row = &cpu->Trace[(position - 1) % KSW_SVM_TRACE_ROWS];
            /* Odd sequence means a writer has begun reusing this slot. */
            before = InterlockedCompareExchange(&row->Sequence, 0, 0);
            /* Avoid copying an already inconsistent row. */
            if (before & 1) { continue; }
            /* Copy all raw 64-bit evidence with no narrowing. */
            output->exitCode = row->ExitCode; output->exitInfo1 = row->Info1; output->exitInfo2 = row->Info2;
            /* Preserve continuation identity. */
            output->rip = row->Rip; output->rsp = row->Rsp; output->cr3 = row->Cr3;
            /* Preserve instruction/event evidence and CPU-local timestamp. */
            output->nrip = row->Nrip; output->event = row->Event; output->tsc = row->Tsc;
            /* Acquire barrier detects a writer that raced the copy. */
            after = InterlockedCompareExchange(&row->Sequence, 0, 0);
            /* A stable even sequence proves this row's internal coherence. */
            if (before == after && !(after & 1)) {
                /* Publish the exact observed sequence and overwrite accounting. */
                output->valid = 1; output->sequence = (ULONG)after; output->ringPosition = position;
                /* Ring overwrite is not the same thing as publication failure. */
                output->ringOverwritten = position > KSW_SVM_TRACE_ROWS ? position - KSW_SVM_TRACE_ROWS : 0;
                /* No extra retries after obtaining a coherent snapshot. */
                break;
            }
        }
    }
}
