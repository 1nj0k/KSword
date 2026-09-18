/*++

Module Name:

    hvm_nested_vmcs.c

Abstract:

    Implements bounded vmcs12 field storage and fail-closed vmcs02 validation.

Environment:

    Kernel-mode Driver Framework.

--*/

#include "hvm_nested_vmcs.h"
#include "hvm_vmcs.h"
#if defined(_M_AMD64)
#include <intrin.h>
#endif

/* Name Intel VM-entry invalid-control-fields error seven. */
#define KSW_HVM_VMX_ERROR_INVALID_CONTROL_FIELDS 7UL

/* Native VMCS layout is implementation-specific. Never decode its page bytes. */
NTSTATUS KswordARKHvmNestedVmcsImportCleared(
    ULONGLONG PhysicalAddress,
    KSW_HVM_VMCS12_STATE* Destination,
    const ULONG* SupportedFields,
    ULONG* DiscoveredFields,
    ULONG* FieldCount)
{
#if defined(_M_AMD64)
    unsigned __int64 original = ~0ULL;
    unsigned __int64 source = PhysicalAddress;
    SIZE_T value = 0;
    SIZE_T originalError = 0;
    ULONG slot;
    NTSTATUS status = STATUS_HV_OPERATION_FAILED;
    BOOLEAN loaded = FALSE;
    UCHAR cleared;
    UCHAR restored = 0;

    if (Destination == NULL || FieldCount == NULL || PhysicalAddress == 0 ||
        (PhysicalAddress & 0xFFFULL) != 0 ||
        (SupportedFields == NULL && DiscoveredFields == NULL)) {
        return STATUS_INVALID_PARAMETER;
    }
    *FieldCount = 0;
    __vmx_vmptrst(&original);
    if (original == source) {
        return STATUS_INVALID_PARAMETER;
    }
    /* The caller admits only inactive VMCSs. VMCLEAR also validates the PA. */
    if (__vmx_vmclear(&source) != 0) {
        return STATUS_HV_OPERATION_FAILED;
    }
    if (__vmx_vmptrld(&source) == 0) {
        loaded = TRUE;
        KswordARKHvmNestedVmcsInitialize(Destination, NULL);
        Destination->Current = TRUE;
        Destination->PhysicalAddress = PhysicalAddress;
        if (DiscoveredFields != NULL) {
            RtlZeroMemory(DiscoveredFields, 64 * sizeof(ULONG));
        }
        /* Unsupported-field discovery itself changes VM_INSTRUCTION_ERROR. */
        if (KswordARKHvmVmcsFieldLoad(0x4400, &originalError) == 0) {
            status = STATUS_SUCCESS;
            for (slot = 0; slot < KSW_HVM_VMCS12_SLOT_COUNT; ++slot) {
                const ULONG encoding = ((slot / 512) << 13) |
                    (((slot % 512) / 128) << 10) | ((slot % 128) << 1);
                if (SupportedFields != NULL &&
                    (SupportedFields[slot / 32] & (1UL << (slot % 32))) == 0) {
                    continue;
                }
                if (encoding == 0x4400) {
                    value = originalError;
                } else if (KswordARKHvmVmcsFieldLoad(encoding, &value) != 0) {
                    if (SupportedFields != NULL) {
                        status = STATUS_HV_OPERATION_FAILED;
                        break;
                    }
                    continue;
                }
                Destination->Fields[slot] = (ULONGLONG)value;
                if (DiscoveredFields != NULL) {
                    DiscoveredFields[slot / 32] |= 1UL << (slot % 32);
                }
                ++*FieldCount;
            }
            Destination->InstructionError = (ULONG)originalError;
            Destination->WriteSerial = 1;
            /* No guessed launch state: admission requires a clear boundary. */
            Destination->Launched = FALSE;
        }
    }
    cleared = loaded ? __vmx_vmclear(&source) : 0;
    if (original != ~0ULL) {
        restored = __vmx_vmptrld(&original);
    }
    if (cleared != 0 || restored != 0) {
        /* A foreign/current or still-active VMCS cannot escape this helper. */
        KeBugCheckEx(0x20001, 0x4E564D43, PhysicalAddress, cleared, restored);
    }
    return status;
#else
    UNREFERENCED_PARAMETER(PhysicalAddress);
    UNREFERENCED_PARAMETER(Destination);
    UNREFERENCED_PARAMETER(SupportedFields);
    UNREFERENCED_PARAMETER(DiscoveredFields);
    UNREFERENCED_PARAMETER(FieldCount);
    return STATUS_NOT_SUPPORTED;
#endif
}

