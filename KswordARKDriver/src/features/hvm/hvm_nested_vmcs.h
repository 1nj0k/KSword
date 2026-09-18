/*++

Module Name:

    hvm_nested_vmcs.h

Abstract:

    Defines bounded vmcs12 storage and explicit partial vmcs02 merge state.

Environment:

    Kernel-mode Driver Framework.

--*/

#pragma once

/* Which step of the native VMCS self-test a processor stopped at. Published in
   the per-processor row so that one status code does not have to stand for four
   different failures. */
#define KSW_HVM_NATIVE_SITE_NONE 0UL
#define KSW_HVM_NATIVE_SITE_LOAD_SOURCE 1UL
#define KSW_HVM_NATIVE_SITE_STORE_FIELDS 2UL
#define KSW_HVM_NATIVE_SITE_SEED_ERROR 3UL
#define KSW_HVM_NATIVE_SITE_SWITCH_ANCHOR 4UL
/* Reading the VM-instruction error the import has to reproduce. */
#define KSW_HVM_NATIVE_SITE_SEED_READ 31UL
#define KSW_HVM_NATIVE_SITE_IMPORT 5UL
/* Conditions the import has to satisfy, separated because they fail for
   unrelated reasons and one code cannot say which one did. */
#define KSW_HVM_NATIVE_SITE_IMPORT_ANCHOR 51UL
#define KSW_HVM_NATIVE_SITE_IMPORT_EMPTY 52UL
#define KSW_HVM_NATIVE_SITE_IMPORT_LAUNCHED 53UL
#define KSW_HVM_NATIVE_SITE_IMPORT_ERRORFIELD 54UL
#define KSW_HVM_NATIVE_SITE_IMPORT_FIELDS 55UL
#define KSW_HVM_NATIVE_SITE_IMPORT_STEADY 56UL
#define KSW_HVM_NATIVE_SITE_CLEANUP 6UL


#include "hvm_internal.h"

/*
 * Address vmcs12 storage by field encoding instead of searching for it.
 *
 * The key is (width, type, index) - all three.  Width is not decoration:
 * Intel reuses the same index within the same type across widths, so
 *
 *     0x0800  ES selector          width 0 (16-bit), type 2, index 0
 *     0x6800  guest CR0            width 3 (natural), type 2, index 0
 *
 * collide on any key that leaves width out.  A key of (type, index) alone
 * makes every segment selector and its same-index control register share one
 * slot, so each write destroys the other - and the only symptom is a VM entry
 * that fails on guest state with no indication which field.  Measured, not
 * reasoned about: that is exactly what the first L2 launch reported.
 *
 * The previous sparse 64-entry cache searched linearly and had its own
 * problems: a real vmcs12 has well over a hundred fields, so L1 would fill it
 * part-way through configuring its VMCS and start receiving
 * unsupported-component errors - which L1 does not treat as fatal, so it would
 * carry on to VMLAUNCH holding a control state we silently truncated.
 *
 * Access type (bit 0) is deliberately not part of the key: it selects the high
 * half of a 64-bit field, which is the same storage.
 *
 * Index is bounded to 128 rather than the encodable 512.  The highest index
 * any defined field uses is far below that, and an encoding above it is
 * reported as an unsupported component - which is the truthful answer, since
 * this model genuinely does not address it.
 */
#define KSW_HVM_VMCS12_WIDTH_COUNT 4UL
#define KSW_HVM_VMCS12_TYPE_COUNT 4UL
#define KSW_HVM_VMCS12_INDEX_COUNT 128UL
#define KSW_HVM_VMCS12_SLOT_COUNT \
    (KSW_HVM_VMCS12_WIDTH_COUNT * KSW_HVM_VMCS12_TYPE_COUNT * \
     KSW_HVM_VMCS12_INDEX_COUNT)

