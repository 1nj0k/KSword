#pragma once

#include <ntddk.h>
#include <wdf.h>
#include "driver/KswordArkDdmaIoctl.h"

EXTERN_C_START

/*
 * 中文说明：DDMA（Disk Direct Memory Access）后端。借助 \Driver\Disk 设备栈的
 * ATA_PASS_THROUGH_DIRECT + ATA_FLAGS_USE_DMA 让磁盘控制器对任意物理地址做总线
 * 主控 DMA。数据通路不经过 CPU 页表与 SLAT/EPT，因此能读到被上层虚拟化重定向
 * 的物理页；代价是必须借用一块磁盘扇区当中转站。
 *
 * 三个函数都要求 PASSIVE_LEVEL（内部要等待 IRP 完成），返回 STATUS_SUCCESS 只
 * 表示响应包有效，语义结果一律通过 response 里的 status 字段表达。
 */

NTSTATUS
KswordARKDriverDdmaQueryCapability(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_DDMA_QUERY_CAPABILITY_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    );

NTSTATUS
KswordARKDriverDdmaReadPhysical(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_DDMA_READ_PHYSICAL_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    );

NTSTATUS
KswordARKDriverDdmaWritePhysical(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_DDMA_WRITE_PHYSICAL_REQUEST* Request,
    _In_ size_t RequestBufferLength,
    _Out_ size_t* BytesWrittenOut
    );

EXTERN_C_END
