#pragma once

#include "KswordArkHvmIoctl.h"

/* Independent versioning keeps existing HVM query clients ABI-compatible. */
#define KSWORD_ARK_HVM_METRICS_VERSION 2UL
#define KSWORD_ARK_IOCTL_FUNCTION_HVM_METRICS 0x916UL
#define IOCTL_KSWORD_ARK_HVM_METRICS \
    CTL_CODE(KSWORD_ARK_IOCTL_DEVICE_TYPE, KSWORD_ARK_IOCTL_FUNCTION_HVM_METRICS, METHOD_BUFFERED, FILE_READ_ACCESS)

/* CPU stamps are QPC readings, not TSC cycles or cross-machine UTC values. */
#define KSW_HVM_TIME_IPI_ENTER 0UL
#define KSW_HVM_TIME_IPI_LEAVE 1UL
#define KSW_HVM_TIME_VMCS_BEGIN 2UL
#define KSW_HVM_TIME_STATE_CAPTURED 3UL
#define KSW_HVM_TIME_VMCS_WRITTEN 4UL
#define KSW_HVM_TIME_ENTRY_BEFORE 5UL
#define KSW_HVM_TIME_ENTRY_AFTER 6UL
#define KSW_HVM_TIME_CPU_STAGES 7UL

/* Global intervals distinguish preparation from the disruptive rendezvous. */
#define KSW_HVM_TIME_RESOURCES_BEGIN 0UL
#define KSW_HVM_TIME_RESOURCES_END 1UL
#define KSW_HVM_TIME_EPT_BEGIN 2UL
#define KSW_HVM_TIME_EPT_END 3UL
#define KSW_HVM_TIME_RENDEZVOUS_BEGIN 4UL
#define KSW_HVM_TIME_RENDEZVOUS_END 5UL
#define KSW_HVM_TIME_GLOBAL_STAGES 6UL

typedef struct _KSWORD_ARK_HVM_METRICS_CPU {
    unsigned short group;
    unsigned char number;
    unsigned char reserved;
    unsigned long validMask;
    unsigned long long qpc[KSW_HVM_TIME_CPU_STAGES];
} KSWORD_ARK_HVM_METRICS_CPU;

/* Per-CPU observational counters reset when resident resources are recreated. */
typedef struct _KSWORD_ARK_HVM_SHADOW_METRICS {
    unsigned long index, pagesUsed, trackedPages, trackedOverflow;
    unsigned long fills, denied, exhausted, kept, dropped;
    unsigned long adPending, adPropagated, adOverflow, verifyMismatch;
} KSWORD_ARK_HVM_SHADOW_METRICS;

typedef struct _KSWORD_ARK_HVM_METRICS_REQUEST {
    unsigned long version, size, flags, reserved;
} KSWORD_ARK_HVM_METRICS_REQUEST;

typedef struct _KSWORD_ARK_HVM_METRICS_RESPONSE {
    unsigned long version, size;
    /* A zero value forbids treating the transition snapshot as complete. */
    unsigned long transitionCoherent, transitionSequence;
    unsigned long command, lastStatus, processorCount, globalValidMask;
    unsigned long long qpcFrequency, snapshotBeginQpc, snapshotEndQpc;
    unsigned long long commandBeginQpc, commandEndQpc;
    unsigned long long globalQpc[KSW_HVM_TIME_GLOBAL_STAGES];
    /* Attempt/result counters include every call through the INVEPT wrapper. */
    unsigned long long inveptAttempts, inveptSucceeded, inveptFailed;
    /* These ledgers cover nested-page rule objects and replacement pages only. */
    unsigned long long ruleAllocations, ruleFrees, replacementAllocations, replacementFrees;
    /* Endpoints bracket concurrent counters; they are not one atomic snapshot. */
    KSWORD_ARK_HVM_METRICS_CPU processors[KSWORD_ARK_HVM_MAX_PROCESSORS];
    /* Version 2 samples current shadow caches separately from transition stamps. */
    unsigned long shadowProcessorCount, reserved;
    KSWORD_ARK_HVM_SHADOW_METRICS shadowProcessors[KSWORD_ARK_HVM_MAX_PROCESSORS];
} KSWORD_ARK_HVM_METRICS_RESPONSE;
