/* Vendor dispatch without introducing a function-pointer call on every VMEXIT. */
#include "hvm_backend.h"
#if defined(_M_AMD64)
#include "hvm_svm.h"
/* Baseline AMD lifecycle; optional Intel features never reach these operations. */
static const KSW_HVM_BACKEND_OPS KswSvmOps = {
    /* Hardware probe and strict option validator. */
    KswordSvmProbe, KswordSvmValidateFlags,
    /* Independent resource ownership and release. */
    KswordSvmPrepare, KswordSvmRelease,
    /* Shared-phase CPU self-test, start and stop. */
    KswordSvmSelfTest, KswordSvmStart, KswordSvmStop
};
#endif

/* Backend choice is explicit; non-x64 architectures never link SVM assembly. */
const KSW_HVM_BACKEND_OPS* KswordHvmBackend(ULONG Backend)
{
#if defined(_M_AMD64)
    /* Only SVM takes the new execution boundary in this increment. */
    if (Backend == KSWORD_ARK_HVM_BACKEND_SVM) { return &KswSvmOps; }
#else
    /* ARM64 has no AMD64 SVM backend. */
    UNREFERENCED_PARAMETER(Backend);
#endif
    /* Intel remains handled by existing VMX functions and tests. */
    return NULL;
}

/* Caller holds the runtime lifetime lock; CPU-local counters may still change. */
VOID KswordHvmBackendQuery(KSW_HVM_RUNTIME* Runtime, KSWORD_ARK_QUERY_HVM_RESPONSE* Response)
{
    /* Always identify the architecture even before prepare. */
    Response->backend = Runtime->BackendId;
    /* Consumers must not mistake a successful control for a power transition. */
    Response->powerGeneration = (ULONG)Runtime->PowerTransitionGeneration;
    /* Missing privileged evidence never becomes an implicit zero-valued success. */
    Response->svmCapabilities = Runtime->SvmCapabilities;
    /* No active backend means no claimed translation implementation. */
    Response->slatType = Runtime->BackendId == KSWORD_ARK_HVM_BACKEND_SVM ? KSWORD_ARK_HVM_SLAT_NPT :
        (Runtime->BackendId == KSWORD_ARK_HVM_BACKEND_VMX ? KSWORD_ARK_HVM_SLAT_EPT : KSWORD_ARK_HVM_SLAT_NONE);
    /* Generic readiness follows real prepared ownership. */
    Response->slatReady = (Runtime->StateFlags & KSWORD_ARK_HVM_STATE_RESOURCES_READY) != 0;
    /* Retain a backend-specific status without overloading VM-instruction error. */
    Response->backendStatus = (ULONG)Runtime->LastStatus;
#if defined(_M_AMD64)
    /* AMD rows describe SVM state, not a zero-valued VMX result. */
    if (Runtime->BackendId == KSWORD_ARK_HVM_BACKEND_SVM && Runtime->BackendContext != NULL) {
        /* Resources are kept alive by the query caller's shared lock. */
        KSW_SVM_STATE* state = Runtime->BackendContext;
        /* Traverse the exact prepared CPU set. */
        ULONG index;
        /* NPT readiness requires an actual complete root. */
        Response->slatReady = state->Npt.RootPa != 0;
        /* Read public row additions without dereferencing unrelated Intel contexts. */
        for (index = 0; index < state->Count; ++index) {
            /* CPU-local stages are naturally aligned atomic words. */
            Response->processors[index].executionStage = (ULONG)state->Cpus[index].Stage;
            /* Make the correct decoder explicit per CPU. */
            Response->processors[index].backend = KSWORD_ARK_HVM_BACKEND_SVM;
        }
    }
#endif
}
