/*++

Module Name:

    memory_ddma_ioctl.c

Abstract:

    IOCTL handlers for the DDMA (Disk Direct Memory Access) backend.

    这些 handler 只负责 WDF 缓冲获取、请求快照、flags/长度初筛与写访问校验，
    真正的磁盘 DMA 逻辑全部在 memory_ddma.c 的 backend 里。

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "ark/ark_ddma.h"
#include "../../dispatch/ioctl_validation.h"
#include "../../platform/pool_compat.h"

#include <ntstrsafe.h>
#include <stdarg.h>

// DDMA 写请求带尾随 data[]，长度不定放不进栈快照，改用池副本。
#define KSWORD_ARK_DDMA_IOCTL_POOL_TAG 'iDsK'

// 头长度必须与 memory_ddma.c 用同一个判据：FIELD_OFFSET 而不是
// "sizeof(结构) - sizeof(尾随成员)"。两者差着结构尾部的对齐填充，混用会让
// R3 解析整体错位。理由详见 memory_ddma.c 里同名宏上方的说明。
#define KSWORD_ARK_DDMA_QUERY_RESPONSE_HEADER_SIZE \
    FIELD_OFFSET(KSWORD_ARK_DDMA_QUERY_CAPABILITY_RESPONSE, entries)

#define KSWORD_ARK_DDMA_READ_RESPONSE_HEADER_SIZE \
    FIELD_OFFSET(KSWORD_ARK_DDMA_READ_PHYSICAL_RESPONSE, data)

#define KSWORD_ARK_DDMA_WRITE_REQUEST_HEADER_SIZE \
    FIELD_OFFSET(KSWORD_ARK_DDMA_WRITE_PHYSICAL_REQUEST, data)

static VOID
KswordARKDdmaIoctlLog(
    _In_ WDFDEVICE Device,
    _In_z_ PCSTR LevelText,
    _In_z_ PCSTR FormatText,
    ...
    )
/*++

Routine Description:

    写入 DDMA IOCTL 诊断日志。中文说明：日志只记录磁盘序号、物理地址、暂存
    LBA、长度与状态，不记录读到或写入的内存内容。

Arguments:

    Device - WDF 设备对象，用于投递日志。
    LevelText - 日志等级文本。
    FormatText - printf 风格 ANSI 格式串。
    ... - 格式参数。

Return Value:

    None. 日志失败不影响 IOCTL 请求完成。

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
KswordARKMemoryIoctlDdmaQueryCapability(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_DDMA_QUERY_CAPABILITY。中文说明：能力探测会向磁盘
    发真实 ATA 读命令，因此和读写一样要求写访问；探测本身只读扇区，不需要
    FORCE。

Arguments:

    Device - WDF 设备对象，用于日志。
    Request - 当前 IOCTL 请求。
    InputBufferLength - 输入长度；METHOD_BUFFERED 下由 WDF 再校验。
    OutputBufferLength - 输出长度；METHOD_BUFFERED 下由 WDF 再校验。
    BytesReturned - 接收写入字节数。

Return Value:

    NTSTATUS from validation or KswordARKDriverDdmaQueryCapability.

--*/
{
    KSWORD_ARK_DDMA_QUERY_CAPABILITY_REQUEST* queryRequest = NULL;
    // requestSnapshot 在后端清零共用 SystemBuffer 之前保存完整请求。
    KSWORD_ARK_DDMA_QUERY_CAPABILITY_REQUEST requestSnapshot;
    PVOID outputBuffer = NULL;
    size_t actualInputLength = 0U;
    size_t actualOutputLength = 0U;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0U;

    status = KswordARKValidateDeviceIoControlWriteAccess(Request);
    if (!NT_SUCCESS(status)) {
        KswordARKDdmaIoctlLog(Device, "Warn", "R0 ddma query denied: write access required, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKRetrieveRequiredInputBuffer(
        Request,
        sizeof(KSWORD_ARK_DDMA_QUERY_CAPABILITY_REQUEST),
        (PVOID*)&queryRequest,
        &actualInputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKDdmaIoctlLog(Device, "Error", "R0 ddma query: input invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    /*
     * METHOD_BUFFERED 的输入输出是同一个 SystemBuffer；后端会先 RtlZeroMemory
     * 输出缓冲再读 flags/scratchLba，不快照就等于拿响应头字节当暂存 LBA 去发
     * ATA 命令，可能写到磁盘上完全不相干的扇区。
     */
    RtlCopyMemory(&requestSnapshot, queryRequest, sizeof(requestSnapshot));
    queryRequest = &requestSnapshot;

    if ((queryRequest->flags & ~KSWORD_ARK_DDMA_QUERY_FLAG_ALLOWED) != 0UL ||
        queryRequest->reserved0 != 0UL ||
        queryRequest->reserved1 != 0UL) {

        KswordARKDdmaIoctlLog(Device, "Warn", "R0 ddma query: flags/reserved rejected, flags=0x%08X.", (unsigned int)queryRequest->flags);
        return STATUS_INVALID_PARAMETER;
    }

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        KSWORD_ARK_DDMA_QUERY_RESPONSE_HEADER_SIZE,
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKDdmaIoctlLog(Device, "Error", "R0 ddma query: output invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKDriverDdmaQueryCapability(
        outputBuffer,
        actualOutputLength,
        queryRequest,
        BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKDdmaIoctlLog(Device, "Error", "R0 ddma query failed: status=0x%08X.", (unsigned int)status);
        return status;
    }

    if (*BytesReturned >= KSWORD_ARK_DDMA_QUERY_RESPONSE_HEADER_SIZE) {
        KSWORD_ARK_DDMA_QUERY_CAPABILITY_RESPONSE* response =
            (KSWORD_ARK_DDMA_QUERY_CAPABILITY_RESPONSE*)outputBuffer;
        KswordARKDdmaIoctlLog(
            Device,
            "Info",
            "R0 ddma query response: status=%lu, disks=%lu, ready=%lu, caps=0x%08X.",
            (unsigned long)response->status,
            (unsigned long)response->returnedDisks,
            (unsigned long)response->readyDisks,
            (unsigned int)response->capabilityFlags);
    }

    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKMemoryIoctlDdmaReadPhysical(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_DDMA_READ_PHYSICAL。中文说明：DDMA 读取要临时占用
    磁盘扇区，属于破坏性路径，因此要求写访问并交给 safety policy 评估。

Arguments:

    Device - WDF 设备对象，用于日志和 safety policy。
    Request - 当前 IOCTL 请求。
    InputBufferLength - 输入长度；METHOD_BUFFERED 下由 WDF 再校验。
    OutputBufferLength - 输出长度；METHOD_BUFFERED 下由 WDF 再校验。
    BytesReturned - 接收写入字节数。

Return Value:

    NTSTATUS from validation, safety policy or KswordARKDriverDdmaReadPhysical.

--*/
{
    KSWORD_ARK_DDMA_READ_PHYSICAL_REQUEST* readRequest = NULL;
    KSWORD_ARK_DDMA_READ_PHYSICAL_REQUEST requestSnapshot;
    PVOID outputBuffer = NULL;
    size_t actualInputLength = 0U;
    size_t actualOutputLength = 0U;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0U;

    status = KswordARKValidateDeviceIoControlWriteAccess(Request);
    if (!NT_SUCCESS(status)) {
        KswordARKDdmaIoctlLog(Device, "Warn", "R0 ddma read denied: write access required, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKRetrieveRequiredInputBuffer(
        Request,
        sizeof(KSWORD_ARK_DDMA_READ_PHYSICAL_REQUEST),
        (PVOID*)&readRequest,
        &actualInputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKDdmaIoctlLog(Device, "Error", "R0 ddma read: input invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    // 与 query 同理：后端会先清零共用 SystemBuffer，请求必须先快照。
    RtlCopyMemory(&requestSnapshot, readRequest, sizeof(requestSnapshot));
    readRequest = &requestSnapshot;

    if ((readRequest->flags & ~KSWORD_ARK_DDMA_READ_FLAG_ALLOWED) != 0UL ||
        readRequest->reserved0 != 0UL) {

        KswordARKDdmaIoctlLog(Device, "Warn", "R0 ddma read: flags/reserved rejected, flags=0x%08X.", (unsigned int)readRequest->flags);
        return STATUS_INVALID_PARAMETER;
    }
    if (readRequest->bytesToRead == 0UL ||
        readRequest->bytesToRead > KSWORD_ARK_DDMA_READ_MAX_BYTES) {

        KswordARKDdmaIoctlLog(Device, "Warn", "R0 ddma read: size rejected, pa=0x%I64X, bytes=%lu.", readRequest->physicalAddress, (unsigned long)readRequest->bytesToRead);
        return STATUS_INVALID_PARAMETER;
    }

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        KSWORD_ARK_DDMA_READ_RESPONSE_HEADER_SIZE,
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKDdmaIoctlLog(Device, "Error", "R0 ddma read: output invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    {
        // DDMA 读要往磁盘暂存扇区写一次，按内存写操作评估而不是内存读操作。
        KSWORD_ARK_SAFETY_CONTEXT safetyContext;
        RtlZeroMemory(&safetyContext, sizeof(safetyContext));
        safetyContext.Operation = KSWORD_ARK_SAFETY_OPERATION_MEMORY_WRITE;
        safetyContext.ContextFlags = KSWORD_ARK_SAFETY_CONTEXT_FLAG_UI_CONFIRMED;
        safetyContext.TargetProcessId = 0UL;
        status = KswordARKSafetyEvaluate(Device, &safetyContext);
        if (!NT_SUCCESS(status)) {
            KswordARKDdmaIoctlLog(Device, "Warn", "R0 ddma read denied by safety policy: pa=0x%I64X, status=0x%08X.", readRequest->physicalAddress, (unsigned int)status);
            return status;
        }
    }

    status = KswordARKDriverDdmaReadPhysical(
        outputBuffer,
        actualOutputLength,
        readRequest,
        BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKDdmaIoctlLog(Device, "Error", "R0 ddma read failed: pa=0x%I64X, bytes=%lu, status=0x%08X.", readRequest->physicalAddress, (unsigned long)readRequest->bytesToRead, (unsigned int)status);
        return status;
    }

    if (*BytesReturned >= KSWORD_ARK_DDMA_READ_RESPONSE_HEADER_SIZE) {
        KSWORD_ARK_DDMA_READ_PHYSICAL_RESPONSE* response =
            (KSWORD_ARK_DDMA_READ_PHYSICAL_RESPONSE*)outputBuffer;
        KswordARKDdmaIoctlLog(
            Device,
            "Info",
            "R0 ddma read response: pa=0x%I64X, lba=0x%I64X, disk=%lu, status=%lu, read=%lu, fields=0x%08X.",
            response->requestedPhysicalAddress,
            response->scratchLba,
            (unsigned long)response->diskIndex,
            (unsigned long)response->readStatus,
            (unsigned long)response->bytesRead,
            (unsigned int)response->fieldFlags);

        // 暂存扇区没能还原是最需要被看见的状态，单独提到 Error 级别。
        if ((response->fieldFlags & KSWORD_ARK_DDMA_FIELD_SCRATCH_RESTORED) == 0UL &&
            response->backupStatus != STATUS_NOT_SUPPORTED) {

            KswordARKDdmaIoctlLog(
                Device,
                "Error",
                "R0 ddma read: scratch sectors NOT restored, lba=0x%I64X, restoreStatus=0x%08X.",
                response->scratchLba,
                (unsigned int)response->restoreStatus);
        }
    }

    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKMemoryIoctlDdmaWritePhysical(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_DDMA_WRITE_PHYSICAL。中文说明：这是本模块破坏性最强
    的路径，同时写物理内存与磁盘暂存扇区，要求写访问、FORCE 位和 safety policy
    三重放行。

Arguments:

    Device - WDF 设备对象，用于日志和 safety policy。
    Request - 当前 IOCTL 请求。
    InputBufferLength - 输入长度；METHOD_BUFFERED 下由 WDF 再校验。
    OutputBufferLength - 输出长度；METHOD_BUFFERED 下由 WDF 再校验。
    BytesReturned - 接收写入字节数。

Return Value:

    NTSTATUS from validation, safety policy or KswordARKDriverDdmaWritePhysical.

--*/
{
    KSWORD_ARK_DDMA_WRITE_PHYSICAL_REQUEST* writeRequest = NULL;
    // 写请求头加尾随 data[] 长度不定，栈上放不下，用池副本。
    KSWORD_ARK_DDMA_WRITE_PHYSICAL_REQUEST* writeRequestCopy = NULL;
    PVOID outputBuffer = NULL;
    size_t actualInputLength = 0U;
    size_t actualOutputLength = 0U;
    size_t requiredInputLength = 0U;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0U;

    status = KswordARKValidateDeviceIoControlWriteAccess(Request);
    if (!NT_SUCCESS(status)) {
        KswordARKDdmaIoctlLog(Device, "Warn", "R0 ddma write denied: write access required, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKRetrieveRequiredInputBuffer(
        Request,
        KSWORD_ARK_DDMA_WRITE_REQUEST_HEADER_SIZE,
        (PVOID*)&writeRequest,
        &actualInputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKDdmaIoctlLog(Device, "Error", "R0 ddma write: input invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    if ((writeRequest->flags & ~KSWORD_ARK_DDMA_WRITE_FLAG_ALLOWED) != 0UL ||
        writeRequest->reserved0 != 0UL) {

        KswordARKDdmaIoctlLog(Device, "Warn", "R0 ddma write: flags/reserved rejected, flags=0x%08X.", (unsigned int)writeRequest->flags);
        return STATUS_INVALID_PARAMETER;
    }
    if (writeRequest->bytesToWrite == 0UL ||
        writeRequest->bytesToWrite > KSWORD_ARK_DDMA_WRITE_MAX_BYTES) {

        KswordARKDdmaIoctlLog(Device, "Warn", "R0 ddma write: size rejected, pa=0x%I64X, bytes=%lu.", writeRequest->physicalAddress, (unsigned long)writeRequest->bytesToWrite);
        return STATUS_INVALID_PARAMETER;
    }

    requiredInputLength =
        KSWORD_ARK_DDMA_WRITE_REQUEST_HEADER_SIZE + (size_t)writeRequest->bytesToWrite;
    if (actualInputLength < requiredInputLength) {
        KswordARKDdmaIoctlLog(Device, "Warn", "R0 ddma write: input truncated, actual=%Iu, required=%Iu.", actualInputLength, requiredInputLength);
        return STATUS_INVALID_PARAMETER;
    }

    /*
     * METHOD_BUFFERED 输入输出共用 SystemBuffer。后端会先清零输出，再读
     * physicalAddress/scratchLba 并把 Request->data 送去 DMA；不先复制就等于
     * 拿响应头字节当地址和数据，直接把响应内容 DMA 到一段错误的物理内存。
     */
    writeRequestCopy = (KSWORD_ARK_DDMA_WRITE_PHYSICAL_REQUEST*)KswordARKAllocateNonPagedPool(
        requiredInputLength,
        KSWORD_ARK_DDMA_IOCTL_POOL_TAG);
    if (writeRequestCopy == NULL) {
        KswordARKDdmaIoctlLog(Device, "Error", "R0 ddma write: input copy allocation failed, bytes=%Iu.", requiredInputLength);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCopyMemory(writeRequestCopy, writeRequest, requiredInputLength);
    writeRequest = writeRequestCopy;

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        sizeof(KSWORD_ARK_DDMA_WRITE_PHYSICAL_RESPONSE),
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKDdmaIoctlLog(Device, "Error", "R0 ddma write: output invalid, status=0x%08X.", (unsigned int)status);
        ExFreePoolWithTag(writeRequestCopy, KSWORD_ARK_DDMA_IOCTL_POOL_TAG);
        return status;
    }

    {
        KSWORD_ARK_SAFETY_CONTEXT safetyContext;
        RtlZeroMemory(&safetyContext, sizeof(safetyContext));
        safetyContext.Operation = KSWORD_ARK_SAFETY_OPERATION_MEMORY_WRITE;
        safetyContext.ContextFlags = KSWORD_ARK_SAFETY_CONTEXT_FLAG_UI_CONFIRMED;
        safetyContext.TargetProcessId = 0UL;
        status = KswordARKSafetyEvaluate(Device, &safetyContext);
        if (!NT_SUCCESS(status)) {
            KswordARKDdmaIoctlLog(Device, "Warn", "R0 ddma write denied by safety policy: pa=0x%I64X, bytes=%lu, status=0x%08X.", writeRequest->physicalAddress, (unsigned long)writeRequest->bytesToWrite, (unsigned int)status);
            ExFreePoolWithTag(writeRequestCopy, KSWORD_ARK_DDMA_IOCTL_POOL_TAG);
            return status;
        }
    }

    // 后端只看得到池副本，长度也必须换成副本长度，不能再用 SystemBuffer 长度。
    status = KswordARKDriverDdmaWritePhysical(
        outputBuffer,
        actualOutputLength,
        writeRequest,
        requiredInputLength,
        BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKDdmaIoctlLog(Device, "Error", "R0 ddma write failed: pa=0x%I64X, bytes=%lu, status=0x%08X.", writeRequest->physicalAddress, (unsigned long)writeRequest->bytesToWrite, (unsigned int)status);
        ExFreePoolWithTag(writeRequestCopy, KSWORD_ARK_DDMA_IOCTL_POOL_TAG);
        return status;
    }

    if (*BytesReturned >= sizeof(KSWORD_ARK_DDMA_WRITE_PHYSICAL_RESPONSE)) {
        KSWORD_ARK_DDMA_WRITE_PHYSICAL_RESPONSE* response =
            (KSWORD_ARK_DDMA_WRITE_PHYSICAL_RESPONSE*)outputBuffer;
        KswordARKDdmaIoctlLog(
            Device,
            "Warn",
            "R0 ddma write response: pa=0x%I64X, lba=0x%I64X, disk=%lu, status=%lu, written=%lu, fields=0x%08X.",
            response->requestedPhysicalAddress,
            response->scratchLba,
            (unsigned long)response->diskIndex,
            (unsigned long)response->writeStatus,
            (unsigned long)response->bytesWritten,
            (unsigned int)response->fieldFlags);

        if ((response->fieldFlags & KSWORD_ARK_DDMA_FIELD_SCRATCH_RESTORED) == 0UL &&
            response->backupStatus != STATUS_NOT_SUPPORTED) {

            KswordARKDdmaIoctlLog(
                Device,
                "Error",
                "R0 ddma write: scratch sectors NOT restored, lba=0x%I64X, restoreStatus=0x%08X.",
                response->scratchLba,
                (unsigned int)response->restoreStatus);
        }
    }

    ExFreePoolWithTag(writeRequestCopy, KSWORD_ARK_DDMA_IOCTL_POOL_TAG);
    return STATUS_SUCCESS;
}
