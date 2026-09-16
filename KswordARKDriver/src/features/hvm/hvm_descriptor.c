/* Preserve descriptor-table registers across native and nested continuations. */
#include "hvm_descriptor.h"
#include "hvm_event.h"

/* Match the packed ten-byte operands consumed by the assembly helper. */
C_ASSERT(FIELD_OFFSET(KSW_HVM_SEGMENT_SNAPSHOT, Gdtr) == 0);
C_ASSERT(FIELD_OFFSET(KSW_HVM_SEGMENT_SNAPSHOT, Idtr) == 10);

BOOLEAN
KswordARKHvmCaptureGuestDescriptorTables(
    _Out_ KSW_HVM_SEGMENT_SNAPSHOT* Snapshot
    )
{
    /* Keep partial reads private until every VMREAD has succeeded. */
    SIZE_T gdtBase = 0U, gdtLimit = 0U, idtBase = 0U, idtLimit = 0U;

    /* Refuse a missing destination without changing the VMCS. */
    if (Snapshot == NULL) {
        /* Report that no restorable state was captured. */
        return FALSE;
    }
    /* Capture guest state, not the host tables currently loaded by VM-exit. */
    if (KswordARKHvmVmcsFieldLoad(0x6816UL, &gdtBase) != 0U ||
        KswordARKHvmVmcsFieldLoad(0x4810UL, &gdtLimit) != 0U ||
        KswordARKHvmVmcsFieldLoad(0x6818UL, &idtBase) != 0U ||
        KswordARKHvmVmcsFieldLoad(0x4812UL, &idtLimit) != 0U ||
        gdtLimit > MAXUSHORT || idtLimit > MAXUSHORT) {
        /* Fail before VMXOFF rather than install an incomplete snapshot. */
        return FALSE;
    }
    /* Selector slots are unused by this table-only snapshot. */
    RtlZeroMemory(Snapshot, sizeof(*Snapshot));
    /* Publish the exact guest GDT base. */
    Snapshot->Gdtr.Base = (ULONGLONG)gdtBase;
    /* Publish the validated sixteen-bit GDT limit. */
    Snapshot->Gdtr.Limit = (USHORT)gdtLimit;
    /* Publish the exact guest IDT base. */
    Snapshot->Idtr.Base = (ULONGLONG)idtBase;
    /* Publish the validated sixteen-bit IDT limit. */
    Snapshot->Idtr.Limit = (USHORT)idtLimit;
    /* Report a complete snapshot that can survive VMCLEAR. */
    return TRUE;
}

#if defined(_M_AMD64)
/* Preserve the three observations in the existing lifecycle evidence format. */
static BOOLEAN
KswordARKHvmRecordDescriptorRestore(
    _In_ const KSW_HVM_DESCRIPTOR_TABLE* Expected,
    _In_ const KSW_HVM_DESCRIPTOR_TABLE* Before,
    _In_ const KSW_HVM_DESCRIPTOR_TABLE* After,
    _In_ ULONG Tag,
    _In_ ULONG Stage
    )
{
    /* Compare both architectural components, not just the base. */
    const BOOLEAN matched = Expected->Base == After->Base &&
        Expected->Limit == After->Limit;
    /* Initialize the fixed-size lifecycle diagnostic row. */
    KSWORD_ARK_HVM_EVENT_ROW row = { 0 };
    /* Attribute each readback to the exact Windows processor identity. */
    PROCESSOR_NUMBER processor = { 0 };

    /* Distinguish these diagnostics from guest memory accesses. */
    row.type = KSWORD_ARK_HVM_EVENT_TYPE_LIFECYCLE;
    /* The ring supplies timestamps but does not infer processor identity. */
    (void)KeGetCurrentProcessorNumberEx(&processor);
    /* Preserve the processor group instead of assuming group zero. */
    row.processorGroup = processor.Group;
    /* Preserve the processor number within that group. */
    row.processorNumber = processor.Number;
    /* Use the four-byte GDTR or IDTR tag. */
    row.ruleId = Tag;
    /* Identify resident stop, one-shot return, or nested-probe return. */
    row.exitReason = Stage;
    /* Retain the expected base in the diagnostic payload. */
    row.guestPhysicalAddress = Expected->Base;
    /* Retain the hardware base read after restoration. */
    row.guestLinearAddress = After->Base;
    /* Retain the preceding base to expose private-host-IDT leakage. */
    row.guestRip = Before->Base;
    /* Pack expected and restored limits into separate sixteen-bit lanes. */
    row.qualification = (ULONGLONG)Expected->Limit |
        ((ULONGLONG)After->Limit << 16);
    /* Retain the preceding limit, normally 0xFFFF after VM-exit. */
    row.access = (ULONG)Before->Limit;
    /* Publish failure unless the complete hardware readback matches. */
    row.status = matched ? STATUS_SUCCESS : STATUS_HV_OPERATION_FAILED;
    /* The allocation-free ring supplies the sequence and QPC timestamp. */
    KswordARKHvmEventPublish(&row);
    /* Refuse an incomplete restoration at the continuation boundary. */
    return matched;
}
#endif

BOOLEAN
KswordARKHvmRestoreDescriptorTables(
    _In_ const KSW_HVM_SEGMENT_SNAPSHOT* Snapshot,
    _In_ ULONG Stage
    )
{
#if defined(_M_AMD64)
    /* Keep both observations on the current processor's stack. */
    KSW_HVM_SEGMENT_SNAPSHOT before = { 0 }, after = { 0 };
    /* Verify both tables without short-circuiting the second evidence row. */
    BOOLEAN gdtMatched = FALSE, idtMatched = FALSE;

    /* Refuse a missing snapshot before privileged instructions run. */
    if (Snapshot == NULL) {
        /* Report an unusable continuation. */
        return FALSE;
    }
    /* Observe the VM-exit host tables before replacing them. */
    KswordARKHvmCaptureSegments(&before);
    /* VMXOFF does not undo host table loads or their 0xFFFF limits. */
    KswordARKHvmAsmRestoreDescriptorTables(Snapshot);
    /* Verify actual hardware state before the caller enables interrupts. */
    KswordARKHvmCaptureSegments(&after);
    /* Retain GDT verification independently of the IDT result. */
    gdtMatched = KswordARKHvmRecordDescriptorRestore(&Snapshot->Gdtr,
        &before.Gdtr, &after.Gdtr, 0x47445452UL, Stage);
    /* Retain IDT verification independently of the GDT result. */
    idtMatched = KswordARKHvmRecordDescriptorRestore(&Snapshot->Idtr,
        &before.Idtr, &after.Idtr, 0x49445452UL, Stage);
    /* Only a complete restoration permits the continuation. */
    return gdtMatched && idtMatched;
#else
    /* This VMX-specific continuation has no non-amd64 implementation. */
    UNREFERENCED_PARAMETER(Snapshot);
    /* Consume the diagnostic stage without claiming success. */
    UNREFERENCED_PARAMETER(Stage);
    /* Refuse unverifiable architectural state. */
    return FALSE;
#endif
}
