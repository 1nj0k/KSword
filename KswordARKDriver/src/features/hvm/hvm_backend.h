/* Narrow backend boundary; Intel continues through the existing VMX adapter. */
#pragma once
#include "hvm_internal.h"

/* Execution policy is selected once, never guessed from a VM-exit number. */
typedef struct _KSW_HVM_BACKEND_OPS {
    /* Probe current hardware without claiming VMRUN evidence. */
    NTSTATUS (*ProbeCapabilities)(KSW_HVM_RUNTIME*);
    /* Reject unimplemented options before any allocation. */
    NTSTATUS (*ValidateStartFlags)(KSW_HVM_RUNTIME*, ULONG);
    /* PASSIVE_LEVEL resource preparation. */
    NTSTATUS (*PrepareResources)(KSW_HVM_RUNTIME*, ULONG);
    /* Release only after native ownership is proven. */
    VOID (*ReleaseResources)(KSW_HVM_RUNTIME*);
    /* Run the complete processor-pinned self-test set. */
    NTSTATUS (*SelfTest)(KSW_HVM_RUNTIME*, ULONG);
    /* Transactional all-CPU resident start. */
    NTSTATUS (*Start)(KSW_HVM_RUNTIME*, ULONG);
    /* Transactional all-CPU native return. */
    NTSTATUS (*Stop)(KSW_HVM_RUNTIME*);
} KSW_HVM_BACKEND_OPS;
/* NULL selects the unchanged legacy Intel VMX execution path. */
const KSW_HVM_BACKEND_OPS* KswordHvmBackend(ULONG Backend);
/* Query processor state using the selected vendor's interpretation. */
VOID KswordHvmBackendQuery(KSW_HVM_RUNTIME* Runtime, KSWORD_ARK_QUERY_HVM_RESPONSE* Response);
