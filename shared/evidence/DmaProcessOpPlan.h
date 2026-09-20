#pragma once

// 基于 DMA 的进程操作 —— 计划与校验层。
//
// 这一层回答三个问题，全部是纯算术，不做任何 I/O：
//   1. 这一页里有没有一段可以安全写入的空隙，在哪；
//   2. 一次写入具体要覆盖哪些字节、原来是什么（备份）；
//   3. 写完读回来的东西对不对。
//
// 为什么必须单独成层并被穷举测试：算错了**不会报错**。空隙找错就是把真代码覆盖
// 掉，目标进程当场死掉而且是我们弄死的；覆盖范围算错会越过页边界，而 DMA 的粒度
// 就是一页，越界那一页可能是任何东西。
//
// ============================================================
// 与 R-1 注入的本质差别（不是"换个后端"，必须让调用方看见）
// ============================================================
//
// R-1 注入（KSWORD_ARK_HVM_INJECT）把载荷放在**影子页**里，装一个执行视图：
// 执行走影子、读写看真页。真页从头到尾没被改过，任何扫描器读到的都是原样，
// 而且有一个明确的触发点。
//
// DMA 没有影子页，也没有执行视图和触发点。它只有一件事：往物理页里写字节。
// 于是：
//   * **真页真的被改了**。任何读取路径都看得见，包括我们自己的 R3/R0/HVM。
//   * **没有触发**。载荷躺在那里，要等目标自己执行到。写进一个永远不会被执行
//     的位置，等于什么都没做。
//   * **还原是调用方的责任**。所以本层强制产出备份字节，不产出备份就没法还原。
//
// ============================================================
// 关于 ud2 那条"结束"
// ============================================================
//
// DMA 调不了 PsTerminateProcess。往目标会执行到的代码里写 ud2（0F 0B）让它因
// 未处理异常退出，是 DMA 手上唯一不依赖内核结构偏移的办法。
//
// 它**不是一个可靠的 terminate**，本层的命名刻意不叫 Terminate：
//   * 目标可能带异常处理器（SEH / VEH）把 #UD 吞掉，于是它不死，只是行为变了；
//   * 生效与否取决于那段代码会不会被执行到，无法预先判断；
//   * 会留下崩溃转储与事件日志；
//   * 不可逆——除非调用方用备份自己写回去。
//
// C++20、Qt-free、Win32-free。

#include <cstdint>
#include <string>
#include <vector>

