#pragma once

#include "KswordArkMemoryIoctl.h"

// ============================================================
// KswordArkDdmaIoctl.h
// 作用：
// - 定义 DDMA（Disk Direct Memory Access）后端的 R3/R0 共享协议；
// - DDMA 借助磁盘控制器的总线主控 DMA 读写任意物理地址，
//   数据通路不经过 CPU 的页表与 SLAT/EPT，因此能看到被
//   上层虚拟化重定向或隐藏的物理页内容；
// - 参考实现：https://github.com/btbd/ddma（Disks for DMA）。
//
// 机制说明（务必读完再改动本文件）：
// - R0 对目标物理地址 MmMapIoSpace 得到内核虚拟地址，再把它
//   作为 ATA_PASS_THROUGH_DIRECT.DataBuffer 发给 \Driver\Disk
//   的设备对象；储存端口驱动会为该缓冲区建立 MDL 并把物理页
//   填入 HBA 的散列表，于是 HBA 真的对那段物理地址做了 DMA。
// - 读取 = 先把目标物理页“写到”磁盘暂存扇区，再从暂存扇区
//   “读回”调用方缓冲区；写入 = 反向执行同一对操作。
//   因此 DDMA 结构性地需要占用一小块磁盘扇区当中转站。
//
// 安全边界（协议层强制，不由 UI 自觉遵守）：
// - 暂存扇区 LBA 必须由调用方显式给出，协议不提供默认值。
//   没有置 SCRATCH_LBA_VALID 的请求一律返回
//   SCRATCH_LBA_REQUIRED，不会退化成“默认用 LBA 0”。
//   注意：LBA 0 是合法取值，所以不能用 0 当“未填写”哨兵，
//   必须靠独立的 flag 位来区分“填了 0”和“没填”。
// - 每次读写都在同一个函数内闭环完成“备份暂存扇区 → 使用 →
//   还原暂存扇区”，不跨 IOCTL 保留脏扇区。
// - 写入额外要求 FORCE 位；缺 FORCE 返回 FORCE_REQUIRED，
//   与 SCRATCH_LBA_REQUIRED 是两个不同状态，不可混为一谈。
// ============================================================

#define KSWORD_ARK_DDMA_PROTOCOL_VERSION 1UL

#define KSWORD_ARK_IOCTL_FUNCTION_DDMA_QUERY_CAPABILITY 0x917UL
#define KSWORD_ARK_IOCTL_FUNCTION_DDMA_READ_PHYSICAL 0x918UL
#define KSWORD_ARK_IOCTL_FUNCTION_DDMA_WRITE_PHYSICAL 0x919UL

// 三个 IOCTL 都用 FILE_WRITE_ACCESS：能力探测也会向磁盘发真实
// ATA 命令，读写更是直接操作物理内存，全部属于破坏性接口。
#define IOCTL_KSWORD_ARK_DDMA_QUERY_CAPABILITY \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_DDMA_QUERY_CAPABILITY, \
        METHOD_BUFFERED, \
        FILE_WRITE_ACCESS)

#define IOCTL_KSWORD_ARK_DDMA_READ_PHYSICAL \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_DDMA_READ_PHYSICAL, \
        METHOD_BUFFERED, \
        FILE_WRITE_ACCESS)

#define IOCTL_KSWORD_ARK_DDMA_WRITE_PHYSICAL \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_DDMA_WRITE_PHYSICAL, \
        METHOD_BUFFERED, \
        FILE_WRITE_ACCESS)

// ------------------------------------------------------------
// 尺寸常量
// ------------------------------------------------------------

// 暂存区固定占用一页 = 8 个 512 字节扇区。固定成常量而不是随
// 请求长度浮动，是为了让“我们会覆盖 [scratchLba, +8) 这 8 个
// 扇区”这句话对用户始终成立，UI 才有办法把影响范围讲清楚。
#define KSWORD_ARK_DDMA_SECTOR_SIZE 512UL
#define KSWORD_ARK_DDMA_TRANSFER_BYTES 4096UL
#define KSWORD_ARK_DDMA_SCRATCH_SECTOR_COUNT \
    (KSWORD_ARK_DDMA_TRANSFER_BYTES / KSWORD_ARK_DDMA_SECTOR_SIZE)

