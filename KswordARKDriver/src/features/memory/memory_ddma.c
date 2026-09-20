/*++

Module Name:

    memory_ddma.c

Abstract:

    DDMA (Disk Direct Memory Access) backend for KswordARK.

    借助 \Driver\Disk 设备栈的 ATA_PASS_THROUGH_DIRECT + ATA_FLAGS_USE_DMA，
    让磁盘控制器对任意物理地址做总线主控 DMA。数据通路不经过 CPU 页表与
    SLAT/EPT，因此能读到被上层虚拟化重定向或隐藏的物理页。

    参考实现：https://github.com/btbd/ddma（Disks for DMA）。

    与上游 PoC 的三点差异：
    1. 暂存扇区 LBA 由调用方显式给出，本模块不提供任何默认值；上游固定用
       LBA 0，也就是 MBR/GPT 保护扇区。
    2. 备份与还原成对出现在同一次请求内（Session Begin/End），上游只在驱动
       加载时备份一次。
    3. 支持 LBA48，LBA >= 2^28 时自动切到 0x24/0x34 命令；上游只有 28 位。

Environment:

    Kernel-mode Driver Framework, PASSIVE_LEVEL only.

--*/

#include "ark/ark_driver.h"
#include "ark/ark_ddma.h"
#include "driver/KswordArkDdmaPlan.h"
#include "../../platform/pool_compat.h"

#include <ntddscsi.h>
// DISK_GEOMETRY 与 IOCTL_DISK_GET_DRIVE_GEOMETRY 在这里；SCSI 直通要用真实
// 扇区大小换算 CDB 的块数，拿不到就不能试那条路。
#include <ntdddisk.h>
#include <ntstrsafe.h>

#define KSWORD_ARK_DDMA_POOL_TAG 'dDsK'

// 单条 ATA 命令的超时秒数。上游用 2 秒，这里保持一致：DDMA 的每一步都在
// 占用磁盘暂存扇区，超时越长，脏扇区暴露窗口越长。
#define KSWORD_ARK_ATA_IO_TIMEOUT 2UL

/*
 * 头长度一律用 FIELD_OFFSET，不用 "sizeof(结构) - sizeof(尾随成员)"。
 *
 * 两者在带尾随数组的结构上并不相等：结构体尾部有对齐填充，sizeof 会把它算进去。
 * 以本协议的读响应为例，sizeof-sizeof(data) = 335 而 offsetof(data) = 328。
 * 如果用前者算"输出缓冲还剩多少可用"、却用 ->data 写负载，R3 按哪个数字解析都会
 * 错位 7 字节。既有的物理读协议就踩过这个坑（见 ArkDriverMemory.cpp 的注释），
 * 新协议直接让两个数字恒等，把这类错位从根上去掉。
 */
#define KSWORD_ARK_DDMA_READ_RESPONSE_HEADER_SIZE \
    FIELD_OFFSET(KSWORD_ARK_DDMA_READ_PHYSICAL_RESPONSE, data)

#define KSWORD_ARK_DDMA_WRITE_REQUEST_HEADER_SIZE \
    FIELD_OFFSET(KSWORD_ARK_DDMA_WRITE_PHYSICAL_REQUEST, data)

#define KSWORD_ARK_DDMA_QUERY_RESPONSE_HEADER_SIZE \
    FIELD_OFFSET(KSWORD_ARK_DDMA_QUERY_CAPABILITY_RESPONSE, entries)

// 下面三个例程由 ntifs.h 声明而 ntddk.h 不声明；本编译单元按项目惯例停留在
// ntddk.h，本地补上与公开原型一致的声明，不要改成包含 ntifs.h。
NTSYSAPI
NTSTATUS
NTAPI
ObReferenceObjectByName(
    _In_ PUNICODE_STRING ObjectName,
    _In_ ULONG Attributes,
    _In_opt_ PACCESS_STATE PassedAccessState,
    _In_opt_ ACCESS_MASK DesiredAccess,
    _In_ POBJECT_TYPE ObjectType,
    _In_ KPROCESSOR_MODE AccessMode,
    _Inout_opt_ PVOID ParseContext,
    _Out_ PVOID* Object
    );

NTKERNELAPI
NTSTATUS
ObQueryNameString(
    _In_ PVOID Object,
    _Out_writes_bytes_opt_(Length) POBJECT_NAME_INFORMATION ObjectNameInfo,
    _In_ ULONG Length,
    _Out_ PULONG ReturnLength
    );

NTKERNELAPI
NTSTATUS
IoEnumerateDeviceObjectList(
    _In_ PDRIVER_OBJECT DriverObject,
    _Out_writes_bytes_to_opt_(
        DeviceObjectListSize,
        (*ActualNumberDeviceObjects) * sizeof(PDEVICE_OBJECT))
        PDEVICE_OBJECT* DeviceObjectList,
    _In_ ULONG DeviceObjectListSize,
    _Out_ PULONG ActualNumberDeviceObjects
    );

extern POBJECT_TYPE* IoDriverObjectType;

// ============================================================
// 磁盘设备枚举
// ============================================================

typedef struct _KSWORD_ARK_DDMA_DISK_LIST
{
    PDRIVER_OBJECT DriverObject;    // \Driver\Disk 驱动对象，持引用。
    PDEVICE_OBJECT* Devices;        // 设备对象数组，每一项都持引用。
    ULONG DeviceCount;              // 数组长度。
} KSWORD_ARK_DDMA_DISK_LIST;

static VOID
KswordARKDdmaReleaseDiskList(
    _Inout_ KSWORD_ARK_DDMA_DISK_LIST* List
    )
/*++

Routine Description:

    释放磁盘设备列表。中文说明：IoEnumerateDeviceObjectList 会对每个设备对象
    加引用，驱动对象也持有一份引用，两者都必须在同一处成对释放，否则设备栈
    永远无法卸载。

Arguments:

    List - 待释放的列表；函数返回后所有指针被清空，可安全重复调用。

Return Value:

    None.

--*/
{
    ULONG index = 0UL;

    if (List == NULL) {
        return;
    }

    if (List->Devices != NULL) {
        for (index = 0UL; index < List->DeviceCount; ++index) {
            if (List->Devices[index] != NULL) {
                ObDereferenceObject(List->Devices[index]);
                List->Devices[index] = NULL;
            }
        }
        ExFreePoolWithTag(List->Devices, KSWORD_ARK_DDMA_POOL_TAG);
        List->Devices = NULL;
    }
    List->DeviceCount = 0UL;

    if (List->DriverObject != NULL) {
        ObDereferenceObject(List->DriverObject);
        List->DriverObject = NULL;
    }
}

static NTSTATUS
KswordARKDdmaAcquireDiskList(
    _Out_ KSWORD_ARK_DDMA_DISK_LIST* List
    )
