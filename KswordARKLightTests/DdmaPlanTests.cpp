// DDMA（磁盘直接内存访问，shared/driver/KswordArkDdmaPlan.h）的离线测试。
//
// 被测的四件事有一个共同点：**算错了不会报错，而且后果不可逆**。
//
//   * ATA 任务文件的 LBA 编码在 28 位与 48 位两种模式下布局完全不同。位算错了
//     不会有任何错误码——磁盘会老老实实地去读写另一个扇区。DDMA 每次操作都要
//     先把目标扇区备份下来、用完再写回去，所以编码错一位的实际后果是：备份读
//     的是 A 处，覆盖写的是 B 处，B 处的原始数据直接没了。
//   * 物理区间判据决定会不会越过页边界去动相邻物理页。DMA 的粒度就是一页，
//     判据放宽一个字节，写入就会溢到下一页，而那一页可能是任何东西。
//   * 切片长度算错会让循环要么漏掉尾部、要么把同一段重复写两遍。
//   * 门禁顺序本身是判据：内核调试那一条不是「用不了」而是「用了会蓝屏」，
//     被一句「请先填写 LBA」盖过去，用户就会照着提示去填 LBA，然后蓝屏。
//
// 这四件事在实机上都无法安全地试错，只能在编译机上被证明。断言原则与
// HvmWatchTests.cpp 一致：
//   * 期望值独立手算写死，绝不从被测函数反算；
//   * 边界两侧都测，只测一侧等于没测；
//   * 该被拒绝的输入必须被显式拒绝，不能只测通过路径。

#include "TestSupport.h"

#include "../shared/driver/KswordArkDdmaPlan.h"

#include <cstdint>

