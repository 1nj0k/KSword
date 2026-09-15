#pragma once

#include "hvm_vmcs.h"

/* Identify the continuation whose descriptor tables are being restored. */
#define KSW_HVM_DESCRIPTOR_RESIDENT 1UL
#define KSW_HVM_DESCRIPTOR_ONESHOT 2UL
#define KSW_HVM_DESCRIPTOR_PROBE 3UL

EXTERN_C_START

/* Capture the current guest's tables before VMCLEAR destroys their source. */
BOOLEAN KswordARKHvmCaptureGuestDescriptorTables(
    _Out_ KSW_HVM_SEGMENT_SNAPSHOT* Snapshot);

/* Restore bases and limits, then verify SGDT/SIDT and publish both readbacks. */
BOOLEAN KswordARKHvmRestoreDescriptorTables(
    _In_ const KSW_HVM_SEGMENT_SNAPSHOT* Snapshot,
    _In_ ULONG Stage);

/* Load GDTR and IDTR without modifying selectors or descriptor memory. */
VOID KswordARKHvmAsmRestoreDescriptorTables(
    _In_ const KSW_HVM_SEGMENT_SNAPSHOT* Snapshot);

EXTERN_C_END