// 单次 IOCTL 读写上限就是一次 DMA 传输长度；跨页与更长的范围
// 由 R3 切片后多次调用，这样每次磁盘窗口都最短。
#define KSWORD_ARK_DDMA_READ_MAX_BYTES KSWORD_ARK_DDMA_TRANSFER_BYTES
#define KSWORD_ARK_DDMA_WRITE_MAX_BYTES KSWORD_ARK_DDMA_TRANSFER_BYTES

// 能力查询一次最多回传多少块磁盘。
#define KSWORD_ARK_DDMA_DISK_LIMIT_DEFAULT 8UL
#define KSWORD_ARK_DDMA_DISK_LIMIT_HARD 32UL

// 设备名与磁盘描述的定长上限（字符数，含结尾 NUL）。
#define KSWORD_ARK_DDMA_DEVICE_NAME_CHARS 128U

// ------------------------------------------------------------
// 请求 flags
// ------------------------------------------------------------

// UI_CONFIRMED：R3 已经在界面上向用户展示过本次操作的影响。
#define KSWORD_ARK_DDMA_FLAG_UI_CONFIRMED 0x00000001UL
// FORCE：写入路径的强制位。缺失时驱动不写入任何字节。
#define KSWORD_ARK_DDMA_FLAG_FORCE 0x00000002UL
// SCRATCH_LBA_VALID：请求里的 scratchLba 是调用方显式给出的。
// 缺失时驱动拒绝，不会替调用方挑选任何默认扇区。
#define KSWORD_ARK_DDMA_FLAG_SCRATCH_LBA_VALID 0x00000004UL
// SCRATCH_ACKNOWLEDGED：调用方确认该暂存扇区上的数据可以被
// 临时覆盖。与 SCRATCH_LBA_VALID 分开，是因为“填了地址”和
// “知道这块地址会被写”是两件事，UI 也分成两个控件。
#define KSWORD_ARK_DDMA_FLAG_SCRATCH_ACKNOWLEDGED 0x00000008UL

#define KSWORD_ARK_DDMA_READ_FLAG_ALLOWED \
    (KSWORD_ARK_DDMA_FLAG_UI_CONFIRMED | \
     KSWORD_ARK_DDMA_FLAG_SCRATCH_LBA_VALID | \
     KSWORD_ARK_DDMA_FLAG_SCRATCH_ACKNOWLEDGED)

#define KSWORD_ARK_DDMA_WRITE_FLAG_ALLOWED \
    (KSWORD_ARK_DDMA_READ_FLAG_ALLOWED | KSWORD_ARK_DDMA_FLAG_FORCE)

// 能力查询专用 flags。放在 0x00010000 以上，与上面那组通用 flags 分开两个
// 互不重叠的位段。
//
// 为什么必须分段：这两组位挤在**同一个** request->flags 字段里。最初把
// PROBE_TRANSFER 定成 0x1，与 UI_CONFIRMED 撞了同一位——"用户已确认"和
// "请做传输探测"变成同一件事，而且两边都不会报错。靶机上表现为一个
// win32=87，从错误码完全看不出是位冲突。
//
// PROBE_TRANSFER 会对每块盘真的发一次 ATA DMA 读命令来判定是否可用；
// 不带该位时只枚举设备，不发命令。
#define KSWORD_ARK_DDMA_QUERY_FLAG_PROBE_TRANSFER 0x00010000UL

// 允许位掩码**必须是调用方实际会设置的位的超集**。
// 后端判定 probeRequested 时同时看 PROBE_TRANSFER 与 SCRATCH_LBA_VALID，
// 所以后者也必须在这里放行——漏掉它，传输探测就是一条永远走不到的死路：
// 请求在 handler 的 flags 校验上就被拒了，后端那段判定一次都不会执行。
#define KSWORD_ARK_DDMA_QUERY_FLAG_ALLOWED \
    (KSWORD_ARK_DDMA_QUERY_FLAG_PROBE_TRANSFER | \
     KSWORD_ARK_DDMA_FLAG_UI_CONFIRMED | \
     KSWORD_ARK_DDMA_FLAG_SCRATCH_LBA_VALID)

// ------------------------------------------------------------
// 状态码
// ------------------------------------------------------------