NTSTATUS KswordARKHvmNestedVmcsNativeSelfTest(
    KSW_HVM_CPU_RESOURCE* Cpu,
    KSW_HVM_VMCS12_STATE* Scratch)
{
#if defined(_M_AMD64)
    static const ULONG fields[] = {0x0802, 0x4004, 0x2010, 0x681E};
    static const ULONGLONG values[] = {
        0x28ULL, 0xA5010080ULL, 0x1122334455667788ULL, 0xFFFF800012345678ULL};
    unsigned __int64 source = (ULONGLONG)Cpu->Vmcs02Physical.QuadPart;
    unsigned __int64 anchor = (ULONGLONG)Cpu->VmcsPhysical.QuadPart;
    unsigned __int64 current = ~0ULL;
    ULONG count = 0, index;
    ULONGLONG readback = 0;
    NTSTATUS status = STATUS_HV_OPERATION_FAILED;
    Cpu->NativeVmcsFieldCount = 0;
    RtlZeroMemory(Cpu->NativeVmcsFields, sizeof(Cpu->NativeVmcsFields));
    /*
     * Say which step failed, not merely that one did.
     *
     * Four of the exits below return the same status, so a failure reported
     * only as STATUS_HV_OPERATION_FAILED on four processors leaves no way to
     * tell "vmcs02 would not load" from "this processor answered a capability
     * question differently than the test assumed". The site is published in the
     * per-processor row, which already carries VMX instruction results, and is
     * cleared on success.
     */
    Cpu->Row.vmxInstructionResult = KSW_HVM_NATIVE_SITE_LOAD_SOURCE;
    if (__vmx_vmclear(&source) != 0 || __vmx_vmptrld(&source) != 0) {
        goto Cleanup;
    }
    Cpu->Row.vmxInstructionResult = KSW_HVM_NATIVE_SITE_STORE_FIELDS;
    for (index = 0; index < RTL_NUMBER_OF(fields); ++index) {
        if (KswordARKHvmVmcsFieldStore(fields[index], (SIZE_T)values[index]) != 0) {
            goto Cleanup;
        }
    }
    Cpu->Row.vmxInstructionResult = KSW_HVM_NATIVE_SITE_SWITCH_ANCHOR;
    if (__vmx_vmclear(&source) != 0 || __vmx_vmclear(&anchor) != 0 ||
        __vmx_vmptrld(&anchor) != 0) {
        goto Cleanup;
    }
    /* Each condition gets its own site: they fail for unrelated reasons. */
    Cpu->Row.vmxInstructionResult = KSW_HVM_NATIVE_SITE_IMPORT;
    status = KswordARKHvmNestedVmcsImportCleared(
        source, Scratch, NULL, Cpu->NativeVmcsFields, &count);
    __vmx_vmptrst(&current);
    if (!NT_SUCCESS(status)) { goto Cleanup; }
    status = STATUS_DATA_ERROR;
    if (current != anchor) {
        Cpu->Row.vmxInstructionResult = KSW_HVM_NATIVE_SITE_IMPORT_ANCHOR;
        goto Cleanup;
    }
    if (count == 0) {
        Cpu->Row.vmxInstructionResult = KSW_HVM_NATIVE_SITE_IMPORT_EMPTY;
        goto Cleanup;
    }
    if (Scratch->Launched) {
        Cpu->Row.vmxInstructionResult = KSW_HVM_NATIVE_SITE_IMPORT_LAUNCHED;
        goto Cleanup;
    }
    /*
     * The error slot and the summary must agree.
     *
     * This used to compare against a value read from vmcs02 before the import
     * cleared it, which is not a property anything guarantees: VM-instruction
     * error is live state the processor rewrites, VMCLEAR resets it, and the
     * import deliberately reloads the VMCS. Comparing across that boundary
     * failed on all four processors and was testing the wrong thing.
     *
     * What the importer actually promises is internal: enumerating the fields
     * issues VMREADs that themselves set VM-instruction error, so it snapshots
     * the value on entry and substitutes that snapshot when it reaches the error
     * field rather than reading it again. If that substitution were dropped the
     * slot would hold whatever the last probe produced, so requiring the slot and
     * the summary to match is exactly the check that catches it.
     */
    if (!NT_SUCCESS(KswordARKHvmNestedVmcs12Read(Scratch, 0x4400, &readback)) ||
        readback != (ULONGLONG)Scratch->InstructionError) {
        Cpu->Row.vmxInstructionResult = KSW_HVM_NATIVE_SITE_IMPORT_ERRORFIELD;
        goto Cleanup;
    }
    Cpu->Row.vmxInstructionResult = KSW_HVM_NATIVE_SITE_IMPORT_FIELDS;
    for (index = 0; index < RTL_NUMBER_OF(fields); ++index) {
        if (!NT_SUCCESS(KswordARKHvmNestedVmcs12Read(Scratch, fields[index], &readback)) ||
            readback != values[index]) {
            goto Cleanup;
        }
    }
    Cpu->Row.vmxInstructionResult = KSW_HVM_NATIVE_SITE_IMPORT_STEADY;
    /* The steady-state importer reads only fields this CPU actually supports. */
    status = KswordARKHvmNestedVmcsImportCleared(
        source, Scratch, Cpu->NativeVmcsFields, NULL, &count);
    __vmx_vmptrst(&current);
    if (NT_SUCCESS(status) && current == anchor) {
        Cpu->NativeVmcsFieldCount = count;
    } else {
        status = STATUS_DATA_ERROR;
    }
Cleanup:
    if (__vmx_vmclear(&source) != 0 || __vmx_vmclear(&anchor) != 0) {
        Cpu->Row.vmxInstructionResult = KSW_HVM_NATIVE_SITE_CLEANUP;
        status = STATUS_HV_OPERATION_FAILED;
    }
    if (!NT_SUCCESS(status)) {
        Cpu->NativeVmcsFieldCount = 0;
    } else {
        /* Nothing to attribute once the whole sequence has passed. */
        Cpu->Row.vmxInstructionResult = KSW_HVM_NATIVE_SITE_NONE;
    }
    return status;
#else
    UNREFERENCED_PARAMETER(Cpu);
    UNREFERENCED_PARAMETER(Scratch);
    return STATUS_NOT_SUPPORTED;
#endif
}