/*++

Routine Description:

    枚举 \Driver\Disk 上挂着的全部设备对象。中文说明：DDMA 需要一个能接受
    IOCTL_ATA_PASS_THROUGH_DIRECT 的磁盘设备对象；disk.sys 的每个物理磁盘都
    对应一个设备对象，这里把它们全部取出来交给调用方按 index 选择。

Arguments:

    List - 输出列表；成功时调用方必须用 KswordARKDdmaReleaseDiskList 释放。

Return Value:

    STATUS_SUCCESS 表示列表有效；其它值表示驱动对象或设备枚举失败。

--*/
{
    UNICODE_STRING diskDriverName = RTL_CONSTANT_STRING(L"\\Driver\\Disk");
    PDRIVER_OBJECT driverObject = NULL;
    PDEVICE_OBJECT* devices = NULL;
    ULONG deviceCount = 0UL;
    ULONG allocationBytes = 0UL;
    NTSTATUS status = STATUS_SUCCESS;

    if (List == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(List, sizeof(*List));

    if (IoDriverObjectType == NULL || *IoDriverObjectType == NULL) {
        return STATUS_NOT_SUPPORTED;
    }

    status = ObReferenceObjectByName(
        &diskDriverName,
        OBJ_CASE_INSENSITIVE,
        NULL,
        0,
        *IoDriverObjectType,
        KernelMode,
        NULL,
        (PVOID*)&driverObject);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // 第一次调用只为问出设备数量，成功路径必然返回 BUFFER_TOO_SMALL。
    status = IoEnumerateDeviceObjectList(driverObject, NULL, 0, &deviceCount);
    if (status != STATUS_BUFFER_TOO_SMALL) {
        ObDereferenceObject(driverObject);
        // 数量为 0 时上面会返回 SUCCESS，这种情况按"没有磁盘"处理。
        return NT_SUCCESS(status) ? STATUS_NO_SUCH_DEVICE : status;
    }
    if (deviceCount == 0UL) {
        ObDereferenceObject(driverObject);
        return STATUS_NO_SUCH_DEVICE;
    }
    if (deviceCount > KSWORD_ARK_DDMA_DISK_LIMIT_HARD) {
        deviceCount = KSWORD_ARK_DDMA_DISK_LIMIT_HARD;
    }

    allocationBytes = deviceCount * (ULONG)sizeof(PDEVICE_OBJECT);
    devices = (PDEVICE_OBJECT*)KswordARKAllocateNonPagedPool(
        allocationBytes,
        KSWORD_ARK_DDMA_POOL_TAG);
    if (devices == NULL) {
        ObDereferenceObject(driverObject);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(devices, allocationBytes);

    status = IoEnumerateDeviceObjectList(driverObject, devices, allocationBytes, &deviceCount);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(devices, KSWORD_ARK_DDMA_POOL_TAG);
        ObDereferenceObject(driverObject);
        return status;
    }

    List->DriverObject = driverObject;
    List->Devices = devices;
    List->DeviceCount = deviceCount;
    return STATUS_SUCCESS;
}

static VOID
KswordARKDdmaQueryDeviceName(
    _In_ PDEVICE_OBJECT Device,
    _Out_writes_(NameChars) PWCHAR NameBuffer,
    _In_ ULONG NameChars,
    _Out_ BOOLEAN* NamePresentOut
    )
/*++

Routine Description:

    查询磁盘设备对象名，例如 \Device\Harddisk0\DR0。中文说明：名字只用于让
    R3 确认"这次用的还是上次探测到的那块盘"，查不到名字不影响读写路径。

Arguments:

    Device - 目标设备对象。
    NameBuffer - 输出缓冲。
    NameChars - 输出缓冲可容纳的字符数（含结尾 NUL）。
    NamePresentOut - 输出是否成功拿到名字。

Return Value:

    None. 失败时只是把名字留空。

--*/
{
    POBJECT_NAME_INFORMATION nameInfo = NULL;
    ULONG requiredBytes = 0UL;
    NTSTATUS status = STATUS_SUCCESS;

    if (NameBuffer == NULL || NameChars == 0UL || NamePresentOut == NULL) {
        return;
    }
    *NamePresentOut = FALSE;
    NameBuffer[0] = L'\0';

    if (Device == NULL) {
        return;
    }

    status = ObQueryNameString(Device, NULL, 0, &requiredBytes);
    if (status != STATUS_INFO_LENGTH_MISMATCH && status != STATUS_BUFFER_OVERFLOW &&
        status != STATUS_BUFFER_TOO_SMALL) {
        return;
    }
    if (requiredBytes == 0UL || requiredBytes > (64UL * 1024UL)) {
        return;
    }

    nameInfo = (POBJECT_NAME_INFORMATION)KswordARKAllocateNonPagedPool(
        requiredBytes,
        KSWORD_ARK_DDMA_POOL_TAG);
    if (nameInfo == NULL) {
        return;
    }
    RtlZeroMemory(nameInfo, requiredBytes);

    status = ObQueryNameString(Device, nameInfo, requiredBytes, &requiredBytes);
    if (NT_SUCCESS(status) && nameInfo->Name.Buffer != NULL && nameInfo->Name.Length > 0) {
        // RtlStringCchCopyNW 会自动补 NUL 并按目标容量截断。
        if (NT_SUCCESS(RtlStringCchCopyNW(
                NameBuffer,
                NameChars,
                nameInfo->Name.Buffer,
                nameInfo->Name.Length / sizeof(WCHAR)))) {

            *NamePresentOut = TRUE;
        }
    }

    ExFreePoolWithTag(nameInfo, KSWORD_ARK_DDMA_POOL_TAG);
}

// ============================================================
// ATA 命令下发
// ============================================================

static NTSTATUS
KswordARKDdmaIssueAtaCommand(
    _In_ PDEVICE_OBJECT Device,
    _In_ USHORT DirectionFlag,
    _In_ BOOLEAN IsWrite,
    _In_ ULONG64 Lba,
    _In_ PVOID DataBuffer
    )
/*++

Routine Description:

    向磁盘设备发一条 ATA DMA 读/写命令。中文说明：DataBuffer 是本技术的全部
    要害——storport 会为它建立 MDL 并把物理页填进 HBA 的散列表，所以只要
    DataBuffer 指向 MmMapIoSpace 出来的任意物理页，HBA 就会对那段物理地址
    做真实 DMA，完全不经过 CPU 页表与 SLAT。

    LBA 小于 2^28 时用 28 位命令，LBA 寄存器分布在 CurrentTaskFile[2..4] 与
    Device 寄存器低 4 位；否则切到 48 位命令，高 3 个字节放在 PreviousTaskFile，
    此时 Device 寄存器不再承载 LBA 位。

Arguments:

    Device - 目标磁盘设备对象。
    DirectionFlag - ATA_FLAGS_DATA_IN（读盘）或 ATA_FLAGS_DATA_OUT（写盘）。
    IsWrite - TRUE 表示往磁盘写，决定选哪个命令码。
    Lba - 暂存扇区起始 LBA。
    DataBuffer - 非分页数据缓冲区或物理页映射，长度固定为一次传输长度。

Return Value:

    IRP 完成状态。

--*/
{
    KEVENT completionEvent;
    ATA_PASS_THROUGH_DIRECT request;
    IO_STATUS_BLOCK ioStatusBlock;
    KSWORD_ARK_DDMA_TASKFILE taskFile;
    PIRP irp = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    if (Device == NULL || DataBuffer == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    // 寄存器编码交给 KswordArkDdmaPlan.h 里的共用纯函数：单元测试覆盖的就是
    // 这一份实现，不在这里另写一遍 28/48 位的位运算。
    taskFile = KswordArkDdmaEncodeTaskFile(
        Lba,
        KSWORD_ARK_DDMA_TRANSFER_BYTES / KSWORD_ARK_DDMA_SECTOR_SIZE,
        IsWrite ? 1 : 0);
    if (!taskFile.valid) {
        return STATUS_INVALID_PARAMETER;
    }

    KeInitializeEvent(&completionEvent, SynchronizationEvent, FALSE);
    RtlZeroMemory(&request, sizeof(request));
    RtlZeroMemory(&ioStatusBlock, sizeof(ioStatusBlock));

    request.Length = sizeof(request);
    request.AtaFlags = (USHORT)(DirectionFlag | ATA_FLAGS_USE_DMA | taskFile.extraAtaFlags);
    request.DataTransferLength = KSWORD_ARK_DDMA_TRANSFER_BYTES;
    request.TimeOutValue = KSWORD_ARK_ATA_IO_TIMEOUT;
    request.DataBuffer = DataBuffer;

    RtlCopyMemory(
        request.CurrentTaskFile,
        taskFile.currentTaskFile,
        sizeof(request.CurrentTaskFile));
    RtlCopyMemory(
        request.PreviousTaskFile,
        taskFile.previousTaskFile,
        sizeof(request.PreviousTaskFile));

    irp = IoBuildDeviceIoControlRequest(
        IOCTL_ATA_PASS_THROUGH_DIRECT,
        Device,
        &request,
        sizeof(request),
        &request,
        sizeof(request),
        FALSE,
        &completionEvent,
        &ioStatusBlock);
    if (irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = IoCallDriver(Device, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&completionEvent, Executive, KernelMode, FALSE, NULL);
        status = ioStatusBlock.Status;
    }

    return status;
}

// ============================================================
// SCSI 直通传输（覆盖 NVMe / SAS / SATA / 合成 SCSI）
// ============================================================

static NTSTATUS
KswordARKDdmaIssueScsiCommand(
    _In_ PDEVICE_OBJECT Device,
    _In_ BOOLEAN IsWrite,
    _In_ ULONG64 Lba,
    _In_ ULONG SectorSize,
    _In_ PVOID DataBuffer
    )
/*++

Routine Description:

    用 SCSI 直通读写暂存扇区。中文说明：这是 ATA 之外的第二条传输，也是现代
    机器上真正能用的那条——DDMA 需要的是"能把指定物理页当 DMA 目标的直通通道"，
    而不是"ATA"。IOCTL_SCSI_PASS_THROUGH_DIRECT 同样带 _DIRECT，storport 会为
    DataBuffer 建 MDL、把物理页填进控制器的散列表；stornvme 则把 SCSI
    READ/WRITE 翻译成 NVMe 命令。于是 NVMe、SAS/SATA 与合成 SCSI 盘都走得通。

    请求缓冲布局是"SCSI_PASS_THROUGH_DIRECT 头 + sense 区"，两者在同一块内存里，
    SenseInfoOffset 指向 sense 区相对头部的偏移。

Arguments:

    Device - 目标磁盘设备对象。
    IsWrite - TRUE 表示往磁盘写。
    Lba - 暂存扇区起始逻辑块号。
    SectorSize - 该盘逻辑扇区大小；CDB 里的传输长度按块计，必须用真实值。
    DataBuffer - 非分页数据缓冲或物理页映射，长度为一次传输长度。

Return Value:

    IRP 完成状态；SCSI 状态非零时转成 STATUS_IO_DEVICE_ERROR。

--*/
{
    typedef struct _KSWORD_ARK_SCSI_REQUEST
    {
        SCSI_PASS_THROUGH_DIRECT Header;
        UCHAR Sense[32];
    } KSWORD_ARK_SCSI_REQUEST;

    KEVENT completionEvent;
    KSWORD_ARK_SCSI_REQUEST request;
    IO_STATUS_BLOCK ioStatusBlock;
    KSWORD_ARK_DDMA_CDB command;
    PIRP irp = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    if (Device == NULL || DataBuffer == NULL || SectorSize == 0UL) {
        return STATUS_INVALID_PARAMETER;
    }

    // CDB 编码交给 KswordArkDdmaPlan.h 里的共用纯函数：单元测试覆盖的就是这一份。
    // 注意 CDB 里的传输长度单位是块而不是字节，除不尽时那个函数会直接拒绝。
    command = KswordArkDdmaEncodeCdb(
        Lba,
        KSWORD_ARK_DDMA_TRANSFER_BYTES,
        SectorSize,
        IsWrite ? 1 : 0);
    if (!command.valid) {
        return STATUS_INVALID_PARAMETER;
    }

    KeInitializeEvent(&completionEvent, SynchronizationEvent, FALSE);
    RtlZeroMemory(&request, sizeof(request));
    RtlZeroMemory(&ioStatusBlock, sizeof(ioStatusBlock));

    request.Header.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
    request.Header.CdbLength = command.cdbLength;
    request.Header.SenseInfoLength = (UCHAR)sizeof(request.Sense);
    request.Header.DataIn = (UCHAR)(IsWrite ? SCSI_IOCTL_DATA_OUT : SCSI_IOCTL_DATA_IN);
    request.Header.DataTransferLength = KSWORD_ARK_DDMA_TRANSFER_BYTES;
    request.Header.TimeOutValue = KSWORD_ARK_ATA_IO_TIMEOUT;
    request.Header.DataBuffer = DataBuffer;
    request.Header.SenseInfoOffset =
        (ULONG)FIELD_OFFSET(KSWORD_ARK_SCSI_REQUEST, Sense);
    RtlCopyMemory(request.Header.Cdb, command.cdb, sizeof(request.Header.Cdb));

    irp = IoBuildDeviceIoControlRequest(
        IOCTL_SCSI_PASS_THROUGH_DIRECT,
        Device,
        &request,
        sizeof(request),
        &request,
        sizeof(request),
        FALSE,
        &completionEvent,
        &ioStatusBlock);
    if (irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = IoCallDriver(Device, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&completionEvent, Executive, KernelMode, FALSE, NULL);
        status = ioStatusBlock.Status;
    }

    // IRP 成功不等于命令成功：SCSI 状态非零说明设备拒绝了这条命令，
    // 把它当成功会让调用方以为数据搬过去了，而缓冲区里其实是旧内容。
    if (NT_SUCCESS(status) && request.Header.ScsiStatus != 0U) {
        status = STATUS_IO_DEVICE_ERROR;
    }

    return status;
}

// ============================================================
// 暂存扇区会话
// ============================================================

typedef struct _KSWORD_ARK_DDMA_SESSION
{
    PDEVICE_OBJECT Device;      // 目标磁盘设备对象，不持有额外引用。
    ULONG64 ScratchLba;         // 暂存扇区起始 LBA。
    PVOID BackupBuffer;         // 暂存扇区原始内容，连续非分页内存。
    BOOLEAN BackupValid;        // 备份是否成功，决定还原能不能做。
    ULONG Transport;            // KSWORD_ARK_DDMA_TRANSPORT_*：本会话走哪条传输。
    ULONG SectorSize;           // 该盘逻辑扇区大小，SCSI 传输按它换算块数。
} KSWORD_ARK_DDMA_SESSION;

static NTSTATUS
KswordARKDdmaTransfer(
    _In_ PDEVICE_OBJECT Device,
    _In_ ULONG Transport,
    _In_ ULONG SectorSize,
    _In_ BOOLEAN IsWrite,
    _In_ ULONG64 Lba,
    _In_ PVOID DataBuffer
    )
/*++

Routine Description:

    按会话选定的传输执行一次扇区读写。中文说明：把"走哪条直通"收敛到这一处，
    上层的备份/搬运/还原三步都不需要再关心 ATA 与 SCSI 的差别。

Arguments:

    Device - 目标磁盘设备对象。
    Transport - KSWORD_ARK_DDMA_TRANSPORT_ATA 或 _SCSI。
    SectorSize - 逻辑扇区大小，SCSI 路径需要。
    IsWrite - TRUE 表示往磁盘写。
    Lba - 暂存扇区起始 LBA。
    DataBuffer - 数据缓冲或物理页映射。

Return Value:

    对应传输的 NTSTATUS；传输标识非法时返回 STATUS_INVALID_PARAMETER。

--*/
{
    if (Transport == KSWORD_ARK_DDMA_TRANSPORT_ATA) {
        return KswordARKDdmaIssueAtaCommand(
            Device,
            IsWrite ? ATA_FLAGS_DATA_OUT : ATA_FLAGS_DATA_IN,
            IsWrite,
            Lba,
            DataBuffer);
    }
    if (Transport == KSWORD_ARK_DDMA_TRANSPORT_SCSI) {
        return KswordARKDdmaIssueScsiCommand(Device, IsWrite, Lba, SectorSize, DataBuffer);
    }
    return STATUS_INVALID_PARAMETER;
}

static NTSTATUS
KswordARKDdmaSessionBegin(
    _Inout_ KSWORD_ARK_DDMA_SESSION* Session,
    _In_ PDEVICE_OBJECT Device,
    _In_ ULONG64 ScratchLba,
    _In_ PVOID BackupBuffer,
    _In_ ULONG Transport,
    _In_ ULONG SectorSize
    )
/*++

Routine Description:

    开启一次 DDMA 会话：把暂存扇区的原始内容读进备份缓冲。中文说明：备份失败
    就必须整个放弃本次操作，否则后续写入会不可逆地毁掉这几个扇区的数据。

Arguments:

    Session - 输出会话状态。
    Device - 目标磁盘设备对象。
    ScratchLba - 调用方显式指定的暂存扇区起始 LBA。
    BackupBuffer - 备份缓冲，长度必须为一次传输长度。

Return Value:

    备份命令的 NTSTATUS。

--*/
{
    NTSTATUS status = STATUS_SUCCESS;

    if (Session == NULL || Device == NULL || BackupBuffer == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(Session, sizeof(*Session));
    Session->Device = Device;
    Session->ScratchLba = ScratchLba;
    Session->BackupBuffer = BackupBuffer;
    Session->BackupValid = FALSE;
    Session->Transport = Transport;
    Session->SectorSize = SectorSize;

    status = KswordARKDdmaTransfer(
        Device, Transport, SectorSize, FALSE, ScratchLba, BackupBuffer);
    if (NT_SUCCESS(status)) {
        Session->BackupValid = TRUE;
    }

    return status;
}

static NTSTATUS
KswordARKDdmaSessionEnd(
    _Inout_ KSWORD_ARK_DDMA_SESSION* Session
    )
/*++

Routine Description:

    结束 DDMA 会话：把备份内容写回暂存扇区。中文说明：这是本模块唯一一次把
    数据写回磁盘的地方；失败意味着磁盘上留下了脏扇区，调用方必须把这个状态
    原样上报给用户，不能吞掉。

Arguments:

    Session - 会话状态。

Return Value:

    还原命令的 NTSTATUS；备份本来就无效时返回 STATUS_UNSUCCESSFUL。

--*/
{
    NTSTATUS status = STATUS_SUCCESS;

    if (Session == NULL || Session->Device == NULL || Session->BackupBuffer == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!Session->BackupValid) {
        // 没有可信备份时不能乱写：写回一段未初始化数据比留着脏扇区更糟。
        return STATUS_UNSUCCESSFUL;
    }

    status = KswordARKDdmaTransfer(
        Session->Device,
        Session->Transport,
        Session->SectorSize,
        TRUE,
        Session->ScratchLba,
        Session->BackupBuffer);
    return status;
}

static NTSTATUS
KswordARKDdmaCopyPage(
    _In_ KSWORD_ARK_DDMA_SESSION* Session,
    _In_ PVOID Destination,
    _In_ PVOID Source,
    _Out_ NTSTATUS* StageOutStatusOut,
    _Out_ NTSTATUS* StageInStatusOut
    )
/*++

Routine Description:

    通过暂存扇区把 Source 指向的一页内容搬到 Destination。中文说明：这是 DDMA
    的核心原语，两端都可以是 MmMapIoSpace 出来的任意物理页映射。

    第一步把 Source 写到磁盘暂存扇区（HBA 从 Source 的物理页 DMA 读出），
    第二步把暂存扇区读回 Destination（HBA 往 Destination 的物理页 DMA 写入）。
    两步都是设备侧 DMA，CPU 从头到尾没有解引用过这两段地址。

Arguments:

    Session - 已经完成备份的会话。
    Destination - 目标缓冲或物理页映射。
    Source - 源缓冲或物理页映射。
    StageOutStatusOut - 输出第一步（写往磁盘）的状态。
    StageInStatusOut - 输出第二步（从磁盘读回）的状态。

Return Value:

    两步中第一个失败的状态；全部成功时返回 STATUS_SUCCESS。

--*/
{
    NTSTATUS status = STATUS_SUCCESS;

    if (Session == NULL || Destination == NULL || Source == NULL ||
        StageOutStatusOut == NULL || StageInStatusOut == NULL) {

        return STATUS_INVALID_PARAMETER;
    }
    *StageOutStatusOut = STATUS_NOT_SUPPORTED;
    *StageInStatusOut = STATUS_NOT_SUPPORTED;

    status = KswordARKDdmaTransfer(
        Session->Device,
        Session->Transport,
        Session->SectorSize,
        TRUE,
        Session->ScratchLba,
        Source);
    *StageOutStatusOut = status;
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = KswordARKDdmaTransfer(
        Session->Device,
        Session->Transport,
        Session->SectorSize,
        FALSE,
        Session->ScratchLba,
        Destination);
    *StageInStatusOut = status;
    return status;
}

// ============================================================
// 通用校验
// ============================================================

static ULONG
KswordARKDdmaSelectTransport(
    _In_ PDEVICE_OBJECT Device,
    _In_ ULONG64 ScratchLba,
    _In_ ULONG SectorSize,
    _In_ PVOID ProbeBuffer
    )
/*++

Routine Description:

    在真正开始搬数据之前，确定这块盘走哪条直通。中文说明：用一次**只读**暂存
    扇区的命令来判定，读不成功就换另一条；两条都不成功返回 NONE。

    为什么每次请求都重新判定而不是记住能力查询的结果：设备可能被重新枚举、
    驱动可能被换掉，而判错传输的后果是命令被拒绝后我们仍以为数据搬过去了。
    一次只读探测的代价远小于这个风险。

Arguments:

    Device - 目标磁盘设备对象。
    ScratchLba - 暂存扇区起始 LBA。
    SectorSize - 逻辑扇区大小；为 0 时不尝试 SCSI。
    ProbeBuffer - 探测用缓冲，长度为一次传输长度。

Return Value:

    KSWORD_ARK_DDMA_TRANSPORT_ATA / _SCSI / _NONE。

--*/
{
    NTSTATUS status = STATUS_SUCCESS;

    status = KswordARKDdmaIssueAtaCommand(
        Device, ATA_FLAGS_DATA_IN, FALSE, ScratchLba, ProbeBuffer);
    if (NT_SUCCESS(status)) {
        return KSWORD_ARK_DDMA_TRANSPORT_ATA;
    }

    if (SectorSize != 0UL) {
        status = KswordARKDdmaIssueScsiCommand(
            Device, FALSE, ScratchLba, SectorSize, ProbeBuffer);
        if (NT_SUCCESS(status)) {
            return KSWORD_ARK_DDMA_TRANSPORT_SCSI;
        }
    }

    return KSWORD_ARK_DDMA_TRANSPORT_NONE;
}

static BOOLEAN
KswordARKDdmaIsPhysicalRangeValid(
    _In_ ULONG64 PhysicalAddress,
    _In_ ULONG Length
    )
/*++

Routine Description:

    校验物理区间。中文说明：判据本体是 KswordArkDdmaPlan.h 里的共用纯函数，
    单元测试覆盖的就是那一份；本函数只做类型适配，不重复实现判据。

Arguments:

    PhysicalAddress - 起始物理地址。
    Length - 请求长度，必须非零。

Return Value:

    TRUE 表示区间可以接受。

--*/
{
    return KswordArkDdmaIsPhysicalRangeValid(PhysicalAddress, Length) ? TRUE : FALSE;
}

static ULONG
KswordARKDdmaQuerySectorSize(
    _In_ PDEVICE_OBJECT Device
    )
/*++

Routine Description:

    问出磁盘的逻辑扇区大小。中文说明：SCSI CDB 里的传输长度单位是**块**而不是
    字节，拿 512 当默认值去算 4Kn 盘就会让控制器读写出八倍的范围。所以这个值
    必须真的问出来，问不到时返回 0 让调用方按"不可用"处理，而不是猜一个。

Arguments:

    Device - 目标磁盘设备对象。

Return Value:

    逻辑扇区字节数；查询失败返回 0。

--*/
{
    KEVENT completionEvent;
    DISK_GEOMETRY geometry;
    IO_STATUS_BLOCK ioStatusBlock;
    PIRP irp = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    if (Device == NULL) {
        return 0UL;
    }

    KeInitializeEvent(&completionEvent, SynchronizationEvent, FALSE);
    RtlZeroMemory(&geometry, sizeof(geometry));
    RtlZeroMemory(&ioStatusBlock, sizeof(ioStatusBlock));

    irp = IoBuildDeviceIoControlRequest(
        IOCTL_DISK_GET_DRIVE_GEOMETRY,
        Device,
        NULL,
        0U,
        &geometry,
        sizeof(geometry),
        FALSE,
        &completionEvent,
        &ioStatusBlock);
    if (irp == NULL) {
        return 0UL;
    }

    status = IoCallDriver(Device, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&completionEvent, Executive, KernelMode, FALSE, NULL);
        status = ioStatusBlock.Status;
    }
    if (!NT_SUCCESS(status)) {
        return 0UL;
    }
    return geometry.BytesPerSector;
}

static PVOID
KswordARKDdmaAllocateTransferBuffer(VOID)
/*++

Routine Description:

    分配一页 DMA 工作缓冲。中文说明：用 MmAllocateContiguousMemory 并把最高
    物理地址限制在 4GB 以内，因为部分 HBA 不支持 64 位寻址（上游 ddma 的
    README 明确记了这一条）。缓冲区天然页对齐，满足 HBA 的对齐要求。

Arguments:

    None.

Return Value:

    成功返回缓冲区地址，失败返回 NULL。调用方用 MmFreeContiguousMemory 释放。

--*/
{
    PHYSICAL_ADDRESS highestAddress;
    PVOID buffer = NULL;

    highestAddress.QuadPart = (LONGLONG)MAXULONG32;
    buffer = MmAllocateContiguousMemory(KSWORD_ARK_DDMA_TRANSFER_BYTES, highestAddress);
    if (buffer != NULL) {
        RtlZeroMemory(buffer, KSWORD_ARK_DDMA_TRANSFER_BYTES);
    }
    return buffer;
}

// ============================================================
// IOCTL 后端：能力查询
// ============================================================

NTSTATUS
KswordARKDriverDdmaQueryCapability(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_DDMA_QUERY_CAPABILITY_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    枚举可用于 DDMA 的磁盘设备，并可选地对每块盘做一次真实 ATA DMA 读探测。
    中文说明：探测会向调用方指定的暂存 LBA 发读命令，只读不写，因此不需要
    FORCE；但依然要求 SCRATCH_LBA_VALID，不带就只枚举不探测。

Arguments:

    OutputBuffer - 响应缓冲，头部后紧跟磁盘条目数组。
    OutputBufferLength - 响应缓冲总长度。
    Request - 查询请求。
    BytesWrittenOut - 接收实际写入响应字节数。

Return Value:

    STATUS_SUCCESS 表示响应包有效；细节看 response->status。

--*/
{
    KSWORD_ARK_DDMA_QUERY_CAPABILITY_RESPONSE* response = NULL;
    KSWORD_ARK_DDMA_DISK_LIST diskList;
    PVOID probeBuffer = NULL;
    size_t availableBytes = 0U;
    ULONG capacityEntries = 0UL;
    ULONG maxDisks = 0UL;
    ULONG returnedDisks = 0UL;
    ULONG readyDisks = 0UL;
    ULONG index = 0UL;
    BOOLEAN probeRequested = FALSE;
    NTSTATUS status = STATUS_SUCCESS;

    if (OutputBuffer == NULL || Request == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;

    if (OutputBufferLength < KSWORD_ARK_DDMA_QUERY_RESPONSE_HEADER_SIZE) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if ((Request->flags & ~KSWORD_ARK_DDMA_QUERY_FLAG_ALLOWED) != 0UL ||
        Request->reserved0 != 0UL || Request->reserved1 != 0UL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_DDMA_QUERY_CAPABILITY_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_DDMA_PROTOCOL_VERSION;
    response->headerSize = KSWORD_ARK_DDMA_QUERY_RESPONSE_HEADER_SIZE;
    response->status = KSWORD_ARK_DDMA_QUERY_STATUS_UNAVAILABLE;
    response->entrySize = (ULONG)sizeof(KSWORD_ARK_DDMA_DISK_ENTRY);
    response->transferBytes = KSWORD_ARK_DDMA_TRANSFER_BYTES;
    response->scratchSectorCount = KSWORD_ARK_DDMA_SCRATCH_SECTOR_COUNT;
    response->capabilityFlags = KSWORD_ARK_DDMA_CAP_FLAG_LBA48_SUPPORTED;
    response->lastStatus = STATUS_NOT_SUPPORTED;

    // 内核调试开启时 MmMapIoSpace 会命中 MiShowBadMapper 并蓝屏，这个事实
    // 必须无条件上报，不能等到读写路径才让用户撞上去。
    if (*KdDebuggerEnabled != FALSE) {
        response->capabilityFlags |= KSWORD_ARK_DDMA_CAP_FLAG_KERNEL_DEBUGGER_ENABLED;
    }

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        response->status = KSWORD_ARK_DDMA_QUERY_STATUS_IRQL_REJECTED;
        response->lastStatus = STATUS_INVALID_DEVICE_STATE;
        *BytesWrittenOut = KSWORD_ARK_DDMA_QUERY_RESPONSE_HEADER_SIZE;
        return STATUS_SUCCESS;
    }

    status = KswordARKDdmaAcquireDiskList(&diskList);
    if (!NT_SUCCESS(status)) {
        response->status = (status == STATUS_NO_SUCH_DEVICE)
            ? KSWORD_ARK_DDMA_QUERY_STATUS_NO_SUPPORTED_DISK
            : KSWORD_ARK_DDMA_QUERY_STATUS_DISK_DRIVER_MISSING;
        response->lastStatus = status;
        *BytesWrittenOut = KSWORD_ARK_DDMA_QUERY_RESPONSE_HEADER_SIZE;
        return STATUS_SUCCESS;
    }
    response->capabilityFlags |= KSWORD_ARK_DDMA_CAP_FLAG_DISK_DRIVER_PRESENT;
    response->totalDisks = diskList.DeviceCount;

    // 探测需要两个条件同时成立：调用方要求探测，且给了显式的暂存 LBA。
    probeRequested =
        ((Request->flags & KSWORD_ARK_DDMA_QUERY_FLAG_PROBE_TRANSFER) != 0UL) &&
        ((Request->flags & KSWORD_ARK_DDMA_FLAG_SCRATCH_LBA_VALID) != 0UL) &&
        (Request->scratchLba < KSWORD_ARK_DDMA_LBA48_LIMIT);

    if (probeRequested) {
        probeBuffer = KswordARKDdmaAllocateTransferBuffer();
        if (probeBuffer == NULL) {
            probeRequested = FALSE;
            response->lastStatus = STATUS_INSUFFICIENT_RESOURCES;
        }
        else {
            response->capabilityFlags |= KSWORD_ARK_DDMA_CAP_FLAG_PROBE_PERFORMED;
        }
    }

    availableBytes = OutputBufferLength - KSWORD_ARK_DDMA_QUERY_RESPONSE_HEADER_SIZE;
    capacityEntries = (ULONG)(availableBytes / sizeof(KSWORD_ARK_DDMA_DISK_ENTRY));

    maxDisks = Request->maxDisks;
    if (maxDisks == 0UL) {
        maxDisks = KSWORD_ARK_DDMA_DISK_LIMIT_DEFAULT;
    }
    if (maxDisks > KSWORD_ARK_DDMA_DISK_LIMIT_HARD) {
        maxDisks = KSWORD_ARK_DDMA_DISK_LIMIT_HARD;
    }

    for (index = 0UL; index < diskList.DeviceCount; ++index) {
        KSWORD_ARK_DDMA_DISK_ENTRY* entry = NULL;
        BOOLEAN namePresent = FALSE;

        if (returnedDisks >= capacityEntries || returnedDisks >= maxDisks) {
            response->status = KSWORD_ARK_DDMA_QUERY_STATUS_TRUNCATED;
            break;
        }

        entry = &response->entries[returnedDisks];
        entry->entrySize = (ULONG)sizeof(*entry);
        entry->deviceIndex = index;
        entry->probeStatus = STATUS_NOT_SUPPORTED;
        entry->scsiProbeStatus = STATUS_NOT_SUPPORTED;
        entry->diskFlags = 0UL;
        // 扇区大小要真的问出来：SCSI CDB 按块计长，4Kn 盘上拿 512 去算会读写
        // 出八倍范围。问不到就留 0，下面的探测会据此跳过 SCSI 那条。
        entry->sectorSize = KswordARKDdmaQuerySectorSize(diskList.Devices[index]);

        KswordARKDdmaQueryDeviceName(
            diskList.Devices[index],
            entry->deviceName,
            KSWORD_ARK_DDMA_DEVICE_NAME_CHARS,
            &namePresent);
        if (namePresent) {
            entry->diskFlags |= KSWORD_ARK_DDMA_DISK_FLAG_NAME_PRESENT;
        }

        if (probeRequested) {
            // 两条传输都试：DDMA 要的是"能把指定物理页当 DMA 目标的直通通道"，
            // 不是 ATA 本身。现代机器基本都是 NVMe，只试 ATA 会让这条通路在
            // 绝大多数机器上直接判死。两条各自记状态，便于分辨"哪条不行"。
            entry->probeStatus = KswordARKDdmaIssueAtaCommand(
                diskList.Devices[index],
                ATA_FLAGS_DATA_IN,
                FALSE,
                Request->scratchLba,
                probeBuffer);
            if (NT_SUCCESS(entry->probeStatus)) {
                entry->diskFlags |= KSWORD_ARK_DDMA_DISK_FLAG_ATA_DMA_READY;
            }

            // 扇区大小问不出来时不能试 SCSI：CDB 的块数算不出来，
            // 硬填一个默认值就等于赌这块盘是 512 字节扇区。
            if (entry->sectorSize != 0UL) {
                entry->scsiProbeStatus = KswordARKDdmaIssueScsiCommand(
                    diskList.Devices[index],
                    FALSE,
                    Request->scratchLba,
                    entry->sectorSize,
                    probeBuffer);
                if (NT_SUCCESS(entry->scsiProbeStatus)) {
                    entry->diskFlags |= KSWORD_ARK_DDMA_DISK_FLAG_SCSI_DMA_READY;
                }
            }

            if ((entry->diskFlags & KSWORD_ARK_DDMA_DISK_FLAG_ANY_DMA_READY) != 0UL) {
                ++readyDisks;
            }
            // lastStatus 留给诊断：优先记还没成功的那条，全成功时记 ATA 那条。
            response->lastStatus = NT_SUCCESS(entry->probeStatus)
                ? entry->scsiProbeStatus
                : entry->probeStatus;
        }
        else {
            entry->diskFlags |= KSWORD_ARK_DDMA_DISK_FLAG_PROBE_SKIPPED;
        }

        ++returnedDisks;
    }

    if (probeBuffer != NULL) {
        MmFreeContiguousMemory(probeBuffer);
        probeBuffer = NULL;
    }
    KswordARKDdmaReleaseDiskList(&diskList);

    response->returnedDisks = returnedDisks;
    response->readyDisks = readyDisks;
    if (response->status != KSWORD_ARK_DDMA_QUERY_STATUS_TRUNCATED) {
        if (probeRequested && readyDisks == 0UL) {
            response->status = KSWORD_ARK_DDMA_QUERY_STATUS_NO_SUPPORTED_DISK;
        }
        else {
            response->status = KSWORD_ARK_DDMA_QUERY_STATUS_OK;
        }
    }

    *BytesWrittenOut =
        KSWORD_ARK_DDMA_QUERY_RESPONSE_HEADER_SIZE +
        ((size_t)returnedDisks * sizeof(KSWORD_ARK_DDMA_DISK_ENTRY));
    return STATUS_SUCCESS;
}

// ============================================================
// IOCTL 后端：物理读
// ============================================================

NTSTATUS
KswordARKDriverDdmaReadPhysical(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_DDMA_READ_PHYSICAL_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    用磁盘 DMA 读取一段物理内存。中文说明：整条路径上 CPU 从未解引用过目标
    物理页——数据是被 HBA 搬到磁盘暂存扇区、再由 HBA 搬进本驱动的工作缓冲。
    这正是它能看到 SLAT 重定向页真实内容的原因。

Arguments:

    OutputBuffer - 响应缓冲，头部后紧跟读取数据。
    OutputBufferLength - 响应缓冲总长度。
    Request - 读取请求。
    BytesWrittenOut - 接收实际写入响应字节数。

Return Value:

    STATUS_SUCCESS 表示响应包有效；细节看 response->readStatus。

--*/
{
    KSWORD_ARK_DDMA_READ_PHYSICAL_RESPONSE* response = NULL;
    KSWORD_ARK_DDMA_DISK_LIST diskList;
    KSWORD_ARK_DDMA_SESSION session;
    PHYSICAL_ADDRESS pageBase;
    PVOID transferBuffer = NULL;
    PVOID backupBuffer = NULL;
    PVOID mapping = NULL;
    PDEVICE_OBJECT device = NULL;
    size_t availableBytes = 0U;
    ULONG pageOffset = 0UL;
    ULONG bytesToRead = 0UL;
    BOOLEAN diskListAcquired = FALSE;
    BOOLEAN namePresent = FALSE;
    NTSTATUS status = STATUS_SUCCESS;
    NTSTATUS stageOutStatus = STATUS_NOT_SUPPORTED;
    NTSTATUS stageInStatus = STATUS_NOT_SUPPORTED;

    if (OutputBuffer == NULL || Request == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;

    if (OutputBufferLength < KSWORD_ARK_DDMA_READ_RESPONSE_HEADER_SIZE) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if ((Request->flags & ~KSWORD_ARK_DDMA_READ_FLAG_ALLOWED) != 0UL ||
        Request->reserved0 != 0UL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Request->bytesToRead == 0UL ||
        Request->bytesToRead > KSWORD_ARK_DDMA_READ_MAX_BYTES) {
        return STATUS_INVALID_PARAMETER;
    }

    bytesToRead = Request->bytesToRead;

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_DDMA_READ_PHYSICAL_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_DDMA_PROTOCOL_VERSION;
    response->headerSize = KSWORD_ARK_DDMA_READ_RESPONSE_HEADER_SIZE;
    response->readStatus = KSWORD_ARK_DDMA_READ_STATUS_UNAVAILABLE;
    response->mapStatus = STATUS_NOT_SUPPORTED;
    response->backupStatus = STATUS_NOT_SUPPORTED;
    response->stageOutStatus = STATUS_NOT_SUPPORTED;
    response->stageInStatus = STATUS_NOT_SUPPORTED;
    response->restoreStatus = STATUS_NOT_SUPPORTED;
    response->requestedBytes = bytesToRead;
    response->maxBytesPerRequest = KSWORD_ARK_DDMA_READ_MAX_BYTES;
    response->requestedPhysicalAddress = Request->physicalAddress;
    response->scratchLba = Request->scratchLba;
    response->diskIndex = Request->diskIndex;

    // 暂存 LBA 必须显式给出。LBA 0 是合法值，所以这里判的是 flag 位而不是
    // scratchLba 是否为零——用零当哨兵就等于偷偷替用户选了 MBR 扇区。
    if ((Request->flags & KSWORD_ARK_DDMA_FLAG_SCRATCH_LBA_VALID) == 0UL ||
        Request->scratchLba >= KSWORD_ARK_DDMA_LBA48_LIMIT) {

        response->readStatus = KSWORD_ARK_DDMA_READ_STATUS_SCRATCH_LBA_REQUIRED;
        *BytesWrittenOut = KSWORD_ARK_DDMA_READ_RESPONSE_HEADER_SIZE;
        return STATUS_SUCCESS;
    }
    // 读取同样要覆盖暂存扇区（先把目标页写过去），所以确认位一样是必需的。
    if ((Request->flags & KSWORD_ARK_DDMA_FLAG_SCRATCH_ACKNOWLEDGED) == 0UL) {
        response->readStatus = KSWORD_ARK_DDMA_READ_STATUS_SCRATCH_NOT_ACKNOWLEDGED;
        *BytesWrittenOut = KSWORD_ARK_DDMA_READ_RESPONSE_HEADER_SIZE;
        return STATUS_SUCCESS;
    }

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        response->readStatus = KSWORD_ARK_DDMA_READ_STATUS_IRQL_REJECTED;
        *BytesWrittenOut = KSWORD_ARK_DDMA_READ_RESPONSE_HEADER_SIZE;
        return STATUS_SUCCESS;
    }

    if (!KswordARKDdmaIsPhysicalRangeValid(Request->physicalAddress, bytesToRead)) {
        response->readStatus = KSWORD_ARK_DDMA_READ_STATUS_RANGE_REJECTED;
        *BytesWrittenOut = KSWORD_ARK_DDMA_READ_RESPONSE_HEADER_SIZE;
        return STATUS_SUCCESS;
    }

    availableBytes = OutputBufferLength - KSWORD_ARK_DDMA_READ_RESPONSE_HEADER_SIZE;
    if ((size_t)bytesToRead > availableBytes) {
        response->readStatus = KSWORD_ARK_DDMA_READ_STATUS_BUFFER_TOO_SMALL;
        *BytesWrittenOut = KSWORD_ARK_DDMA_READ_RESPONSE_HEADER_SIZE;
        return STATUS_SUCCESS;
    }

    status = KswordARKDdmaAcquireDiskList(&diskList);
    if (!NT_SUCCESS(status)) {
        response->readStatus = KSWORD_ARK_DDMA_READ_STATUS_DISK_NOT_FOUND;
        response->mapStatus = status;
        *BytesWrittenOut = KSWORD_ARK_DDMA_READ_RESPONSE_HEADER_SIZE;
        return STATUS_SUCCESS;
    }
    diskListAcquired = TRUE;

    if (Request->diskIndex >= diskList.DeviceCount) {
        response->readStatus = KSWORD_ARK_DDMA_READ_STATUS_DISK_NOT_FOUND;
        response->mapStatus = STATUS_NO_SUCH_DEVICE;
        KswordARKDdmaReleaseDiskList(&diskList);
        *BytesWrittenOut = KSWORD_ARK_DDMA_READ_RESPONSE_HEADER_SIZE;
        return STATUS_SUCCESS;
    }
    device = diskList.Devices[Request->diskIndex];

    // 回填设备名让 R3 能确认本次用的还是能力查询时看到的那块盘。
    KswordARKDdmaQueryDeviceName(
        device,
        response->deviceName,
        KSWORD_ARK_DDMA_DEVICE_NAME_CHARS,
        &namePresent);
    if (namePresent) {
        response->fieldFlags |= KSWORD_ARK_DDMA_FIELD_DEVICE_NAME_PRESENT;
    }

    transferBuffer = KswordARKDdmaAllocateTransferBuffer();
    backupBuffer = KswordARKDdmaAllocateTransferBuffer();
    if (transferBuffer == NULL || backupBuffer == NULL) {
        response->readStatus = KSWORD_ARK_DDMA_READ_STATUS_MAP_FAILED;
        response->mapStatus = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }

    pageOffset = (ULONG)(Request->physicalAddress & ((ULONG64)PAGE_SIZE - 1ULL));
    pageBase.QuadPart =
        (LONGLONG)(Request->physicalAddress & ~((ULONG64)PAGE_SIZE - 1ULL));

    // MmMapIoSpace 只是为了给 storport 一个可建 MDL 的内核虚拟地址；CPU 不会
    // 通过这个映射读写目标页，真正搬数据的是 HBA。
    mapping = MmMapIoSpace(pageBase, PAGE_SIZE, MmNonCached);
    if (mapping == NULL) {
        response->readStatus = KSWORD_ARK_DDMA_READ_STATUS_MAP_FAILED;
        response->mapStatus = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }
    response->mapStatus = STATUS_SUCCESS;

    {
        // 先用一次只读探测确定这块盘走哪条直通（ATA 还是 SCSI/NVMe）。
        // 两条都不通就当作没有可用磁盘，而不是硬发一条注定被拒的命令。
        const ULONG sectorSize = KswordARKDdmaQuerySectorSize(device);
        const ULONG transport = KswordARKDdmaSelectTransport(
            device, Request->scratchLba, sectorSize, transferBuffer);
        if (transport == KSWORD_ARK_DDMA_TRANSPORT_NONE) {
            response->readStatus = KSWORD_ARK_DDMA_READ_STATUS_DISK_NOT_FOUND;
            response->backupStatus = STATUS_NOT_SUPPORTED;
            goto Cleanup;
        }
        status = KswordARKDdmaSessionBegin(
            &session, device, Request->scratchLba, backupBuffer, transport, sectorSize);
    }
    response->backupStatus = status;
    if (!NT_SUCCESS(status)) {
        // 备份失败就完全不碰暂存扇区，宁可这次读不到也不能毁用户数据。
        response->readStatus = KSWORD_ARK_DDMA_READ_STATUS_BACKUP_FAILED;
        goto Cleanup;
    }

    status = KswordARKDdmaCopyPage(
        &session,
        transferBuffer,
        mapping,
        &stageOutStatus,
        &stageInStatus);
    response->stageOutStatus = stageOutStatus;
    response->stageInStatus = stageInStatus;

    // 无论拷贝成功与否都必须还原暂存扇区。
    response->restoreStatus = KswordARKDdmaSessionEnd(&session);
    if (NT_SUCCESS(response->restoreStatus)) {
        response->fieldFlags |= KSWORD_ARK_DDMA_FIELD_SCRATCH_RESTORED;
    }

    if (!NT_SUCCESS(status)) {
        response->readStatus = NT_SUCCESS(stageOutStatus)
            ? KSWORD_ARK_DDMA_READ_STATUS_STAGE_IN_FAILED
            : KSWORD_ARK_DDMA_READ_STATUS_STAGE_OUT_FAILED;
        goto Cleanup;
    }

    RtlCopyMemory(response->data, (PUCHAR)transferBuffer + pageOffset, bytesToRead);
    response->bytesRead = bytesToRead;
    response->readStatus = KSWORD_ARK_DDMA_READ_STATUS_OK;

Cleanup:

    if (mapping != NULL) {
        MmUnmapIoSpace(mapping, PAGE_SIZE);
        mapping = NULL;
    }
    if (transferBuffer != NULL) {
        MmFreeContiguousMemory(transferBuffer);
        transferBuffer = NULL;
    }
    if (backupBuffer != NULL) {
        MmFreeContiguousMemory(backupBuffer);
        backupBuffer = NULL;
    }
    if (diskListAcquired) {
        KswordARKDdmaReleaseDiskList(&diskList);
    }

    *BytesWrittenOut =
        KSWORD_ARK_DDMA_READ_RESPONSE_HEADER_SIZE + (size_t)response->bytesRead;
    return STATUS_SUCCESS;
}

// ============================================================
// IOCTL 后端：物理写
// ============================================================

NTSTATUS
KswordARKDriverDdmaWritePhysical(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_DDMA_WRITE_PHYSICAL_REQUEST* Request,
    _In_ size_t RequestBufferLength,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    用磁盘 DMA 写入一段物理内存。中文说明：DMA 的传输粒度是整页，所以当请求
    不是"页对齐且恰好一页"时，必须先 DMA 读回整页、在工作缓冲里替换目标子
    区间、再整页写回，也就是 read-modify-write。

    RMW 存在 4KB 粒度的 lost update 窗口：读回与写回之间，同页上其它字节若被
    别人改动，会被这次写回覆盖成旧值。这一事实通过响应里的
    READ_MODIFY_WRITE_USED 位如实上报，不做静默处理。

Arguments:

    OutputBuffer - 固定响应缓冲。
    OutputBufferLength - 响应缓冲总长度。
    Request - 写入请求，头部后紧跟待写字节。
    RequestBufferLength - 输入缓冲实际长度。
    BytesWrittenOut - 接收固定响应字节数。

Return Value:

    STATUS_SUCCESS 表示响应包有效；细节看 response->writeStatus。

--*/
{
    KSWORD_ARK_DDMA_WRITE_PHYSICAL_RESPONSE* response = NULL;
    KSWORD_ARK_DDMA_DISK_LIST diskList;
    KSWORD_ARK_DDMA_SESSION session;
    PHYSICAL_ADDRESS pageBase;
    PVOID transferBuffer = NULL;
    PVOID backupBuffer = NULL;
    PVOID mapping = NULL;
    PDEVICE_OBJECT device = NULL;
    size_t requiredInputBytes = 0U;
    ULONG pageOffset = 0UL;
    ULONG bytesToWrite = 0UL;
    BOOLEAN diskListAcquired = FALSE;
    BOOLEAN namePresent = FALSE;
    BOOLEAN needReadModifyWrite = FALSE;
    NTSTATUS status = STATUS_SUCCESS;
    NTSTATUS stageOutStatus = STATUS_NOT_SUPPORTED;
    NTSTATUS stageInStatus = STATUS_NOT_SUPPORTED;

    if (OutputBuffer == NULL || Request == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;

    if (OutputBufferLength < sizeof(KSWORD_ARK_DDMA_WRITE_PHYSICAL_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (RequestBufferLength < KSWORD_ARK_DDMA_WRITE_REQUEST_HEADER_SIZE) {
        return STATUS_INVALID_PARAMETER;
    }
    if ((Request->flags & ~KSWORD_ARK_DDMA_WRITE_FLAG_ALLOWED) != 0UL ||
        Request->reserved0 != 0UL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Request->bytesToWrite == 0UL ||
        Request->bytesToWrite > KSWORD_ARK_DDMA_WRITE_MAX_BYTES) {
        return STATUS_INVALID_PARAMETER;
    }

    bytesToWrite = Request->bytesToWrite;
    requiredInputBytes = KSWORD_ARK_DDMA_WRITE_REQUEST_HEADER_SIZE + (size_t)bytesToWrite;
    if (RequestBufferLength < requiredInputBytes) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_DDMA_WRITE_PHYSICAL_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_DDMA_PROTOCOL_VERSION;
    response->size = (ULONG)sizeof(*response);
    response->writeStatus = KSWORD_ARK_DDMA_WRITE_STATUS_UNAVAILABLE;
    response->mapStatus = STATUS_NOT_SUPPORTED;
    response->backupStatus = STATUS_NOT_SUPPORTED;
    response->stageOutStatus = STATUS_NOT_SUPPORTED;
    response->stageInStatus = STATUS_NOT_SUPPORTED;
    response->restoreStatus = STATUS_NOT_SUPPORTED;
    response->readbackStatus = STATUS_NOT_SUPPORTED;
    response->requestedBytes = bytesToWrite;
    response->maxBytesPerRequest = KSWORD_ARK_DDMA_WRITE_MAX_BYTES;
    response->requestedPhysicalAddress = Request->physicalAddress;
    response->scratchLba = Request->scratchLba;
    response->diskIndex = Request->diskIndex;

    // 三道门分别对应三种不同的用户错误，状态码必须分开，不能合并成一个
    // 笼统的"需要确认"，否则 UI 没法给出正确的下一步提示。
    if ((Request->flags & KSWORD_ARK_DDMA_FLAG_SCRATCH_LBA_VALID) == 0UL ||
        Request->scratchLba >= KSWORD_ARK_DDMA_LBA48_LIMIT) {

        response->writeStatus = KSWORD_ARK_DDMA_WRITE_STATUS_SCRATCH_LBA_REQUIRED;
        *BytesWrittenOut = sizeof(*response);
        return STATUS_SUCCESS;
    }
    if ((Request->flags & KSWORD_ARK_DDMA_FLAG_SCRATCH_ACKNOWLEDGED) == 0UL) {
        response->writeStatus = KSWORD_ARK_DDMA_WRITE_STATUS_SCRATCH_NOT_ACKNOWLEDGED;
        *BytesWrittenOut = sizeof(*response);
        return STATUS_SUCCESS;
    }
    if ((Request->flags & KSWORD_ARK_DDMA_FLAG_FORCE) == 0UL) {
        response->writeStatus = KSWORD_ARK_DDMA_WRITE_STATUS_FORCE_REQUIRED;
        *BytesWrittenOut = sizeof(*response);
        return STATUS_SUCCESS;
    }
    response->fieldFlags |= KSWORD_ARK_DDMA_FIELD_FORCE_USED;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        response->writeStatus = KSWORD_ARK_DDMA_WRITE_STATUS_IRQL_REJECTED;
        *BytesWrittenOut = sizeof(*response);
        return STATUS_SUCCESS;
    }

    if (!KswordARKDdmaIsPhysicalRangeValid(Request->physicalAddress, bytesToWrite)) {
        response->writeStatus = KSWORD_ARK_DDMA_WRITE_STATUS_RANGE_REJECTED;
        *BytesWrittenOut = sizeof(*response);
        return STATUS_SUCCESS;
    }

    status = KswordARKDdmaAcquireDiskList(&diskList);
    if (!NT_SUCCESS(status)) {
        response->writeStatus = KSWORD_ARK_DDMA_WRITE_STATUS_DISK_NOT_FOUND;
        response->mapStatus = status;
        *BytesWrittenOut = sizeof(*response);
        return STATUS_SUCCESS;
    }
    diskListAcquired = TRUE;

    if (Request->diskIndex >= diskList.DeviceCount) {
        response->writeStatus = KSWORD_ARK_DDMA_WRITE_STATUS_DISK_NOT_FOUND;
        response->mapStatus = STATUS_NO_SUCH_DEVICE;
        KswordARKDdmaReleaseDiskList(&diskList);
        *BytesWrittenOut = sizeof(*response);
        return STATUS_SUCCESS;
    }
    device = diskList.Devices[Request->diskIndex];

    KswordARKDdmaQueryDeviceName(
        device,
        response->deviceName,
        KSWORD_ARK_DDMA_DEVICE_NAME_CHARS,
        &namePresent);
    if (namePresent) {
        response->fieldFlags |= KSWORD_ARK_DDMA_FIELD_DEVICE_NAME_PRESENT;
    }

    transferBuffer = KswordARKDdmaAllocateTransferBuffer();
    backupBuffer = KswordARKDdmaAllocateTransferBuffer();
    if (transferBuffer == NULL || backupBuffer == NULL) {
        response->writeStatus = KSWORD_ARK_DDMA_WRITE_STATUS_MAP_FAILED;
        response->mapStatus = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }

    pageOffset = (ULONG)(Request->physicalAddress & ((ULONG64)PAGE_SIZE - 1ULL));
    pageBase.QuadPart =
        (LONGLONG)(Request->physicalAddress & ~((ULONG64)PAGE_SIZE - 1ULL));
    needReadModifyWrite =
        (pageOffset != 0UL) || (bytesToWrite != KSWORD_ARK_DDMA_TRANSFER_BYTES);

    mapping = MmMapIoSpace(pageBase, PAGE_SIZE, MmNonCached);
    if (mapping == NULL) {
        response->writeStatus = KSWORD_ARK_DDMA_WRITE_STATUS_MAP_FAILED;
        response->mapStatus = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }
    response->mapStatus = STATUS_SUCCESS;

    {
        // 与读路径同理：先判定传输，判不出来就不要往下走。
        const ULONG sectorSize = KswordARKDdmaQuerySectorSize(device);
        const ULONG transport = KswordARKDdmaSelectTransport(
            device, Request->scratchLba, sectorSize, transferBuffer);
        if (transport == KSWORD_ARK_DDMA_TRANSPORT_NONE) {
            response->writeStatus = KSWORD_ARK_DDMA_WRITE_STATUS_DISK_NOT_FOUND;
            response->backupStatus = STATUS_NOT_SUPPORTED;
            goto Cleanup;
        }
        status = KswordARKDdmaSessionBegin(
            &session, device, Request->scratchLba, backupBuffer, transport, sectorSize);
    }
    response->backupStatus = status;
    if (!NT_SUCCESS(status)) {
        response->writeStatus = KSWORD_ARK_DDMA_WRITE_STATUS_BACKUP_FAILED;
        goto Cleanup;
    }

    if (needReadModifyWrite) {
        // 第一趟：把目标页整页 DMA 读进工作缓冲，作为写回的底稿。
        response->fieldFlags |= KSWORD_ARK_DDMA_FIELD_READ_MODIFY_WRITE_USED;
        status = KswordARKDdmaCopyPage(
            &session,
            transferBuffer,
            mapping,
            &stageOutStatus,
            &stageInStatus);
        response->readbackStatus = status;
        if (!NT_SUCCESS(status)) {
            response->stageOutStatus = stageOutStatus;
            response->stageInStatus = stageInStatus;
            response->writeStatus = KSWORD_ARK_DDMA_WRITE_STATUS_READBACK_FAILED;
            goto Restore;
        }
    }

    // 把请求携带的字节替换进底稿的对应子区间。
    RtlCopyMemory((PUCHAR)transferBuffer + pageOffset, Request->data, bytesToWrite);

    // 第二趟：底稿整页 DMA 写回目标物理页。
    status = KswordARKDdmaCopyPage(
        &session,
        mapping,
        transferBuffer,
        &stageOutStatus,
        &stageInStatus);
    response->stageOutStatus = stageOutStatus;
    response->stageInStatus = stageInStatus;

Restore:

    // 与读路径一致：无论前面成败，暂存扇区都必须还原。
    response->restoreStatus = KswordARKDdmaSessionEnd(&session);
    if (NT_SUCCESS(response->restoreStatus)) {
        response->fieldFlags |= KSWORD_ARK_DDMA_FIELD_SCRATCH_RESTORED;
    }

    if (response->writeStatus == KSWORD_ARK_DDMA_WRITE_STATUS_UNAVAILABLE) {
        if (NT_SUCCESS(status)) {
            response->bytesWritten = bytesToWrite;
            response->writeStatus = KSWORD_ARK_DDMA_WRITE_STATUS_OK;
        }
        else {
            response->writeStatus = NT_SUCCESS(stageOutStatus)
                ? KSWORD_ARK_DDMA_WRITE_STATUS_STAGE_IN_FAILED
                : KSWORD_ARK_DDMA_WRITE_STATUS_STAGE_OUT_FAILED;
        }
    }

Cleanup:

    if (mapping != NULL) {
        MmUnmapIoSpace(mapping, PAGE_SIZE);
        mapping = NULL;
    }
    if (transferBuffer != NULL) {
        MmFreeContiguousMemory(transferBuffer);
        transferBuffer = NULL;
    }
    if (backupBuffer != NULL) {
        MmFreeContiguousMemory(backupBuffer);
        backupBuffer = NULL;
    }
    if (diskListAcquired) {
        KswordARKDdmaReleaseDiskList(&diskList);
    }

    *BytesWrittenOut = sizeof(*response);
    return STATUS_SUCCESS;
}