namespace {

// 一次 DDMA 传输固定 4096 字节 = 8 个 512 字节扇区。
constexpr unsigned long kSectors = KSWORD_ARK_DDMA_TRANSFER_BYTES / KSWORD_ARK_DDMA_SECTOR_SIZE;

// 寄存器下标的可读别名，避免测试里出现裸数字下标。
constexpr int kCount = KSWORD_ARK_ATA_TASKFILE_SECTOR_COUNT;
constexpr int kLow = KSWORD_ARK_ATA_TASKFILE_LBA_LOW;
constexpr int kMid = KSWORD_ARK_ATA_TASKFILE_LBA_MID;
constexpr int kHigh = KSWORD_ARK_ATA_TASKFILE_LBA_HIGH;
constexpr int kDevice = KSWORD_ARK_ATA_TASKFILE_DEVICE;
constexpr int kCommand = KSWORD_ARK_ATA_TASKFILE_COMMAND;

// ---------------------------------------------------------------------------
// 28 位 LBA 编码
// ---------------------------------------------------------------------------
void TestLba28Encoding(KswordTests::Suite& suite) {
    // LBA 0x0ABBCCDD 手算：
    //   LBA Low = 0xDD，LBA Mid = 0xCC，LBA High = 0xBB，
    //   Device = 0x40 | ((0x0ABBCCDD >> 24) & 0x0F) = 0x40 | 0x0A = 0x4A。
    const KSWORD_ARK_DDMA_TASKFILE read =
        KswordArkDdmaEncodeTaskFile(0x0ABBCCDDULL, kSectors, 0);
    suite.expect(read.valid == 1, L"ddma lba28: a representative read encodes");
    suite.expect(read.usesLba48 == 0, L"ddma lba28: stays in 28-bit mode");
    suite.expect(read.extraAtaFlags == 0, L"ddma lba28: adds no 48-bit ATA flag");
    suite.expect(read.currentTaskFile[kLow] == 0xDD, L"ddma lba28: LBA low byte");
    suite.expect(read.currentTaskFile[kMid] == 0xCC, L"ddma lba28: LBA mid byte");
    suite.expect(read.currentTaskFile[kHigh] == 0xBB, L"ddma lba28: LBA high byte");
    suite.expect(read.currentTaskFile[kDevice] == 0x4A,
        L"ddma lba28: device register carries LBA mode plus the top four LBA bits");
    suite.expect(read.currentTaskFile[kCount] == 8, L"ddma lba28: sector count is 8");
    suite.expect(read.currentTaskFile[kCommand] == KSWORD_ARK_ATA_CMD_READ_SECTORS,
        L"ddma lba28: read uses command 0x20");

    // previousTaskFile 在 28 位模式下必须整块保持为零：任何残留都会被硬件当成
    // 48 位模式的高位 LBA 读走。
    bool previousAllZero = true;
    for (int index = 0; index < KSWORD_ARK_ATA_TASKFILE_BYTES; ++index) {
        if (read.previousTaskFile[index] != 0) {
            previousAllZero = false;
        }
    }
    suite.expect(previousAllZero, L"ddma lba28: previous task file stays all zero");

    const KSWORD_ARK_DDMA_TASKFILE write =
        KswordArkDdmaEncodeTaskFile(0x0ABBCCDDULL, kSectors, 1);
    suite.expect(write.currentTaskFile[kCommand] == KSWORD_ARK_ATA_CMD_WRITE_SECTORS,
        L"ddma lba28: write uses command 0x30");
    suite.expect(write.currentTaskFile[kDevice] == 0x4A,
        L"ddma lba28: write keeps the same device register as read");

    // LBA 0：所有 LBA 字节为零，Device 只剩模式位。
    const KSWORD_ARK_DDMA_TASKFILE zero = KswordArkDdmaEncodeTaskFile(0ULL, kSectors, 0);
    suite.expect(zero.valid == 1, L"ddma lba28: LBA 0 is a legal value, not a sentinel");
    suite.expect(zero.currentTaskFile[kLow] == 0x00, L"ddma lba28: LBA 0 low byte");
    suite.expect(zero.currentTaskFile[kMid] == 0x00, L"ddma lba28: LBA 0 mid byte");
    suite.expect(zero.currentTaskFile[kHigh] == 0x00, L"ddma lba28: LBA 0 high byte");
    suite.expect(zero.currentTaskFile[kDevice] == 0x40,
        L"ddma lba28: LBA 0 device register is the bare LBA mode bit");
}

// ---------------------------------------------------------------------------
// 28/48 位模式的切换边界
// ---------------------------------------------------------------------------
void TestLbaModeBoundary(KswordTests::Suite& suite) {
    // 0x0FFFFFFF 是 28 位能表示的最大 LBA：Device = 0x40 | 0x0F = 0x4F。
    const KSWORD_ARK_DDMA_TASKFILE last28 =
        KswordArkDdmaEncodeTaskFile(0x0FFFFFFFULL, kSectors, 0);
    suite.expect(last28.usesLba48 == 0, L"ddma boundary: 0x0FFFFFFF is still 28-bit");
    suite.expect(last28.currentTaskFile[kDevice] == 0x4F,
        L"ddma boundary: 0x0FFFFFFF fills all four device LBA bits");
    suite.expect(last28.currentTaskFile[kCommand] == KSWORD_ARK_ATA_CMD_READ_SECTORS,
        L"ddma boundary: 0x0FFFFFFF uses the 28-bit read command");

    // 再加一就必须切到 48 位。这是整个编码里最容易写错的一格：边界两侧的命令
    // 码、Device 寄存器与 previousTaskFile 全都不一样。
    const KSWORD_ARK_DDMA_TASKFILE first48 =
        KswordArkDdmaEncodeTaskFile(0x10000000ULL, kSectors, 0);
    suite.expect(first48.usesLba48 == 1, L"ddma boundary: 0x10000000 switches to 48-bit");
    suite.expect(first48.extraAtaFlags == ATA_FLAGS_48BIT_COMMAND,
        L"ddma boundary: 48-bit mode sets the 48-bit ATA flag");
    suite.expect(first48.currentTaskFile[kCommand] == KSWORD_ARK_ATA_CMD_READ_SECTORS_EXT,
        L"ddma boundary: 48-bit read uses command 0x24");
    suite.expect(first48.currentTaskFile[kDevice] == 0x40,
        L"ddma boundary: 48-bit device register carries no LBA bits at all");
    suite.expect(first48.currentTaskFile[kLow] == 0x00,
        L"ddma boundary: 0x10000000 low byte");
    suite.expect(first48.currentTaskFile[kMid] == 0x00,
        L"ddma boundary: 0x10000000 mid byte");
    suite.expect(first48.currentTaskFile[kHigh] == 0x00,
        L"ddma boundary: 0x10000000 high byte");
    suite.expect(first48.previousTaskFile[kLow] == 0x10,
        L"ddma boundary: 0x10000000 spills 0x10 into the previous LBA low byte");
    suite.expect(first48.previousTaskFile[kMid] == 0x00,
        L"ddma boundary: 0x10000000 previous mid byte");
    suite.expect(first48.previousTaskFile[kHigh] == 0x00,
        L"ddma boundary: 0x10000000 previous high byte");

    const KSWORD_ARK_DDMA_TASKFILE write48 =
        KswordArkDdmaEncodeTaskFile(0x10000000ULL, kSectors, 1);
    suite.expect(write48.currentTaskFile[kCommand] == KSWORD_ARK_ATA_CMD_WRITE_SECTORS_EXT,
        L"ddma boundary: 48-bit write uses command 0x34");
}

// ---------------------------------------------------------------------------
// 48 位 LBA 编码与上限
// ---------------------------------------------------------------------------
void TestLba48Encoding(KswordTests::Suite& suite) {
    // LBA 0x0000FFFFFFFFFFF0 手算：低三字节 F0 FF FF，高三字节 FF FF FF。
    const KSWORD_ARK_DDMA_TASKFILE high =
        KswordArkDdmaEncodeTaskFile(0x0000FFFFFFFFFFF0ULL, kSectors, 0);
    suite.expect(high.valid == 1, L"ddma lba48: a near-maximum LBA encodes");
    suite.expect(high.usesLba48 == 1, L"ddma lba48: near-maximum LBA is 48-bit");
    suite.expect(high.currentTaskFile[kLow] == 0xF0, L"ddma lba48: current low byte");
    suite.expect(high.currentTaskFile[kMid] == 0xFF, L"ddma lba48: current mid byte");
    suite.expect(high.currentTaskFile[kHigh] == 0xFF, L"ddma lba48: current high byte");
    suite.expect(high.previousTaskFile[kLow] == 0xFF, L"ddma lba48: previous low byte");
    suite.expect(high.previousTaskFile[kMid] == 0xFF, L"ddma lba48: previous mid byte");
    suite.expect(high.previousTaskFile[kHigh] == 0xFF, L"ddma lba48: previous high byte");

    // 恰好顶到 48 位上限：LBA + 扇区数会越过 2^48，必须拒绝而不是回绕。
    // 0x0000FFFFFFFFFFFF + 8 > 0x0001000000000000。
    const KSWORD_ARK_DDMA_TASKFILE overflow =
        KswordArkDdmaEncodeTaskFile(0x0000FFFFFFFFFFFFULL, kSectors, 0);
    suite.expect(overflow.valid == 0,
        L"ddma lba48: a range running past 2^48 is rejected, not wrapped");

    // LBA 本身就超过 48 位上限。
    const KSWORD_ARK_DDMA_TASKFILE beyond =
        KswordArkDdmaEncodeTaskFile(KSWORD_ARK_DDMA_LBA48_LIMIT, 1UL, 0);
    suite.expect(beyond.valid == 0, L"ddma lba48: an LBA at the 2^48 limit is rejected");

    // 上限内的最后一个单扇区请求必须被接受，否则判据多拒了一格。
    const KSWORD_ARK_DDMA_TASKFILE lastSector =
        KswordArkDdmaEncodeTaskFile(KSWORD_ARK_DDMA_LBA48_LIMIT - 1ULL, 1UL, 0);
    suite.expect(lastSector.valid == 1,
        L"ddma lba48: the last single sector below 2^48 is accepted");
}

// ---------------------------------------------------------------------------
// 扇区数编码
// ---------------------------------------------------------------------------
void TestSectorCountEncoding(KswordTests::Suite& suite) {
    // 0 个扇区没有意义，必须拒绝——ATA 上 0 另有含义（256），放行等于静默地
    // 把一次空操作变成一次 256 扇区的操作。
    suite.expect(KswordArkDdmaEncodeTaskFile(0ULL, 0UL, 0).valid == 0,
        L"ddma count: zero sectors is rejected");

    // 28 位模式下扇区数是 8 位，256 按 ATA 约定用 0 表示。
    const KSWORD_ARK_DDMA_TASKFILE count256 = KswordArkDdmaEncodeTaskFile(0ULL, 256UL, 0);
    suite.expect(count256.valid == 1, L"ddma count: 256 sectors is legal");
    suite.expect(count256.usesLba48 == 0, L"ddma count: 256 sectors still fits 28-bit mode");
    suite.expect(count256.currentTaskFile[kCount] == 0x00,
        L"ddma count: 256 sectors is encoded as 0 in 28-bit mode");

    // 257 个扇区超出 8 位，必须靠 48 位模式的 16 位扇区数来表达，即使 LBA 很小。
    const KSWORD_ARK_DDMA_TASKFILE count257 = KswordArkDdmaEncodeTaskFile(0ULL, 257UL, 0);
    suite.expect(count257.valid == 1, L"ddma count: 257 sectors is legal");
    suite.expect(count257.usesLba48 == 1,
        L"ddma count: 257 sectors forces 48-bit mode even at LBA 0");
    suite.expect(count257.currentTaskFile[kCount] == 0x01,
        L"ddma count: 257 low byte is 0x01");
    suite.expect(count257.previousTaskFile[kCount] == 0x01,
        L"ddma count: 257 high byte is 0x01");

    // 65536 是 48 位模式的上限，同样按 0 表示。
    const KSWORD_ARK_DDMA_TASKFILE count65536 = KswordArkDdmaEncodeTaskFile(0ULL, 65536UL, 0);
    suite.expect(count65536.valid == 1, L"ddma count: 65536 sectors is legal");
    suite.expect(count65536.currentTaskFile[kCount] == 0x00,
        L"ddma count: 65536 low byte is 0");
    suite.expect(count65536.previousTaskFile[kCount] == 0x00,
        L"ddma count: 65536 high byte is 0");

    suite.expect(KswordArkDdmaEncodeTaskFile(0ULL, 65537UL, 0).valid == 0,
        L"ddma count: 65537 sectors is rejected");
}

// ---------------------------------------------------------------------------
// 物理区间判据
// ---------------------------------------------------------------------------
void TestPhysicalRange(KswordTests::Suite& suite) {
    constexpr unsigned long long kPage = KSWORD_ARK_DDMA_TRANSFER_BYTES;

    suite.expect(KswordArkDdmaIsPhysicalRangeValid(0x1000ULL, KSWORD_ARK_DDMA_TRANSFER_BYTES) == 1,
        L"ddma range: an aligned whole page is accepted");
    // 页内最后一个字节可以单独读，再多一个字节就跨页了。边界两侧都要测。
    suite.expect(KswordArkDdmaIsPhysicalRangeValid(0x1000ULL + kPage - 1ULL, 1UL) == 1,
        L"ddma range: the last byte of a page is accepted");
    suite.expect(KswordArkDdmaIsPhysicalRangeValid(0x1000ULL + kPage - 1ULL, 2UL) == 0,
        L"ddma range: two bytes straddling the page boundary are rejected");
    suite.expect(KswordArkDdmaIsPhysicalRangeValid(0x1001ULL, KSWORD_ARK_DDMA_TRANSFER_BYTES) == 0,
        L"ddma range: a full page starting one byte in is rejected");

    suite.expect(KswordArkDdmaIsPhysicalRangeValid(0x1000ULL, 0UL) == 0,
        L"ddma range: zero length is rejected");
    suite.expect(
        KswordArkDdmaIsPhysicalRangeValid(0x1000ULL, KSWORD_ARK_DDMA_TRANSFER_BYTES + 1UL) == 0,
        L"ddma range: longer than one transfer is rejected");

    // 52 位物理地址上限两侧。最后一页刚好顶到上限，必须被接受。
    constexpr unsigned long long kLastPage =
        (KSWORD_ARK_DDMA_PHYSICAL_ADDRESS_MAX + 1ULL) - KSWORD_ARK_DDMA_TRANSFER_BYTES;
    suite.expect(
        KswordArkDdmaIsPhysicalRangeValid(kLastPage, KSWORD_ARK_DDMA_TRANSFER_BYTES) == 1,
        L"ddma range: the last page below the 52-bit limit is accepted");
    suite.expect(
        KswordArkDdmaIsPhysicalRangeValid(KSWORD_ARK_DDMA_PHYSICAL_ADDRESS_MAX + 1ULL, 1UL) == 0,
        L"ddma range: one byte past the 52-bit limit is rejected");
    suite.expect(KswordArkDdmaIsPhysicalRangeValid(0ULL, 1UL) == 1,
        L"ddma range: physical address 0 is a legal target");
}

// ---------------------------------------------------------------------------
// 切片长度
// ---------------------------------------------------------------------------
void TestChunkLength(KswordTests::Suite& suite) {
    constexpr unsigned long long kPage = KSWORD_ARK_DDMA_TRANSFER_BYTES;

    suite.expect(KswordArkDdmaChunkLength(0x1000ULL, kPage * 2ULL) == kPage,
        L"ddma chunk: an aligned cursor takes a whole page");
    // 页中间起步只能取到页尾：0x2000 - 0x1800 = 0x800。
    suite.expect(KswordArkDdmaChunkLength(0x1800ULL, kPage * 2ULL) == 0x800UL,
        L"ddma chunk: an unaligned cursor stops at the page boundary");
    suite.expect(KswordArkDdmaChunkLength(0x1800ULL, 100ULL) == 100UL,
        L"ddma chunk: a short remainder is not padded up to the page boundary");
    suite.expect(KswordArkDdmaChunkLength(0x1FFFULL, 10ULL) == 1UL,
        L"ddma chunk: the last byte of a page yields exactly one byte");
    suite.expect(KswordArkDdmaChunkLength(0x1000ULL, 0ULL) == 0UL,
        L"ddma chunk: nothing remaining yields zero");

    // 走一遍完整循环：从非对齐地址跨三页，累计必须刚好等于请求长度，且每一步
    // 都不跨页。算错任何一步，这个不变式都会立刻破掉。
    unsigned long long cursor = 0x1800ULL;
    unsigned long long remaining = kPage * 2ULL + 0x100ULL;
    unsigned long long consumed = 0ULL;
    int steps = 0;
    bool everCrossedPage = false;
    while (remaining > 0ULL && steps < 16) {
        const unsigned long chunk = KswordArkDdmaChunkLength(cursor, remaining);
        if (chunk == 0UL) {
            break;
        }
        const unsigned long long pageBase = cursor & ~(kPage - 1ULL);
        if ((cursor + chunk - 1ULL) >= (pageBase + kPage)) {
            everCrossedPage = true;
        }
        cursor += chunk;
        remaining -= chunk;
        consumed += chunk;
        ++steps;
    }
    suite.expect(remaining == 0ULL, L"ddma chunk: the walk consumes the whole range");
    suite.expect(consumed == kPage * 2ULL + 0x100ULL,
        L"ddma chunk: the walk consumes exactly the requested byte count");
    suite.expect(!everCrossedPage, L"ddma chunk: no single step ever crosses a page boundary");
    // 手算步数：0x1800 起先补到页尾拿 0x800(2048)，再整页 0x1000(4096)，
    // 剩下 8448-2048-4096 = 2304 一次取完，合计三步。
    suite.expect(steps == 3, L"ddma chunk: 0x1800 plus 0x2100 bytes takes exactly three steps");
}

// ---------------------------------------------------------------------------
// 门禁顺序
// ---------------------------------------------------------------------------
void TestGateOrder(KswordTests::Suite& suite) {
    // 全部就绪才放行。
    suite.expect(KswordArkDdmaEvaluateGate(1, 0, 1, 1) == KSWORD_ARK_DDMA_GATE_ALLOWED,
        L"ddma gate: a fully prepared session is allowed");

    // 未配置优先于一切：还没探测过磁盘时，其它状态都无从谈起。
    suite.expect(KswordArkDdmaEvaluateGate(0, 0, 1, 1) == KSWORD_ARK_DDMA_GATE_NOT_CONFIGURED,
        L"ddma gate: an unconfigured session reports not-configured");
    suite.expect(KswordArkDdmaEvaluateGate(0, 1, 0, 0) == KSWORD_ARK_DDMA_GATE_NOT_CONFIGURED,
        L"ddma gate: not-configured outranks every later reason");

    // 内核调试必须排在 LBA 与确认之前。这条不是「还没配好」，是「配好了也会
    // 蓝屏」；被后面两条盖住，用户会照着提示去补配置，然后撞上蓝屏。
    suite.expect(KswordArkDdmaEvaluateGate(1, 1, 1, 1) == KSWORD_ARK_DDMA_GATE_KERNEL_DEBUGGER,
        L"ddma gate: kernel debugging blocks an otherwise complete session");
    suite.expect(KswordArkDdmaEvaluateGate(1, 1, 0, 0) == KSWORD_ARK_DDMA_GATE_KERNEL_DEBUGGER,
        L"ddma gate: kernel debugging outranks a missing scratch LBA");
    suite.expect(KswordArkDdmaEvaluateGate(1, 1, 1, 0) == KSWORD_ARK_DDMA_GATE_KERNEL_DEBUGGER,
        L"ddma gate: kernel debugging outranks a missing acknowledgement");

    // LBA 缺失排在确认之前：先有地址，再谈确认这个地址可被覆盖。
    suite.expect(
        KswordArkDdmaEvaluateGate(1, 0, 0, 1) == KSWORD_ARK_DDMA_GATE_SCRATCH_LBA_MISSING,
        L"ddma gate: a missing scratch LBA is reported even when acknowledged");
    suite.expect(
        KswordArkDdmaEvaluateGate(1, 0, 0, 0) == KSWORD_ARK_DDMA_GATE_SCRATCH_LBA_MISSING,
        L"ddma gate: a missing scratch LBA outranks a missing acknowledgement");
    suite.expect(
        KswordArkDdmaEvaluateGate(1, 0, 1, 0) == KSWORD_ARK_DDMA_GATE_SCRATCH_NOT_ACKNOWLEDGED,
        L"ddma gate: an unacknowledged scratch sector is the last blocker");

    // 穷举全部 16 种组合：只有唯一一种被放行，其余都必须给出拒绝原因。
    int allowedCount = 0;
    bool everUnclassified = false;
    for (int mask = 0; mask < 16; ++mask) {
        const int configured = (mask & 1) ? 1 : 0;
        const int debugger = (mask & 2) ? 1 : 0;
        const int lba = (mask & 4) ? 1 : 0;
        const int ack = (mask & 8) ? 1 : 0;
        const int gate = KswordArkDdmaEvaluateGate(configured, debugger, lba, ack);
        if (gate == KSWORD_ARK_DDMA_GATE_ALLOWED) {
            ++allowedCount;
            // 放行只能发生在「已配置 + 没开内核调试 + 有 LBA + 已确认」。
            if (!(configured == 1 && debugger == 0 && lba == 1 && ack == 1)) {
                everUnclassified = true;
            }
        }
        else if (gate < KSWORD_ARK_DDMA_GATE_NOT_CONFIGURED ||
                 gate > KSWORD_ARK_DDMA_GATE_SCRATCH_NOT_ACKNOWLEDGED) {
            everUnclassified = true;
        }
    }
    suite.expect(allowedCount == 1,
        L"ddma gate: exactly one of the sixteen state combinations is allowed");
    suite.expect(!everUnclassified,
        L"ddma gate: every rejected combination carries a classified reason");
}

// ---------------------------------------------------------------------------
// flags 位段与允许掩码
// ---------------------------------------------------------------------------
//
// 这一组断言是一次实机故障换来的。原先 QUERY_FLAG_PROBE_TRANSFER 取 0x1，
// 与通用的 UI_CONFIRMED 撞同一位；而 QUERY_FLAG_ALLOWED 又漏掉了后端判定
// probeRequested 时要读的 SCRATCH_LBA_VALID。两个错误叠在一起的效果是：
// 传输探测这条路**永远走不到**——请求在 handler 的 flags 校验上就被拒成
// win32=87，后端那段判定一次都没执行过。
//
// 编译、离线单测、代码走读都抓不到它：位值是合法常量，掩码是合法表达式，
// 只有把两者放在一起对照才看得出矛盾。所以判据必须写成断言。
void TestFlagLayout(KswordTests::Suite& suite) {
    // 通用位段与查询专用位段必须完全不相交。
    constexpr unsigned long commonFlags =
        KSWORD_ARK_DDMA_FLAG_UI_CONFIRMED |
        KSWORD_ARK_DDMA_FLAG_FORCE |
        KSWORD_ARK_DDMA_FLAG_SCRATCH_LBA_VALID |
        KSWORD_ARK_DDMA_FLAG_SCRATCH_ACKNOWLEDGED;
    suite.expect((commonFlags & KSWORD_ARK_DDMA_QUERY_FLAG_PROBE_TRANSFER) == 0UL,
        L"ddma flags: the query-only flag does not collide with any common flag");

    // 四个通用位两两不同。
    suite.expect(KSWORD_ARK_DDMA_FLAG_UI_CONFIRMED != KSWORD_ARK_DDMA_FLAG_FORCE,
        L"ddma flags: UI_CONFIRMED and FORCE are distinct bits");
    suite.expect(KSWORD_ARK_DDMA_FLAG_SCRATCH_LBA_VALID !=
                 KSWORD_ARK_DDMA_FLAG_SCRATCH_ACKNOWLEDGED,
        L"ddma flags: SCRATCH_LBA_VALID and SCRATCH_ACKNOWLEDGED are distinct bits");
    suite.expect((KSWORD_ARK_DDMA_FLAG_UI_CONFIRMED |
                  KSWORD_ARK_DDMA_FLAG_FORCE |
                  KSWORD_ARK_DDMA_FLAG_SCRATCH_LBA_VALID |
                  KSWORD_ARK_DDMA_FLAG_SCRATCH_ACKNOWLEDGED) == 0x0000000FUL,
        L"ddma flags: the four common bits occupy exactly the low nibble");

    // 允许掩码必须是"这条路径上后端真正会读的位"的超集。
    // 查询：后端读 PROBE_TRANSFER 与 SCRATCH_LBA_VALID。
    suite.expect(
        (KSWORD_ARK_DDMA_QUERY_FLAG_ALLOWED & KSWORD_ARK_DDMA_QUERY_FLAG_PROBE_TRANSFER) != 0UL,
        L"ddma flags: query allow-mask permits PROBE_TRANSFER");
    suite.expect(
        (KSWORD_ARK_DDMA_QUERY_FLAG_ALLOWED & KSWORD_ARK_DDMA_FLAG_SCRATCH_LBA_VALID) != 0UL,
        L"ddma flags: query allow-mask permits SCRATCH_LBA_VALID, so the transfer probe is reachable");

    // 读：后端读 SCRATCH_LBA_VALID 与 SCRATCH_ACKNOWLEDGED。
    suite.expect(
        (KSWORD_ARK_DDMA_READ_FLAG_ALLOWED & KSWORD_ARK_DDMA_FLAG_SCRATCH_LBA_VALID) != 0UL,
        L"ddma flags: read allow-mask permits SCRATCH_LBA_VALID");
    suite.expect(
        (KSWORD_ARK_DDMA_READ_FLAG_ALLOWED & KSWORD_ARK_DDMA_FLAG_SCRATCH_ACKNOWLEDGED) != 0UL,
        L"ddma flags: read allow-mask permits SCRATCH_ACKNOWLEDGED");
    // 读路径不该放行 FORCE：读没有强制语义，放行它等于给一个无人解释的位。
    suite.expect((KSWORD_ARK_DDMA_READ_FLAG_ALLOWED & KSWORD_ARK_DDMA_FLAG_FORCE) == 0UL,
        L"ddma flags: read allow-mask does not permit FORCE");

    // 写：读的全部加上 FORCE。
    suite.expect(
        (KSWORD_ARK_DDMA_WRITE_FLAG_ALLOWED & KSWORD_ARK_DDMA_FLAG_FORCE) != 0UL,
        L"ddma flags: write allow-mask permits FORCE");
    suite.expect(
        (KSWORD_ARK_DDMA_WRITE_FLAG_ALLOWED & KSWORD_ARK_DDMA_READ_FLAG_ALLOWED) ==
            KSWORD_ARK_DDMA_READ_FLAG_ALLOWED,
        L"ddma flags: write allow-mask is a superset of the read allow-mask");

    // 门禁状态码两两不同。三条拒绝原因对应三种完全不同的用户操作，
    // 任意两个撞在一起，界面就给不出正确的下一步。
    suite.expect(
        KSWORD_ARK_DDMA_READ_STATUS_SCRATCH_LBA_REQUIRED !=
            KSWORD_ARK_DDMA_READ_STATUS_SCRATCH_NOT_ACKNOWLEDGED,
        L"ddma status: read LBA-required and not-acknowledged are distinct");
    suite.expect(
        KSWORD_ARK_DDMA_WRITE_STATUS_FORCE_REQUIRED !=
            KSWORD_ARK_DDMA_WRITE_STATUS_SCRATCH_NOT_ACKNOWLEDGED,
        L"ddma status: write force-required and not-acknowledged are distinct");
    suite.expect(
        KSWORD_ARK_DDMA_WRITE_STATUS_FORCE_REQUIRED !=
            KSWORD_ARK_DDMA_WRITE_STATUS_SCRATCH_LBA_REQUIRED,
        L"ddma status: write force-required and LBA-required are distinct");
}

} // namespace

int RunDdmaPlanTests() {
    KswordTests::Suite suite(L"DDMA plan");
    TestFlagLayout(suite);
    TestLba28Encoding(suite);
    TestLbaModeBoundary(suite);
    TestLba48Encoding(suite);
    TestSectorCountEncoding(suite);
    TestPhysicalRange(suite);
    TestChunkLength(suite);
    TestGateOrder(suite);
    suite.report();
    return suite.failures();
}