VOID
KswordARKHvmNestedVmcsInitialize(
    _Out_ KSW_HVM_VMCS12_STATE* Vmcs12,
    _Out_ KSW_HVM_VMCS02_STATE* Vmcs02
    )
{
    /* Initialize a supplied vmcs12 state. */
    if (Vmcs12 != NULL) {
        /* Clear every bounded vmcs12 field and identity. */
        RtlZeroMemory(Vmcs12, sizeof(*Vmcs12));
    }
    /* Initialize a supplied vmcs02 merge state. */
    if (Vmcs02 != NULL) {
        /* Clear every merge and active-state marker. */
        RtlZeroMemory(Vmcs02, sizeof(*Vmcs02));
        /* Publish explicit partial implementation status. */
        Vmcs02->LastStatus = STATUS_NOT_IMPLEMENTED;
    }
}

/*
 * Decompose one VMCS field encoding.
 *
 * Returns FALSE for an encoding this model does not address, which is what
 * makes the architectural "unsupported component" answer honest rather than a
 * stand-in for running out of room.
 */
static BOOLEAN
KswordARKHvmNestedVmcs12Decompose(
    _In_ ULONG Encoding,
    _Out_ ULONG* Slot,
    _Out_ ULONG* Width,
    _Out_ BOOLEAN* HighHalf
    )
{
    const ULONG index = (Encoding >> 1) & 0x1FFUL;
    const ULONG type = (Encoding >> 10) & 0x3UL;
    const ULONG width = (Encoding >> 13) & 0x3UL;
    const BOOLEAN high = ((Encoding & 0x1UL) != 0UL);

    *Slot = 0UL;
    *Width = 0UL;
    *HighHalf = FALSE;
    /* Reject bits above the encoding Intel defines. */
    if ((Encoding & ~0x00007FFFUL) != 0UL) {
        /* Report an encoding outside the model. */
        return FALSE;
    }
    /* Reject an index this bounded model does not address. */
    if (index >= KSW_HVM_VMCS12_INDEX_COUNT) {
        /* Report an encoding outside the model. */
        return FALSE;
    }
    /*
     * The high half exists only for 64-bit fields.
     *
     * Width one is the 64-bit class; every other class is a single storage
     * unit, so an access-type bit set on one is a malformed encoding rather
     * than a request for its upper half.
     */
    if (high && width != 1UL) {
        /* Report an encoding outside the model. */
        return FALSE;
    }
    /* Width is part of the identity, not a hint - see the header. */
    *Slot =
        (width * KSW_HVM_VMCS12_TYPE_COUNT * KSW_HVM_VMCS12_INDEX_COUNT) +
        (type * KSW_HVM_VMCS12_INDEX_COUNT) +
        index;
    *Width = width;
    *HighHalf = high;
    /* Report a complete decomposition. */
    return TRUE;
}

