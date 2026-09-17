/* Build AMD control/save state from the currently pinned Windows processor. */
#include "hvm_svm.h"
#include <intrin.h>

/* Decode the GDT format into AMD segment attributes and expanded limits. */
static BOOLEAN KswSvmSegment(KSW_SVM_VMCB* Vmcb, ULONG Offset, BOOLEAN System)
{
    /* Selector was captured by the assembly helper on this processor. */
    KSW_SVM_SEGMENT* segment = (KSW_SVM_SEGMENT*)((PUCHAR)Vmcb + Offset);
    /* GDTR was captured in the same operation. */
    KSW_SVM_SEGMENT* gdtr = (KSW_SVM_SEGMENT*)((PUCHAR)Vmcb + KSW_VMCB_GDTR);
    /* Null selectors remain architecturally unusable. */
    ULONG offset = segment->selector & ~7U;
    /* Raw descriptor words are read only from the current kernel GDT. */
    ULONGLONG descriptor;
    /* No null-segment memory read. */
    if (offset == 0) { return TRUE; }
    /* Local descriptor tables are outside the initial backend contract. */
    if ((segment->selector & 4U) || offset + (System ? 15U : 7U) > gdtr->limit) { return FALSE; }
    /* Never propagate a failed descriptor read as a zeroed valid segment. */
    __try {
        /* Read the low descriptor. */
        descriptor = *(volatile ULONGLONG*)(ULONG_PTR)(gdtr->base + offset);
        /* Convert access byte and AVL/L/DB/G flags into AMD attributes. */
        segment->attributes = (USHORT)(((descriptor >> 40) & 0xffULL) | ((descriptor >> 44) & 0xf00ULL));
        /* Reconstruct the twenty-bit limit. */
        segment->limit = (ULONG)((descriptor & 0xffffULL) | ((descriptor >> 32) & 0xf0000ULL));
        /* Expand granularity exactly once. */
        if (descriptor & (1ULL << 55)) { segment->limit = (segment->limit << 12) | 0xfffU; }
        /* Reconstruct the low thirty-two base bits. */
        segment->base = ((descriptor >> 16) & 0xffffffULL) | ((descriptor >> 32) & 0xff000000ULL);
        /* Long-mode system segments contain a second descriptor word. */
        if (System) { segment->base |= (*(volatile ULONG*)(ULONG_PTR)(gdtr->base + offset + 8ULL)) * 0x100000000ULL; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        /* An unreadable GDT is not a usable entry context. */
        return FALSE;
    }
    /* Segment attributes now match the current Windows descriptor. */
    return TRUE;
}

/* Set one MSRPM bit after validating its architectural range. */
static VOID KswSvmInterceptMsr(KSW_SVM_CPU* Cpu, ULONG Msr, BOOLEAN WriteOnly)
{
    /* Two bits per MSR encode read then write. */
    ULONG bit = KswSvmMsrpmBit(Msr, WriteOnly ? 1U : 0U);
    /* All callers pass SVM/PAT/XSS encodings inside the three MSRPM windows. */
    if (bit == 0xffffffffU) { return; }
    /* Reads remain allowed when only a write could invalidate saved state. */
    ((PUCHAR)Cpu->Msrpm)[bit / 8] |= (UCHAR)(1U << (bit & 7));
    /* SVM ownership registers require both directions to be virtualized. */
    if (!WriteOnly) { ++bit; ((PUCHAR)Cpu->Msrpm)[bit / 8] |= (UCHAR)(1U << (bit & 7)); }
}

/* Called before enabling SVME, while pinned at DISPATCH_LEVEL or IPI_LEVEL. */
NTSTATUS KswordSvmBuildVmcb(KSW_SVM_CPU* Cpu)
{
    /* Shared NPT remains immutable while any processor is resident. */
    KSW_SVM_STATE* state = Cpu->Runtime->BackendContext;
    /* Save-area mapping is processor-private. */
    KSW_SVM_VMCB* v = Cpu->Guest;
    /* Decode the descriptors captured by assembly. */
    ULONG offset;
    /* Reject a changed XSAVE contract before writing virtualization state. */
    if ((__readcr4() & ((1ULL << 12) | (1ULL << 23) | (1ULL << 24) | (1ULL << 25))) || _xgetbv(0) != Cpu->XstateMask) { return STATUS_NOT_SUPPORTED; }
    /* Reinitialize control and state areas on every fresh launch. */
    RtlZeroMemory(v, sizeof(*v));
    /* Capture selector and GDTR/IDTR values on the target CPU. */
    KswordSvmAsmCaptureSegments(v);
    /* Decode ordinary segments, including FS/GS selector attributes. */
    for (offset = KSW_VMCB_ES; offset <= KSW_VMCB_GS; offset += 16) {
        /* A malformed or LDT-based segment cannot be guessed. */
        if (!KswSvmSegment(v, offset, FALSE)) { return STATUS_NOT_SUPPORTED; }
    }
    /* Decode both long-mode system descriptors. */
    if (!KswSvmSegment(v, KSW_VMCB_LDTR, TRUE) || !KswSvmSegment(v, KSW_VMCB_TR, TRUE)) { return STATUS_NOT_SUPPORTED; }
    /* Intercept CPUID and MSRPM-controlled MSRs, not ordinary I/O or HLT. */
    KswSvmWrite32(v, KSW_VMCB_MISC1, (1U << 18) | (1U << 28));
    /* Intercept all SVM operations plus XSETBV; nested SVM is not provided. */
    KswSvmWrite32(v, KSW_VMCB_MISC2, 0x7fU | (1U << 13));
    /* Give the CPU valid permission-map addresses even for disabled intercepts. */
    KswSvmWrite64(v, KSW_VMCB_IOPM, (ULONGLONG)MmGetPhysicalAddress(Cpu->Iopm).QuadPart);
    /* MSRPM excludes safe native MSRs from exit storms. */
    KswSvmWrite64(v, KSW_VMCB_MSRPM, (ULONGLONG)MmGetPhysicalAddress(Cpu->Msrpm).QuadPart);
    /* ASIDs are processor-local; every VMRUN conservatively flushes them. */
    KswSvmWrite32(v, KSW_VMCB_ASID, 1);
    /* TLB_CONTROL=1 requests the baseline architectural complete flush. */
    ((PUCHAR)v)[KSW_VMCB_TLB] = 1;
    /* Virtual interrupt masking keeps guest IF/TPR behavior independent of host IF. */
    KswSvmWrite32(v, KSW_VMCB_INTCTL, 1U << 24);
    /* Enable nested paging without exposing nested virtualization to Windows. */
    KswSvmWrite64(v, KSW_VMCB_NP, 1);
    /* Publish the fully constructed shared NPT. */
    KswSvmWrite64(v, KSW_VMCB_NCR3, state->Npt.RootPa);
    /* Resolve the opaque HSAVE address before entering the assembly host loop. */
    Cpu->HsavePa = (ULONGLONG)MmGetPhysicalAddress(Cpu->Hsave).QuadPart;
    /* CR state is the current guest state, not a stale prepare-time snapshot. */
    KswSvmWrite64(v, KSW_VMCB_CR0, __readcr0());
    /* Capture the current Windows address space for guest execution. */
    KswSvmWrite64(v, KSW_VMCB_CR3, __readcr3());
    /* Preserve supported CR4 features. */
    KswSvmWrite64(v, KSW_VMCB_CR4, __readcr4());
    /* Preserve pending page-fault address and debug register state. */
    KswSvmWrite64(v, KSW_VMCB_CR2, __readcr2());
    /* Debug control registers are not general-purpose registers. */
    KswSvmWrite64(v, KSW_VMCB_DR6, __readdr(6));
    /* Restore guest debug state on native stop as well as VM entry. */
    KswSvmWrite64(v, KSW_VMCB_DR7, __readdr(7));
    /* Read remaining current processor state before SVME ownership changes. */
    __try {
        /* Save exact original ownership registers for rollback. */
        Cpu->OriginalEfer = __readmsr(KSW_SVM_MSR_EFER);
        /* Do not discard pre-entry HSAVE evidence. */
        Cpu->OriginalHsave = __readmsr(KSW_SVM_MSR_HSAVE);
        /* Refuse ownership races after preparation/self-test. */
        if ((Cpu->OriginalEfer & KSW_SVM_EFER_SVME) || Cpu->OriginalHsave) { return STATUS_DEVICE_BUSY; }
        /* VMRUN guest state needs SVME set; MSR reads hide our owned bit. */
        KswSvmWrite64(v, KSW_VMCB_EFER, Cpu->OriginalEfer | KSW_SVM_EFER_SVME);
        /* Preserve current guest PAT without installing another layout. */
        KswSvmWrite64(v, KSW_VMCB_PAT, __readmsr(0x277U));
        /* Identity NPT PAT indices require the prepared host layout. */
        if (KswSvmRead64(v, KSW_VMCB_PAT) != Cpu->Caps.Pat) { return STATUS_NOT_SUPPORTED; }
        /* Long-mode FS/GS base comes from MSRs rather than legacy descriptors. */
        ((KSW_SVM_SEGMENT*)((PUCHAR)v + KSW_VMCB_FS))->base = __readmsr(0xc0000100U);
        /* Preserve the current kernel GS base. */
        ((KSW_SVM_SEGMENT*)((PUCHAR)v + KSW_VMCB_GS))->base = __readmsr(0xc0000101U);
        /* SWAPGS's alternate base is separate from the current GS base. */
        KswSvmWrite64(v, KSW_VMCB_KERNEL_GS, __readmsr(0xc0000102U));
        /* Capture all long-mode syscall MSRs used by VMLOAD/VMSAVE. */
        KswSvmWrite64(v, KSW_VMCB_STAR, __readmsr(0xc0000081U));
        /* SYSCALL target. */
        KswSvmWrite64(v, KSW_VMCB_LSTAR, __readmsr(0xc0000082U));
        /* Compatibility-mode SYSCALL target. */
        KswSvmWrite64(v, KSW_VMCB_CSTAR, __readmsr(0xc0000083U));
        /* SYSCALL flags mask. */
        KswSvmWrite64(v, KSW_VMCB_SFMASK, __readmsr(0xc0000084U));
        /* SYSENTER compatibility state. */
        KswSvmWrite64(v, KSW_VMCB_SYSENTER_CS, __readmsr(0x174U));
        /* SYSENTER stack state. */
        KswSvmWrite64(v, KSW_VMCB_SYSENTER_ESP, __readmsr(0x175U));
        /* SYSENTER instruction state. */
        KswSvmWrite64(v, KSW_VMCB_SYSENTER_EIP, __readmsr(0x176U));
        /* Preserve debug-control MSR. */
        KswSvmWrite64(v, KSW_VMCB_DEBUGCTL, __readmsr(0x1d9U));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        /* No hardware ownership was changed by the builder. */
        return GetExceptionCode();
    }
    /* Build explicit MSR ownership interception. */
    KswSvmInterceptMsr(Cpu, KSW_SVM_MSR_EFER, FALSE);
    /* Guest writes cannot redirect the host-save area. */
    KswSvmInterceptMsr(Cpu, KSW_SVM_MSR_HSAVE, FALSE);
    /* Firmware lock state is emulated read-only. */
    KswSvmInterceptMsr(Cpu, KSW_SVM_MSR_VM_CR, FALSE);
    /* PAT writes would invalidate every leaf's cache index interpretation. */
    KswSvmInterceptMsr(Cpu, 0x277U, TRUE);
    /* Active supervisor XSTATE is outside the initial XSAVE contract. */
    KswSvmInterceptMsr(Cpu, 0xda0U, TRUE);
    /* The assembly wrapper sets final RIP/RSP/RFLAGS immediately before VMRUN. */
    return STATUS_SUCCESS;
}

/* All entry callers are already pinned; no allocation occurs here. */
NTSTATUS KswordSvmEnterCurrent(KSW_SVM_CPU* Cpu)
{
    /* Validate current target state before setting SVME or HSAVE. */
    NTSTATUS status = KswordSvmBuildVmcb(Cpu);
    /* A failed builder has not entered virtualization. */
    if (!NT_SUCCESS(status)) { Cpu->Result = status; return status; }
    /* Preserve a deterministic entry failure until guest continuation proves otherwise. */
    Cpu->Result = STATUS_HV_OPERATION_FAILED;
    /* Entry rejection checks count exits within this launch, not a previous self-test. */
    Cpu->Resource->Row.vmExitCount = 0;
    /* Publish the current phase before executing any privileged transition. */
    Cpu->Stage = KSWORD_ARK_HVM_STAGE_ENTERING;
    /* A new launch starts with no native-stop acknowledgement. */
    Cpu->NativeReturnSeen = 0;
    /* Execute the independently implemented AMD continuation. */
    status = KswordSvmAsmLaunch(Cpu);
    /* An invalid VMCB also returns natively, but must never publish Active. */
    if (Cpu->Stage == KSWORD_ARK_HVM_STAGE_FAILED || Cpu->SelfTest) { status = Cpu->Result; }
    /* A self-test succeeds only after ownership registers are natively restored. */
    if (Cpu->SelfTest && NT_SUCCESS(status) &&
        (__readmsr(KSW_SVM_MSR_EFER) != Cpu->OriginalEfer || __readmsr(KSW_SVM_MSR_HSAVE) != Cpu->OriginalHsave)) {
        /* Preserve retained ownership evidence instead of publishing a false passed test. */
        Cpu->Stage = KSWORD_ARK_HVM_STAGE_FAILED;
        /* Returning a recoverable status could release pages still referenced by hardware. */
        KswordARKHvmStateSet(Cpu->Runtime, KSWORD_ARK_HVM_STATE_FAULTED | KSWORD_ARK_HVM_STATE_ROLLBACK_REQUIRED);
        /* Fail closed in the restored Windows calling context instead of fabricating self-test success. */
        KeBugCheckEx(0x20001, 0x53564dUL, (ULONG_PTR)Cpu, 4, 0);
    }
    /* Guest/native continuation, not the host dispatcher, acknowledges completion. */
    if (NT_SUCCESS(status) && !Cpu->SelfTest) {
        /* Mark the current processor active exactly once after guest continuation. */
        InterlockedExchange(&Cpu->Active, 1);
        /* Publish the corresponding runtime owner count. */
        InterlockedIncrement(&Cpu->Runtime->ResidentProcessorCount);
        /* Retain protocol-visible non-VMX success evidence. */
        Cpu->Resource->Row.stateFlags |= KSWORD_ARK_HVM_CPU_STATE_RESIDENT_ACTIVE;
        /* Record entry completion for the all-CPU commit check. */
        Cpu->Stage = KSWORD_ARK_HVM_STAGE_ENTERED;
    }
    /* Return the actual continuation status. */
    return status;
}