/* Preserve bounded vmcs12 identity, launch state, and fields. */
typedef struct _KSW_HVM_VMCS12_STATE
{
    /* Record whether one vmcs12 pointer is current. */
    BOOLEAN Current;
    /* Record whether the current vmcs12 has launched. */
    BOOLEAN Launched;
    /* Keep the structure explicitly initialized across architectures. */
    USHORT Reserved0;
    /* Preserve the last VM-instruction error visible to L1. */
    ULONG InstructionError;
    /*
     * How many field writes this vmcs12 has taken.
     *
     * Not a statistic: it is what lets the backing store in the VMCS region be
     * skipped when nothing has changed since it was last written.  Measured
     * need - a guest hypervisor's steady state is VMPTRLD, invalidate, VMCLEAR
     * with no field writes at all in between, and spilling anyway cost about
     * ninety thousand cycles per cycle of its loop, a third of all the time
     * this driver spent.  Carried inside the vmcs12 rather than beside it so a
     * copy that is pooled, restored, or migrated to another processor brings
     * its own history with it.
     */
    ULONG WriteSerial;
    /* Preserve the current vmcs12 physical address. */
    ULONGLONG PhysicalAddress;
    /* Preserve every field L1 wrote, indexed by (type, index). */
    ULONGLONG Fields[KSW_HVM_VMCS12_SLOT_COUNT];
} KSW_HVM_VMCS12_STATE;

/* Preserve explicit vmcs02 merge maturity without claiming L2 active. */
typedef struct _KSW_HVM_VMCS02_STATE
{
    /* Record whether control merge validation completed. */
    BOOLEAN ControlsValidated;
    /* Record whether guest-state merge validation completed. */
    BOOLEAN GuestStateValidated;
    /* Record whether exit-reflection metadata is complete. */
    BOOLEAN ExitReflectionReady;
    /* Record whether a hardware vmcs02 is active. */
    BOOLEAN Active;
    /* Preserve the last merge status. */
    NTSTATUS LastStatus;
    /* Preserve the last Intel VM-instruction error. */
    ULONG InstructionError;
    /* Preserve the merged EPT pointer when available. */
    ULONGLONG EptPointer;
} KSW_HVM_VMCS02_STATE;

EXTERN_C_START

/* Read a hardware VMCS only at a boundary where its launch state is clear.
 * The source is left inactive and the caller's current VMCS is restored.
 * A NULL field bitmap discovers supported fields on a private test VMCS.
 * Restore failure is fatal: returning with a foreign current VMCS is unsafe.
 */
NTSTATUS KswordARKHvmNestedVmcsImportCleared(
    ULONGLONG PhysicalAddress,
    KSW_HVM_VMCS12_STATE* Destination,
    const ULONG* SupportedFields,
    ULONG* DiscoveredFields,
    ULONG* FieldCount);

/* Real VMREAD/VMWRITE persistence and import test; caller owns VMX operation. */
NTSTATUS KswordARKHvmNestedVmcsNativeSelfTest(
    KSW_HVM_CPU_RESOURCE* Cpu,
    KSW_HVM_VMCS12_STATE* Scratch);

/* Initialize bounded vmcs12 and vmcs02 state. */
VOID
KswordARKHvmNestedVmcsInitialize(
    _Out_ KSW_HVM_VMCS12_STATE* Vmcs12,
    _Out_ KSW_HVM_VMCS02_STATE* Vmcs02
    );

/* Cache one vmcs12 field without dynamic allocation. */
NTSTATUS
KswordARKHvmNestedVmcs12Write(
    _Inout_ KSW_HVM_VMCS12_STATE* Vmcs12,
    _In_ ULONG Encoding,
    _In_ ULONGLONG Value
    );

/* Read one cached vmcs12 field. */
NTSTATUS
KswordARKHvmNestedVmcs12Read(
    _In_ const KSW_HVM_VMCS12_STATE* Vmcs12,
    _In_ ULONG Encoding,
    _Out_ ULONGLONG* Value
    );

/* Validate merge prerequisites while retaining explicit partial state. */
NTSTATUS
KswordARKHvmNestedVmcs02Prepare(
    _In_ const KSW_HVM_VMCS12_STATE* Vmcs12,
    _In_ ULONGLONG ComposedEptPointer,
    _Out_ KSW_HVM_VMCS02_STATE* Vmcs02
    );

EXTERN_C_END