/* Narrow one stored value to the width its encoding declares. */
static ULONGLONG
KswordARKHvmNestedVmcs12Narrow(
    _In_ ULONGLONG Value,
    _In_ ULONG Width
    )
{
    /* Select the sixteen-bit field class. */
    if (Width == 0UL) {
        /* Return only the bits a sixteen-bit field holds. */
        return Value & 0xFFFFULL;
    }
    /* Select the thirty-two-bit field class. */
    if (Width == 2UL) {
        /* Return only the bits a thirty-two-bit field holds. */
        return Value & 0xFFFFFFFFULL;
    }
    /* Return the full value for the 64-bit and natural-width classes. */
    return Value;
}

NTSTATUS
KswordARKHvmNestedVmcs12Write(
    _Inout_ KSW_HVM_VMCS12_STATE* Vmcs12,
    _In_ ULONG Encoding,
    _In_ ULONGLONG Value
    )
{
    ULONG slot = 0UL;
    ULONG width = 0UL;
    BOOLEAN high = FALSE;

    /* Require one current vmcs12 before touching its storage. */
    if (Vmcs12 == NULL || !Vmcs12->Current) {
        /* Return the exact nested-state contract failure. */
        return STATUS_INVALID_DEVICE_STATE;
    }
    /* Reject an encoding this model does not address. */
    if (!KswordARKHvmNestedVmcs12Decompose(
            Encoding,
            &slot,
            &width,
            &high)) {
        /* Return the exact unsupported-component failure. */
        return STATUS_NOT_FOUND;
    }
    if (high) {
        /* Replace only the upper half the access type names. */
        Vmcs12->Fields[slot] =
            (Vmcs12->Fields[slot] & 0xFFFFFFFFULL) |
            ((Value & 0xFFFFFFFFULL) << 32);
    } else {
        Vmcs12->Fields[slot] =
            KswordARKHvmNestedVmcs12Narrow(Value, width);
    }
    /* Mark the copy changed, so the backing store knows it has work to do. */
    Vmcs12->WriteSerial += 1UL;
    /* Complete the field write successfully. */
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKHvmNestedVmcs12Read(
    _In_ const KSW_HVM_VMCS12_STATE* Vmcs12,
    _In_ ULONG Encoding,
    _Out_ ULONGLONG* Value
    )
{
    ULONG slot = 0UL;
    ULONG width = 0UL;
    BOOLEAN high = FALSE;

    /* Require one current vmcs12 and a fixed output. */
    if (Vmcs12 == NULL || Value == NULL || !Vmcs12->Current) {
        /* Return the exact nested-state contract failure. */
        return STATUS_INVALID_DEVICE_STATE;
    }
    *Value = 0ULL;
    /* Reject an encoding this model does not address. */
    if (!KswordARKHvmNestedVmcs12Decompose(
            Encoding,
            &slot,
            &width,
            &high)) {
        /* Return the exact unsupported-component failure. */
        return STATUS_NOT_FOUND;
    }
    /*
     * A field never written reads as zero rather than failing.
     *
     * That is the architectural shape: whether a component is supported is a
     * property of its encoding, not of whether anyone has written it yet.
     * Failing on an unwritten field would make VMREAD-before-VMWRITE - which
     * L1 is entitled to do - look like an unsupported component.
     */
    *Value = high
        ? ((Vmcs12->Fields[slot] >> 32) & 0xFFFFFFFFULL)
        : KswordARKHvmNestedVmcs12Narrow(Vmcs12->Fields[slot], width);
    /* Complete the field read successfully. */
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKHvmNestedVmcs02Prepare(
    _In_ const KSW_HVM_VMCS12_STATE* Vmcs12,
    _In_ ULONGLONG ComposedEptPointer,
    _Out_ KSW_HVM_VMCS02_STATE* Vmcs02
    )
{
    /* Validate fixed merge-state pointers. */
    if (Vmcs12 == NULL ||
        Vmcs02 == NULL) {
        /* Return the exact caller-contract failure. */
        return STATUS_INVALID_PARAMETER;
    }
    /* Clear stale merge state before evaluating prerequisites. */
    RtlZeroMemory(Vmcs02, sizeof(*Vmcs02));
    /* Require one current vmcs12 before merge validation. */
    if (!Vmcs12->Current) {
        /* Publish the exact invalid nested state. */
        Vmcs02->LastStatus =
            STATUS_INVALID_DEVICE_STATE;
        /* Publish Intel invalid-control-fields evidence. */
        Vmcs02->InstructionError =
            KSW_HVM_VMX_ERROR_INVALID_CONTROL_FIELDS;
        /* Return the exact invalid nested state. */
        return Vmcs02->LastStatus;
    }
    /* Require a fully composed L1-on-L0 EPT pointer before L2 entry. */
    if (ComposedEptPointer == 0ULL) {
        /* Publish explicit partial shadow-EPT status. */
        Vmcs02->LastStatus = STATUS_NOT_IMPLEMENTED;
        /* Publish Intel invalid-control-fields evidence. */
        Vmcs02->InstructionError =
            KSW_HVM_VMX_ERROR_INVALID_CONTROL_FIELDS;
        /* Return without claiming vmcs02 readiness. */
        return Vmcs02->LastStatus;
    }
    /*
     * Control/guest merge and exit reflection remain deliberately incomplete.
     * Preserve the composed EPT identity but do not publish an active vmcs02.
     */
    Vmcs02->EptPointer = ComposedEptPointer;
    /* Publish explicit partial merge status. */
    Vmcs02->LastStatus = STATUS_NOT_IMPLEMENTED;
    /* Publish Intel invalid-control-fields evidence. */
    Vmcs02->InstructionError =
        KSW_HVM_VMX_ERROR_INVALID_CONTROL_FIELDS;
    /* Return without claiming L2 active state. */
    return Vmcs02->LastStatus;
}
