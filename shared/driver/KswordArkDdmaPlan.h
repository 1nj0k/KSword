#pragma once

#include "KswordArkDdmaIoctl.h"

// ============================================================
// KswordArkDdmaPlan.h
// 作用：
// - 把 DDMA 里三段"纯算术、错了后果最重"的逻辑抽成可被 C 与 C++ 共用的
//   static inline 函数：ATA 任务文件寄存器编码、物理区间校验、按页切片长度；
// - R0 驱动、R3 客户端与单元测试引用的是同一份实现，避免"测的和跑的不是
//   同一段代码"。
//
// 为什么单独抽出来：
// - LBA 到 ATA 寄存器的映射在 28 位与 48 位两种模式下布局完全不同，写错一位
//   就是往磁盘上另一个位置读写。这是本功能里唯一一个"错了会毁用户数据"的
//   纯计算，必须能被穷举测试覆盖；
// - 区间校验与切片长度决定了会不会越过页边界去动相邻物理页。
//
// 本文件只做算术，不碰任何内核对象、句柄或全局状态。
// ============================================================

// ATA 任务文件寄存器下标。顺序取自 IDE 寄存器定义，两种模式共用：
// [0]=Features [1]=SectorCount [2]=LBA Low [3]=LBA Mid [4]=LBA High
// [5]=Device/Head [6]=Command [7]=Reserved
#define KSWORD_ARK_ATA_TASKFILE_FEATURES 0
#define KSWORD_ARK_ATA_TASKFILE_SECTOR_COUNT 1
#define KSWORD_ARK_ATA_TASKFILE_LBA_LOW 2
#define KSWORD_ARK_ATA_TASKFILE_LBA_MID 3
#define KSWORD_ARK_ATA_TASKFILE_LBA_HIGH 4
#define KSWORD_ARK_ATA_TASKFILE_DEVICE 5
#define KSWORD_ARK_ATA_TASKFILE_COMMAND 6
#define KSWORD_ARK_ATA_TASKFILE_RESERVED 7

#define KSWORD_ARK_ATA_TASKFILE_BYTES 8

// ATA 命令码。28 位与 48 位是两套命令，寄存器布局也不同。
#define KSWORD_ARK_ATA_CMD_READ_SECTORS 0x20
#define KSWORD_ARK_ATA_CMD_WRITE_SECTORS 0x30
#define KSWORD_ARK_ATA_CMD_READ_SECTORS_EXT 0x24
#define KSWORD_ARK_ATA_CMD_WRITE_SECTORS_EXT 0x34

// Device/Head 寄存器的 LBA 模式位。
#define KSWORD_ARK_ATA_DEVICE_LBA 0x40

// ATA_FLAGS_48BIT_COMMAND 来自 ntddscsi.h。本头文件要能被不方便包含
// ntddscsi.h 的单元测试引用，所以在缺失时按同一取值补一份定义；两边同时存在
// 时取值一致，不会冲突。
#ifndef ATA_FLAGS_48BIT_COMMAND
#define ATA_FLAGS_48BIT_COMMAND (1 << 3)
#endif

// 28 位 LBA 的上限；达到或超过就必须换 48 位命令。
#define KSWORD_ARK_DDMA_LBA28_LIMIT 0x10000000ULL
// 48 位 LBA 的上限。
#define KSWORD_ARK_DDMA_LBA48_LIMIT 0x0001000000000000ULL

// x64 当前页表格式最多使用 52 位物理地址，与 memory_physical.c 同一上限。
#define KSWORD_ARK_DDMA_PHYSICAL_ADDRESS_MAX 0x000FFFFFFFFFFFFFULL

// 门禁判定结果。三种拒绝原因必须分开：它们对应三种完全不同的用户操作，
// 合并成一个笼统的"需要确认"会让界面给不出正确的下一步。
#define KSWORD_ARK_DDMA_GATE_ALLOWED 0
#define KSWORD_ARK_DDMA_GATE_NOT_CONFIGURED 1
#define KSWORD_ARK_DDMA_GATE_KERNEL_DEBUGGER 2
#define KSWORD_ARK_DDMA_GATE_SCRATCH_LBA_MISSING 3
#define KSWORD_ARK_DDMA_GATE_SCRATCH_NOT_ACKNOWLEDGED 4

// KSWORD_ARK_DDMA_TASKFILE：一条 ATA 命令的完整寄存器快照。
typedef struct _KSWORD_ARK_DDMA_TASKFILE
{
    unsigned char currentTaskFile[KSWORD_ARK_ATA_TASKFILE_BYTES];
    unsigned char previousTaskFile[KSWORD_ARK_ATA_TASKFILE_BYTES];
    unsigned short extraAtaFlags;  // 需要额外或上的 AtaFlags 位（48 位命令标志）。
    unsigned char usesLba48;       // 1 表示本次走 48 位命令。
    unsigned char valid;           // 0 表示参数被拒绝，其余字段无意义。
} KSWORD_ARK_DDMA_TASKFILE;

