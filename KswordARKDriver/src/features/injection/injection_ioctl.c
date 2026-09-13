/*++

Module Name:

    injection_ioctl.c

Abstract:

    IOCTL handlers for the injection-trace scan backend.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "ark/ark_injection_scan.h"
#include "../../dispatch/ioctl_validation.h"

#include <ntstrsafe.h>
#include <stdarg.h>

static VOID
KswordARKInjectionIoctlLog(
    _In_ WDFDEVICE Device,
    _In_z_ PCSTR LevelText,
    _In_z_ PCSTR FormatText,
    ...
    )
/*++

Routine Description:

    格式化注入扫描 IOCTL 日志。中文说明：日志只做诊断，不改变 IOCTL 完成状态。

Return Value:

    None.

--*/
{
    CHAR logMessage[KSWORD_ARK_LOG_ENTRY_MAX_BYTES] = { 0 };
    va_list arguments;

    va_start(arguments, FormatText);
    if (NT_SUCCESS(RtlStringCbVPrintfA(logMessage, sizeof(logMessage), FormatText, arguments))) {
        (VOID)KswordARKDriverEnqueueLogFrame(Device, LevelText, logMessage);
    }
    va_end(arguments);
}

NTSTATUS
KswordARKInjectionIoctlEnumerateProcessVad(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_ENUMERATE_PROCESS_VAD。

Return Value:

    NTSTATUS 表示缓冲校验或查询执行结果。

--*/
{
    KSWORD_ARK_ENUMERATE_PROCESS_VAD_REQUEST requestSnapshot;
    PVOID inputBuffer = NULL;
    PVOID outputBuffer = NULL;
    size_t actualInputLength = 0;
    size_t actualOutputLength = 0;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0;

    status = KswordARKRetrieveRequiredInputBuffer(
        Request,
        sizeof(KSWORD_ARK_ENUMERATE_PROCESS_VAD_REQUEST),
        &inputBuffer,
        &actualInputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKInjectionIoctlLog(Device, "Error", "R0 enum-vad ioctl: input invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    /*
     * METHOD_BUFFERED 的输入输出是同一个 SystemBuffer。后端会先把输出整块清零，
     * 那一刻请求里的 pid/范围就没了 —— 必须先快照。仓库里因为漏了这一步蓝过 0x50。
     */
    RtlCopyMemory(&requestSnapshot, inputBuffer, sizeof(requestSnapshot));

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        KSWORD_ARK_INJECTION_VAD_RESPONSE_HEADER_SIZE,
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKInjectionIoctlLog(Device, "Error", "R0 enum-vad ioctl: output invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKDriverEnumerateProcessVad(
        outputBuffer,
        actualOutputLength,
        &requestSnapshot,
        BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKInjectionIoctlLog(
            Device,
            "Error",
            "R0 enum-vad failed: pid=%lu, status=0x%08X.",
            (unsigned long)requestSnapshot.processId,
            (unsigned int)status);
        return status;
    }

    KswordARKInjectionIoctlLog(
        Device,
        "Info",
        "R0 enum-vad completed: pid=%lu, bytes=%llu.",
        (unsigned long)requestSnapshot.processId,
        (unsigned long long)*BytesReturned);
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKInjectionIoctlScanExecutablePte(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_SCAN_PROCESS_EXECUTABLE_PTE。

Return Value:

    NTSTATUS 表示缓冲校验或扫描执行结果。

--*/
{
    KSWORD_ARK_SCAN_PROCESS_EXECUTABLE_PTE_REQUEST requestSnapshot;
    PVOID inputBuffer = NULL;
    PVOID outputBuffer = NULL;
    size_t actualInputLength = 0;
    size_t actualOutputLength = 0;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0;

    status = KswordARKRetrieveRequiredInputBuffer(
        Request,
        sizeof(KSWORD_ARK_SCAN_PROCESS_EXECUTABLE_PTE_REQUEST),
        &inputBuffer,
        &actualInputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKInjectionIoctlLog(Device, "Error", "R0 scan-exec-pte ioctl: input invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    RtlCopyMemory(&requestSnapshot, inputBuffer, sizeof(requestSnapshot));

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        KSWORD_ARK_INJECTION_PTE_RESPONSE_HEADER_SIZE,
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKInjectionIoctlLog(Device, "Error", "R0 scan-exec-pte ioctl: output invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKDriverScanProcessExecutablePte(
        outputBuffer,
        actualOutputLength,
        &requestSnapshot,
        BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKInjectionIoctlLog(
            Device,
            "Error",
            "R0 scan-exec-pte failed: pid=%lu, status=0x%08X.",
            (unsigned long)requestSnapshot.processId,
            (unsigned int)status);
        return status;
    }

    KswordARKInjectionIoctlLog(
        Device,
        "Info",
        "R0 scan-exec-pte completed: pid=%lu, bytes=%llu.",
        (unsigned long)requestSnapshot.processId,
        (unsigned long long)*BytesReturned);
    return STATUS_SUCCESS;
}