#define KSWORD_ARK_DDMA_QUERY_STATUS_UNAVAILABLE 0UL
#define KSWORD_ARK_DDMA_QUERY_STATUS_OK 1UL
#define KSWORD_ARK_DDMA_QUERY_STATUS_TRUNCATED 2UL
#define KSWORD_ARK_DDMA_QUERY_STATUS_DISK_DRIVER_MISSING 3UL
#define KSWORD_ARK_DDMA_QUERY_STATUS_ENUM_FAILED 4UL
#define KSWORD_ARK_DDMA_QUERY_STATUS_NO_SUPPORTED_DISK 5UL
#define KSWORD_ARK_DDMA_QUERY_STATUS_IRQL_REJECTED 6UL

#define KSWORD_ARK_DDMA_READ_STATUS_UNAVAILABLE 0UL
#define KSWORD_ARK_DDMA_READ_STATUS_OK 1UL
#define KSWORD_ARK_DDMA_READ_STATUS_RANGE_REJECTED 2UL
#define KSWORD_ARK_DDMA_READ_STATUS_SCRATCH_LBA_REQUIRED 3UL
#define KSWORD_ARK_DDMA_READ_STATUS_SCRATCH_NOT_ACKNOWLEDGED 4UL
#define KSWORD_ARK_DDMA_READ_STATUS_DISK_NOT_FOUND 5UL
#define KSWORD_ARK_DDMA_READ_STATUS_MAP_FAILED 6UL
#define KSWORD_ARK_DDMA_READ_STATUS_BACKUP_FAILED 7UL
#define KSWORD_ARK_DDMA_READ_STATUS_STAGE_OUT_FAILED 8UL
#define KSWORD_ARK_DDMA_READ_STATUS_STAGE_IN_FAILED 9UL
#define KSWORD_ARK_DDMA_READ_STATUS_BUFFER_TOO_SMALL 10UL
#define KSWORD_ARK_DDMA_READ_STATUS_IRQL_REJECTED 11UL

#define KSWORD_ARK_DDMA_WRITE_STATUS_UNAVAILABLE 0UL
#define KSWORD_ARK_DDMA_WRITE_STATUS_OK 1UL
#define KSWORD_ARK_DDMA_WRITE_STATUS_RANGE_REJECTED 2UL
#define KSWORD_ARK_DDMA_WRITE_STATUS_SCRATCH_LBA_REQUIRED 3UL
#define KSWORD_ARK_DDMA_WRITE_STATUS_SCRATCH_NOT_ACKNOWLEDGED 4UL
#define KSWORD_ARK_DDMA_WRITE_STATUS_DISK_NOT_FOUND 5UL
#define KSWORD_ARK_DDMA_WRITE_STATUS_MAP_FAILED 6UL
#define KSWORD_ARK_DDMA_WRITE_STATUS_BACKUP_FAILED 7UL
#define KSWORD_ARK_DDMA_WRITE_STATUS_STAGE_OUT_FAILED 8UL
#define KSWORD_ARK_DDMA_WRITE_STATUS_STAGE_IN_FAILED 9UL
#define KSWORD_ARK_DDMA_WRITE_STATUS_FORCE_REQUIRED 10UL
#define KSWORD_ARK_DDMA_WRITE_STATUS_ACCESS_DENIED 11UL
#define KSWORD_ARK_DDMA_WRITE_STATUS_READBACK_FAILED 12UL
#define KSWORD_ARK_DDMA_WRITE_STATUS_IRQL_REJECTED 13UL

// ------------------------------------------------------------
// 响应 fieldFlags
// ------------------------------------------------------------

// SCRATCH_RESTORED：暂存扇区已经写回原始内容。没有这一位说明
// 还原步骤本身失败了，磁盘上留下了脏扇区，必须显著告警。
#define KSWORD_ARK_DDMA_FIELD_SCRATCH_RESTORED 0x00000001UL
// READ_MODIFY_WRITE_USED：写入不是整页覆盖，驱动先 DMA 读回
// 整页、替换请求覆盖的子区间、再整页写回。这中间存在 4KB 粒度
// 的 lost update 窗口，调用方需要知道。
#define KSWORD_ARK_DDMA_FIELD_READ_MODIFY_WRITE_USED 0x00000002UL
// DEVICE_NAME_PRESENT：响应里的 deviceName 有效，R3 可以拿它
// 与能力查询时记下的名字比对，确认本次用的还是同一块盘。
#define KSWORD_ARK_DDMA_FIELD_DEVICE_NAME_PRESENT 0x00000004UL
// FORCE_USED：本次写入确实带了 FORCE 位。
#define KSWORD_ARK_DDMA_FIELD_FORCE_USED 0x00000008UL