/*
 * KswordArkDdmaEncodeTaskFile：把 LBA、扇区数与读写方向编码成 ATA 任务文件。
 *
 * 输入：Lba 为起始扇区号；SectorCount 为本次传输扇区数（1..65536，28 位模式下
 * 不得超过 256）；IsWrite 非零表示写盘。
 *
 * 处理：LBA 小于 2^28 时用 28 位命令，LBA 的最高 4 位挤在 Device 寄存器低半
 * 字节里，扇区数 256 用 0 表示；否则切到 48 位命令，高 3 个字节与扇区数高字节
 * 改由 previousTaskFile 承载，此时 Device 寄存器不再携带任何 LBA 位。
 *
 * 返回：填好的寄存器快照；参数越界时 valid 为 0。
 */
static __inline KSWORD_ARK_DDMA_TASKFILE
KswordArkDdmaEncodeTaskFile(
    unsigned long long Lba,
    unsigned long SectorCount,
    int IsWrite
    )
{
    KSWORD_ARK_DDMA_TASKFILE taskFile;
    int index = 0;
    int useLba48 = 0;

    for (index = 0; index < KSWORD_ARK_ATA_TASKFILE_BYTES; ++index) {
        taskFile.currentTaskFile[index] = 0;
        taskFile.previousTaskFile[index] = 0;
    }
    taskFile.extraAtaFlags = 0;
    taskFile.usesLba48 = 0;
    taskFile.valid = 0;

    if (SectorCount == 0UL || SectorCount > 65536UL) {
        return taskFile;
    }
    if (Lba >= KSWORD_ARK_DDMA_LBA48_LIMIT) {
        return taskFile;
    }
    /* 区间不得跨过 48 位上限。 */
    if ((KSWORD_ARK_DDMA_LBA48_LIMIT - Lba) < (unsigned long long)SectorCount) {
        return taskFile;
    }

    useLba48 = (Lba >= KSWORD_ARK_DDMA_LBA28_LIMIT) || (SectorCount > 256UL);
    taskFile.usesLba48 = (unsigned char)(useLba48 ? 1 : 0);

    taskFile.currentTaskFile[KSWORD_ARK_ATA_TASKFILE_LBA_LOW] =
        (unsigned char)(Lba & 0xFFULL);
    taskFile.currentTaskFile[KSWORD_ARK_ATA_TASKFILE_LBA_MID] =
        (unsigned char)((Lba >> 8) & 0xFFULL);
    taskFile.currentTaskFile[KSWORD_ARK_ATA_TASKFILE_LBA_HIGH] =
        (unsigned char)((Lba >> 16) & 0xFFULL);

    if (useLba48) {
        taskFile.extraAtaFlags = (unsigned short)ATA_FLAGS_48BIT_COMMAND;
        /* 48 位模式下 Device 寄存器只保留 LBA 模式位。 */
        taskFile.currentTaskFile[KSWORD_ARK_ATA_TASKFILE_DEVICE] =
            (unsigned char)KSWORD_ARK_ATA_DEVICE_LBA;
        taskFile.previousTaskFile[KSWORD_ARK_ATA_TASKFILE_LBA_LOW] =
            (unsigned char)((Lba >> 24) & 0xFFULL);
        taskFile.previousTaskFile[KSWORD_ARK_ATA_TASKFILE_LBA_MID] =
            (unsigned char)((Lba >> 32) & 0xFFULL);
        taskFile.previousTaskFile[KSWORD_ARK_ATA_TASKFILE_LBA_HIGH] =
            (unsigned char)((Lba >> 40) & 0xFFULL);
        /* 48 位模式的扇区数是 16 位：低字节在 current，高字节在 previous。 */
        taskFile.currentTaskFile[KSWORD_ARK_ATA_TASKFILE_SECTOR_COUNT] =
            (unsigned char)(SectorCount & 0xFFUL);
        taskFile.previousTaskFile[KSWORD_ARK_ATA_TASKFILE_SECTOR_COUNT] =
            (unsigned char)((SectorCount >> 8) & 0xFFUL);
        taskFile.currentTaskFile[KSWORD_ARK_ATA_TASKFILE_COMMAND] = (unsigned char)(IsWrite
            ? KSWORD_ARK_ATA_CMD_WRITE_SECTORS_EXT
            : KSWORD_ARK_ATA_CMD_READ_SECTORS_EXT);
    }
    else {
        /* 28 位模式下 LBA 的最高 4 位挤在 Device 寄存器低半字节里。 */
        taskFile.currentTaskFile[KSWORD_ARK_ATA_TASKFILE_DEVICE] =
            (unsigned char)(KSWORD_ARK_ATA_DEVICE_LBA |
                            (unsigned char)((Lba >> 24) & 0x0FULL));
        /* 28 位模式的扇区数是 8 位，256 用 0 表示。 */
        taskFile.currentTaskFile[KSWORD_ARK_ATA_TASKFILE_SECTOR_COUNT] =
            (unsigned char)(SectorCount & 0xFFUL);
        taskFile.currentTaskFile[KSWORD_ARK_ATA_TASKFILE_COMMAND] = (unsigned char)(IsWrite
            ? KSWORD_ARK_ATA_CMD_WRITE_SECTORS
            : KSWORD_ARK_ATA_CMD_READ_SECTORS);
    }

    taskFile.valid = 1;
    return taskFile;
}