namespace Ksword::Evidence {

// DMA 的传输粒度就是一页，所有计划都必须落在一页之内。
inline constexpr std::uint64_t kDmaOpPageBytes = 4096ULL;

// 空隙最短长度。与 R-1 注入的 KSWORD_ARK_HVM_INJECT_MIN_CAVE_BYTES 取同一个值，
// 理由也相同：更短的"空隙"多半不是填充，而是真代码里恰好连续的相同字节，
// 写进去就是把目标打死。
inline constexpr std::size_t kDmaOpMinCaveBytes = 64U;

// DmaOpPlanStatus：一次计划的结果。
//
// 每一种拒绝都是独立的状态而不是统一的 false：调用方要据此告诉用户该改什么，
// "计划失败"这四个字对任何人都没有帮助。
enum class DmaOpPlanStatus : int {
    Ok = 0,
    EmptyPayload,          // 载荷长度为 0。
    PayloadTooLarge,       // 载荷超过一页，或超过找到的空隙。
    PageBytesUnavailable,  // 没有提供目标页的当前内容，无法备份也无法找空隙。
    PageBytesWrongSize,    // 提供的页内容长度不等于一页。
    OffsetOutOfPage,       // 指定的页内偏移落在页外。
    WouldCrossPage,        // 写入范围会越过页边界。
    NoCave,                // 页内找不到足够长的空隙。
};

const char* DmaOpPlanStatusName(DmaOpPlanStatus status) noexcept;

// DmaCodeCave：页内一段可写的空隙。
struct DmaCodeCave final {
    bool found = false;
    std::size_t offset = 0;   // 页内偏移。
    std::size_t length = 0;
    std::uint8_t fillByte = 0; // 构成这段空隙的填充字节（0x00 或 0xCC）。
};

// FindLargestCodeCave：
// - 在一页字节里找**最长**的一段由单一填充字节构成的连续区间；
// - 只认 0x00 与 0xCC 两种填充：它们是链接器与编译器实际产出的对齐填充。
//   任何其它重复字节都可能是真数据（例如一段全 0x41 的字符串），认了就会把
//   数据当空隙覆盖掉。
// - 长度不足 minLength 时返回 found = false，不返回"最长的那个"——让调用方
//   拿着一段不够长的空隙去写，与直接写真代码没有区别。
DmaCodeCave FindLargestCodeCave(
    const std::vector<std::uint8_t>& pageBytes,
    std::size_t minLength);

// DmaWritePlan：一次写入的完整计划。
struct DmaWritePlan final {
    DmaOpPlanStatus status = DmaOpPlanStatus::PageBytesUnavailable;
    std::size_t offsetInPage = 0;
    std::vector<std::uint8_t> bytesToWrite;
    // originalBytes：即将被覆盖的原始字节，**与 bytesToWrite 等长**。
    // 这是还原的唯一依据，所以它是结构的一部分而不是可选项：拿不到备份就
    // 不该发起这次写入。
    std::vector<std::uint8_t> originalBytes;
};

// PlanPayloadIntoCave：
// - 在页内找空隙并把载荷排进去；
// - 备份被覆盖的原始字节。
DmaWritePlan PlanPayloadIntoCave(
    const std::vector<std::uint8_t>& pageBytes,
    const std::vector<std::uint8_t>& payload,
    std::size_t minCaveBytes);

// PlanBytesAtOffset：
// - 在指定页内偏移处排一次写入，不找空隙；
// - 用于"往这个确切地址写 ud2"这类调用方已经知道位置的情况。
DmaWritePlan PlanBytesAtOffset(
    const std::vector<std::uint8_t>& pageBytes,
    std::size_t offsetInPage,
    const std::vector<std::uint8_t>& bytes);

// kUndefinedInstruction：x86 的 UD2，两字节，保证产生 #UD。
//
// 刻意不用 int3（0xCC）：那是断点，附了调试器的进程会停下来而不是退出，
// 而"目标停住了"与"目标退出了"在界面上会被读成同一个结果。
std::vector<std::uint8_t> UndefinedInstructionBytes();

// ============================================================
// 目标页是不是被别的进程共享
// ============================================================
//
// 这是本模块里后果最重的一条判据。DMA 写的是**物理页**，而写时复制靠缺页异常
// 实现——**DMA 不触发缺页**。于是往一张共享的映像页（任何 DLL 的代码页都是）
// 写字节，会打到**每一个映射了它的进程**，不只是目标。往 ntdll 的代码页写一条
// UD2，等于让全机器的进程在跑到那里时一起崩。
//
// 光看区域类型是**推测**而不是读数：一页 MEM_IMAGE 可能早就因为写时复制变成了
// 这个进程私有的副本，此时写它只影响目标；而一页 MEM_IMAGE 也可能仍然是那张
// 共享页。两者在 MEMORY_BASIC_INFORMATION 里长得一样。
//
// 唯一的读数是**跨进程比物理地址**：在另一个映射同一文件的进程里翻译同一个虚拟
// 地址，拿到的物理地址相同就是同一张页，也就是共享。
enum class DmaTargetSharing : int {
    // 区域是进程私有的，或跨进程比对证明物理地址不同（写时复制已经发生）。
    PrivateConfirmed = 0,
    // 跨进程比对证明另一个进程的同一虚拟地址落在同一张物理页上。
    SharedConfirmed,
    // 区域由节对象支撑，但没能找到第二个进程来比对。**不是"私有"**：
    // 没找到不等于不存在，此刻没有别的进程映射它也不代表下一刻没有。
    SharingUnknown,
};

const char* DmaTargetSharingName(DmaTargetSharing sharing) noexcept;

// EvaluateTargetSharing：
// - regionIsPrivate：目标区域的 type 是否为 MEM_PRIVATE；
// - comparisonPerformed：是否真的在另一个进程里翻译过同一个虚拟地址；
// - comparisonMatched：那个进程拿到的物理地址是否与目标相同。
//
// 三个输入刻意分开而不是合成一个 bool：没比对过与比对了但不同，是完全不同的
// 两件事，合并之后"没找到别的进程"会被读成"确认私有"。
DmaTargetSharing EvaluateTargetSharing(
    bool regionIsPrivate,
    bool comparisonPerformed,
    bool comparisonMatched) noexcept;

// DmaWriteVerification：写入之后读回来的比对结果。
struct DmaWriteVerification final {
    bool matched = false;
    std::size_t comparedBytes = 0;
    std::size_t firstMismatchOffset = 0;  // 相对于写入起点。
    std::uint8_t expectedByte = 0;
    std::uint8_t actualByte = 0;
    // readbackTooShort：读回的字节比写入的少。这与"内容不同"是两件事：
    // 前者说明读回通路本身有问题，后者说明写入没落地。
    bool readbackTooShort = false;
};

// VerifyWriteReadback：
// - 把读回的字节与计划写入的字节逐字节比对。
//
// 为什么这一步是强制的而不是可选的诊断：DMA 写入**没有任何自证**。驱动报
// writeStatus=OK 只说明命令被接受了，不说明目标物理页真的变了。一次静默没落地
// 的写入与一次成功的写入在界面上完全同形，而调用方接下来会以为载荷已经就位。
DmaWriteVerification VerifyWriteReadback(
    const std::vector<std::uint8_t>& intendedBytes,
    const std::vector<std::uint8_t>& readbackBytes);

}  // namespace Ksword::Evidence