// ------------------------------------------------------------
// 能力响应 capabilityFlags
// ------------------------------------------------------------

// KERNEL_DEBUGGER_ENABLED：本机启用了内核调试。
// 这不是可有可无的提示：DDMA 用 MmMapIoSpace 映射普通 RAM，
// 开着内核调试时会命中 MiShowBadMapper 而蓝屏（上游 ddma 的
// README 也记了这一条）。UI 必须在这种机器上默认禁用 DDMA。
// 出路是改用自建 MDL 映射物理页绕开 MmMapIoSpace 的 RAM 检查，
// 本版本没有实现，只做如实上报。
#define KSWORD_ARK_DDMA_CAP_FLAG_KERNEL_DEBUGGER_ENABLED 0x00000001UL
// DISK_DRIVER_PRESENT：成功拿到了 \Driver\Disk 驱动对象。
#define KSWORD_ARK_DDMA_CAP_FLAG_DISK_DRIVER_PRESENT 0x00000002UL
// PROBE_PERFORMED：本次查询真的发过 ATA DMA 读命令做探测。
#define KSWORD_ARK_DDMA_CAP_FLAG_PROBE_PERFORMED 0x00000004UL
// LBA48_SUPPORTED：驱动会在 LBA >= 2^28 时自动切到 48 位命令。
#define KSWORD_ARK_DDMA_CAP_FLAG_LBA48_SUPPORTED 0x00000008UL

// ------------------------------------------------------------
// 磁盘条目 diskFlags
// ------------------------------------------------------------

// ATA_DMA_READY：探测阶段真的用 ATA 直通读到了暂存扇区。
#define KSWORD_ARK_DDMA_DISK_FLAG_ATA_DMA_READY 0x00000001UL
// PROBE_SKIPPED：请求没带 PROBE_TRANSFER，本条只做了设备枚举。
#define KSWORD_ARK_DDMA_DISK_FLAG_PROBE_SKIPPED 0x00000002UL
// NAME_PRESENT：deviceName 字段有效。
#define KSWORD_ARK_DDMA_DISK_FLAG_NAME_PRESENT 0x00000004UL
// SCSI_DMA_READY：探测阶段用 SCSI 直通读到了暂存扇区。
//
// 这一位存在的理由：DDMA 需要的是"能把指定物理页当 DMA 目标的直通通道"，
// 而不是"ATA"。现代机器基本都是 NVMe，ATA 直通直接返回 STATUS_NOT_SUPPORTED，
// 只认 ATA 会让这条通路在绝大多数机器上毫无意义。
// IOCTL_SCSI_PASS_THROUGH_DIRECT 同样走 MDL 直接 DMA，而 stornvme 会把 SCSI
// READ/WRITE 翻译成 NVMe 命令，因此它同时覆盖 NVMe、SAS/SATA 与合成 SCSI。
#define KSWORD_ARK_DDMA_DISK_FLAG_SCSI_DMA_READY 0x00000008UL

// 任意一条传输可用即可作为 DDMA 通道。
#define KSWORD_ARK_DDMA_DISK_FLAG_ANY_DMA_READY \
    (KSWORD_ARK_DDMA_DISK_FLAG_ATA_DMA_READY | \
     KSWORD_ARK_DDMA_DISK_FLAG_SCSI_DMA_READY)

// 传输通道标识，用于响应里说明这次实际走了哪条路。
#define KSWORD_ARK_DDMA_TRANSPORT_NONE 0UL
#define KSWORD_ARK_DDMA_TRANSPORT_ATA 1UL
#define KSWORD_ARK_DDMA_TRANSPORT_SCSI 2UL

// ------------------------------------------------------------
// 结构体
// ------------------------------------------------------------

