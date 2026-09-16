#pragma once

#include "ark/ark_driver.h"
#include "driver/KswordArkHvmMetricsIoctl.h"

/* Lifecycle serialization belongs to the existing HVM control lock. */
VOID KswordARKHvmMetricsBegin(ULONG Command);
VOID KswordARKHvmMetricsEnd(NTSTATUS Status);
VOID KswordARKHvmMetricsStamp(ULONG Stage);
VOID KswordARKHvmMetricsCpuStamp(ULONG Index, ULONG Stage);
VOID KswordARKHvmMetricsAllocation(BOOLEAN Replacement, BOOLEAN Free);
NTSTATUS KswordARKHvmMetricsQuery(KSWORD_ARK_HVM_METRICS_RESPONSE* Response);

/* Caller holds the runtime resource lock; values remain observational. */
VOID KswordARKHvmResidentMetrics(KSWORD_ARK_HVM_METRICS_RESPONSE* Response);
