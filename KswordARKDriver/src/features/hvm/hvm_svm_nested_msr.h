/* Software ownership of SVM MSRs; never forwards an operand to a physical MSR. */
#pragma once
#include "hvm_svm_arch.h"
/* Return codes distinguish an owned access, #GP, and an unrelated MSR. */
#define KSW_NSVM_MSR_OK 0U
/* A rejected access must preserve RIP and inject #GP(0). */
#define KSW_NSVM_MSR_GP 1U
/* The outer dispatcher retains ownership of other intercepted MSRs. */
#define KSW_NSVM_MSR_OTHER 2U
/* Virtual state belongs to one logical processor, not to a process or APIC ID. */
typedef struct _KSW_NSVM_MSRS {
    /* EFER.SVME is guest-owned here, distinct from the real backend-owned bit. */
    KSW_SVM_U64 Efer;
    /* Hardware's opaque HSAVE format is never used for virtual host state. */
    KSW_SVM_U64 Hsave;
    /* Firmware configuration is exposed read-only by this first implementation. */
    KSW_SVM_U64 VmCr;
    /* Validated address mask captured at prepare. */
    KSW_SVM_U64 AddressMask;
} KSW_NSVM_MSRS;
/* Baseline admits SVME transitions, aligned HSAVE and idempotent VM_CR only. */
static __inline unsigned int KswSvmNestedMsrAccess(KSW_NSVM_MSRS* State,
    unsigned int Msr, unsigned int Write, KSW_SVM_U64* Value)
{
    /* EFER changes beyond virtual SVM ownership need separate mode validation. */
    if (Msr == KSW_SVM_MSR_EFER) {
        /* An unimplemented paging-mode change cannot silently reach real EFER. */
        if (Write && ((*Value ^ State->Efer) & ~KSW_SVM_EFER_SVME)) { return KSW_NSVM_MSR_GP; }
        /* Virtual firmware disable remains authoritative. */
        if (Write && (*Value & KSW_SVM_EFER_SVME) && (State->VmCr & 0x10ULL)) { return KSW_NSVM_MSR_GP; }
        /* Update only the software image. */
        if (Write) { State->Efer = *Value; }
        /* RDMSR must report the virtual bit, not the monitor's real SVME. */
        else { *Value = State->Efer; }
        /* This MSR access completed. */
        return KSW_NSVM_MSR_OK;
    }
    /* HSAVE is a virtual address/ownership declaration, not a WRMSR passthrough. */
    if (Msr == KSW_SVM_MSR_HSAVE) {
        /* Refuse reserved low/high bits and absent address-width evidence. */
        if (Write && (!State->AddressMask || (*Value & ~State->AddressMask))) { return KSW_NSVM_MSR_GP; }
        /* Zero is useful when the guest relinquishes ownership. */
        if (Write) { State->Hsave = *Value; }
        /* Never disclose the real host-save page. */
        else { *Value = State->Hsave; }
        /* No physical ownership register was touched. */
        return KSW_NSVM_MSR_OK;
    }
    /* Firmware configuration is read-only except harmless identical writes. */
    if (Msr == KSW_SVM_MSR_VM_CR) {
        /* Changes to locks or firmware disable are outside this implementation. */
        if (Write && *Value != State->VmCr) { return KSW_NSVM_MSR_GP; }
        /* Return only the captured virtual firmware image. */
        *Value = State->VmCr;
        /* The read or identical write completed. */
        return KSW_NSVM_MSR_OK;
    }
    /* Let the outer policy decide unrelated MSRs. */
    return KSW_NSVM_MSR_OTHER;
}