typedef struct _KSWORD_ARK_DDMA_QUERY_CAPABILITY_REQUEST
{
    unsigned long flags;
    unsigned long maxDisks;
    // 探测阶段要往磁盘发读命令，同样需要一个显式的暂存 LBA；
    // 不带 SCRATCH_LBA_VALID 时驱动只枚举设备，不做传输探测。
    unsigned long long scratchLba;
    unsigned long reserved0;
    unsigned long reserved1;
} KSWORD_ARK_DDMA_QUERY_CAPABILITY_REQUEST;

typedef struct _KSWORD_ARK_DDMA_DISK_ENTRY
{
    unsigned long entrySize;
    unsigned long deviceIndex;
    unsigned long diskFlags;
    long probeStatus;       // ATA 直通探测的 NTSTATUS。
    unsigned long sectorSize; // 该盘真实逻辑扇区大小，SCSI CDB 的块数按它换算。
    long scsiProbeStatus;   // SCSI 直通探测的 NTSTATUS，与 ATA 那条分开记。
    wchar_t deviceName[KSWORD_ARK_DDMA_DEVICE_NAME_CHARS];
} KSWORD_ARK_DDMA_DISK_ENTRY;

typedef struct _KSWORD_ARK_DDMA_QUERY_CAPABILITY_RESPONSE
{
    unsigned long version;
    unsigned long headerSize;
    unsigned long status;
    unsigned long entrySize;
    unsigned long totalDisks;
    unsigned long returnedDisks;
    unsigned long readyDisks;
    unsigned long transferBytes;
    unsigned long scratchSectorCount;
    unsigned long capabilityFlags;
    long lastStatus;
    unsigned long reserved0;
    KSWORD_ARK_DDMA_DISK_ENTRY entries[1];
} KSWORD_ARK_DDMA_QUERY_CAPABILITY_RESPONSE;

typedef struct _KSWORD_ARK_DDMA_READ_PHYSICAL_REQUEST
{
    unsigned long flags;
    unsigned long diskIndex;
    unsigned long long physicalAddress;
    unsigned long long scratchLba;
    unsigned long bytesToRead;
    unsigned long reserved0;
} KSWORD_ARK_DDMA_READ_PHYSICAL_REQUEST;

typedef struct _KSWORD_ARK_DDMA_READ_PHYSICAL_RESPONSE
{
    unsigned long version;
    unsigned long headerSize;
    unsigned long fieldFlags;
    unsigned long readStatus;
    long mapStatus;
    long backupStatus;
    long stageOutStatus;
    long stageInStatus;
    long restoreStatus;
    unsigned long requestedBytes;
    unsigned long bytesRead;
    unsigned long maxBytesPerRequest;
    unsigned long long requestedPhysicalAddress;
    unsigned long long scratchLba;
    unsigned long diskIndex;
    unsigned long reserved0;
    wchar_t deviceName[KSWORD_ARK_DDMA_DEVICE_NAME_CHARS];
    unsigned char data[1];
} KSWORD_ARK_DDMA_READ_PHYSICAL_RESPONSE;

typedef struct _KSWORD_ARK_DDMA_WRITE_PHYSICAL_REQUEST
{
    unsigned long flags;
    unsigned long diskIndex;
    unsigned long long physicalAddress;
    unsigned long long scratchLba;
    unsigned long bytesToWrite;
    unsigned long reserved0;
    unsigned char data[1];
} KSWORD_ARK_DDMA_WRITE_PHYSICAL_REQUEST;

typedef struct _KSWORD_ARK_DDMA_WRITE_PHYSICAL_RESPONSE
{
    unsigned long version;
    unsigned long size;
    unsigned long fieldFlags;
    unsigned long writeStatus;
    long mapStatus;
    long backupStatus;
    long stageOutStatus;
    long stageInStatus;
    long restoreStatus;
    long readbackStatus;
    unsigned long requestedBytes;
    unsigned long bytesWritten;
    unsigned long maxBytesPerRequest;
    unsigned long reserved0;
    unsigned long long requestedPhysicalAddress;
    unsigned long long scratchLba;
    unsigned long diskIndex;
    unsigned long reserved1;
    wchar_t deviceName[KSWORD_ARK_DDMA_DEVICE_NAME_CHARS];
} KSWORD_ARK_DDMA_WRITE_PHYSICAL_RESPONSE;