/*
 * KswordArkDdmaIsPhysicalRangeValid：校验一次 DDMA 传输的物理区间。
 *
 * 除了 52 位上限与回绕，DDMA 额外要求整个区间落在同一个传输页内——DMA 的
 * 粒度就是一页，跨页必须由调用方切片，否则会悄悄动到相邻物理页。
 *
 * 返回：非零表示可以接受。
 */
static __inline int
KswordArkDdmaIsPhysicalRangeValid(
    unsigned long long PhysicalAddress,
    unsigned long Length
    )
{
    unsigned long long endAddress = 0ULL;
    unsigned long long pageBase = 0ULL;

    if (Length == 0UL || Length > KSWORD_ARK_DDMA_TRANSFER_BYTES) {
        return 0;
    }
    if (PhysicalAddress > KSWORD_ARK_DDMA_PHYSICAL_ADDRESS_MAX) {
        return 0;
    }
    if ((unsigned long long)Length >
        (KSWORD_ARK_DDMA_PHYSICAL_ADDRESS_MAX - PhysicalAddress + 1ULL)) {
        return 0;
    }

    endAddress = PhysicalAddress + (unsigned long long)Length - 1ULL;
    if (endAddress < PhysicalAddress) {
        return 0;
    }

    pageBase = PhysicalAddress & ~((unsigned long long)KSWORD_ARK_DDMA_TRANSFER_BYTES - 1ULL);
    return (endAddress < (pageBase + (unsigned long long)KSWORD_ARK_DDMA_TRANSFER_BYTES)) ? 1 : 0;
}

/*
 * KswordArkDdmaChunkLength：算出从 Address 起、在不跨页的前提下本次能处理多少
 * 字节。调用方据此切片，循环推进。
 *
 * 返回：本次可处理的字节数；Remaining 为 0 时返回 0。
 */
static __inline unsigned long
KswordArkDdmaChunkLength(
    unsigned long long Address,
    unsigned long long Remaining
    )
{
    unsigned long long pageBase = 0ULL;
    unsigned long long pageEnd = 0ULL;
    unsigned long long available = 0ULL;

    if (Remaining == 0ULL) {
        return 0UL;
    }

    pageBase = Address & ~((unsigned long long)KSWORD_ARK_DDMA_TRANSFER_BYTES - 1ULL);
    pageEnd = pageBase + (unsigned long long)KSWORD_ARK_DDMA_TRANSFER_BYTES;
    available = pageEnd - Address;
    return (unsigned long)((Remaining < available) ? Remaining : available);
}

/*
 * KswordArkDdmaEvaluateGate：按固定顺序判定 DDMA 会话是否可用。
 *
 * 顺序本身是判据的一部分：内核调试必须排在"还没配好"之类的原因前面，因为它
 * 不是"用不了"而是"用了会蓝屏"，不能被一句"请先填写 LBA"盖过去。
 *
 * 输入：四个布尔状态位，均为非零表示成立。
 * 返回：KSWORD_ARK_DDMA_GATE_* 之一。
 */
static __inline int
KswordArkDdmaEvaluateGate(
    int Configured,
    int KernelDebuggerEnabled,
    int ScratchLbaValid,
    int ScratchAcknowledged
    )
{
    if (!Configured) {
        return KSWORD_ARK_DDMA_GATE_NOT_CONFIGURED;
    }
    if (KernelDebuggerEnabled) {
        return KSWORD_ARK_DDMA_GATE_KERNEL_DEBUGGER;
    }
    if (!ScratchLbaValid) {
        return KSWORD_ARK_DDMA_GATE_SCRATCH_LBA_MISSING;
    }
    if (!ScratchAcknowledged) {
        return KSWORD_ARK_DDMA_GATE_SCRATCH_NOT_ACKNOWLEDGED;
    }
    return KSWORD_ARK_DDMA_GATE_ALLOWED;
}
