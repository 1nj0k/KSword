/*++

Module Name:

    hvm_nested_l2.h

Abstract:

    Defines L2 entry from vmcs12 and the routing of L2 exits between us and L1.

Environment:

    Kernel-mode Driver Framework.

--*/

#pragma once

#include "hvm_internal.h"
#include "hvm_nested.h"

/* Forward-declare the per-processor resident context. */
struct _KSW_HVM_RESIDENT_VCPU;
/* Forward-declare the VM-exit register frame. */
struct _KSW_HVM_GPR_FRAME;

EXTERN_C_START

/*
 * Enter L2 from the current vmcs12.
 *
 * On success this does not return: the processor is running L2, and the next
 * thing that executes on this processor is the VM-exit stub.  On failure it
 * returns the Intel VM-instruction error L1 should observe, with vmcs01 loaded
 * again and nothing else disturbed.
 *
 * Returns zero only when a failure could not even be expressed, which the
 * caller reports as VMfailInvalid.
 */
ULONG
KswordARKHvmNestedL2Enter(
    _Inout_ struct _KSW_HVM_RESIDENT_VCPU* Context,
    _In_ BOOLEAN IsResume
    );

/*
 * Name what routing did with one exit.
 *
 * Three outcomes, not two.  Collapsing "we already dealt with it" into "not
 * ours" sends a resolved exit back through the ordinary handling, where an EPT
 * violation the shadow hierarchy just satisfied gets evaluated a second time
 * against our own hierarchy - which does not describe L2's addresses at all,
 * and whose refusal path tears residency down while L2 is running.
 */
#define KSW_HVM_L2_ROUTE_NOT_L2 0UL
#define KSW_HVM_L2_ROUTE_REFLECTED 1UL
#define KSW_HVM_L2_ROUTE_HANDLED 2UL

/*
 * Route one L2 exit.
 *
 * REFLECTED: delivered to L1 - vmcs01 is loaded, L1's guest state is set to
 * its own VM-exit handler, and the caller must resume.
 * HANDLED: satisfied here on vmcs02 - the caller must resume without further
 * handling.
 * NOT_L2: this processor is not running L2, so the caller owns the exit.
 */
ULONG
KswordARKHvmNestedL2Reflect(
    _Inout_ struct _KSW_HVM_RESIDENT_VCPU* Context,
    _In_ ULONG ExitReason
    );

EXTERN_C_END
