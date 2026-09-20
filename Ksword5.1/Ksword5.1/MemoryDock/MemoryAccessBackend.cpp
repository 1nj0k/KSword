#include "MemoryAccessBackend.h"

#include "../ArkDriverClient/ArkDriverClient.h"
// 切片长度与门禁顺序都取共用纯函数，和 R0 用的是同一份实现，也是单元测试
// 覆盖的那一份。
#include "../../../shared/driver/KswordArkDdmaPlan.h"

#include <algorithm>
#include <vector>

// ============================================================
// MemoryAccessBackend.cpp
// 作用：
// - 实现标准驱动通道与 DDMA 通道的统一读写门面；
// - 所有"DDMA 能不能用""怎么切片""失败怎么说"的判据只在本文件里有一份。
// ============================================================

namespace ksword::memory_backend
{
    namespace
    {
        // DDMA 的一次 DMA 传输长度，与 R0 协议常量一致。
        constexpr std::uint64_t kDdmaPageBytes =
            static_cast<std::uint64_t>(KSWORD_ARK_DDMA_TRANSFER_BYTES);

        // 标准通道的单次上限，直接取协议常量，避免在 UI 里再写一遍魔数。
        constexpr std::uint64_t kStandardPhysicalReadMax =
            static_cast<std::uint64_t>(KSWORD_ARK_MEMORY_PHYSICAL_READ_MAX_BYTES);
        constexpr std::uint64_t kStandardPhysicalWriteMax =
            static_cast<std::uint64_t>(KSWORD_ARK_MEMORY_PHYSICAL_WRITE_MAX_BYTES);
        constexpr std::uint64_t kStandardVirtualReadMax =
            static_cast<std::uint64_t>(KSWORD_ARK_MEMORY_READ_MAX_BYTES);
        constexpr std::uint64_t kStandardVirtualWriteMax =
            static_cast<std::uint64_t>(KSWORD_ARK_MEMORY_WRITE_MAX_BYTES);

        // x64 内核高半区起点。规范形式的地址要么低于 0x0000800000000000，
        // 要么不低于 0xFFFF800000000000，中间是不可用的空洞。
        constexpr std::uint64_t kKernelSpaceStart = 0xFFFF800000000000ULL;

        // ddmaFlagsForRead / ddmaFlagsForWrite：
        // 把"会话已确认"翻译成协议 flags。LBA_VALID 与 ACKNOWLEDGED 必须同时
        // 带上，缺任何一个 R0 都会返回各自独立的状态码而不是笼统拒绝。
        unsigned long ddmaFlagsForRead()
        {
            return KSWORD_ARK_DDMA_FLAG_UI_CONFIRMED |
                KSWORD_ARK_DDMA_FLAG_SCRATCH_LBA_VALID |
                KSWORD_ARK_DDMA_FLAG_SCRATCH_ACKNOWLEDGED;
        }

        unsigned long ddmaFlagsForWrite(const bool forceApproved)
        {
            unsigned long flags = ddmaFlagsForRead();
            if (forceApproved)
            {
                flags |= KSWORD_ARK_DDMA_FLAG_FORCE;
            }
            return flags;
        }

        // describeDdmaReadFailure：把 R0 读状态翻译成用户能据此行动的文案。
        QString describeDdmaReadFailure(const ksword::ark::DdmaReadResult& result)
        {
            switch (result.readStatus)
            {
            case KSWORD_ARK_DDMA_READ_STATUS_SCRATCH_LBA_REQUIRED:
                return QStringLiteral("驱动拒绝：必须显式指定暂存扇区 LBA。");
            case KSWORD_ARK_DDMA_READ_STATUS_SCRATCH_NOT_ACKNOWLEDGED:
                return QStringLiteral("驱动拒绝：尚未确认暂存扇区可被临时覆盖。");
            case KSWORD_ARK_DDMA_READ_STATUS_DISK_NOT_FOUND:
                return QStringLiteral("目标磁盘已不在设备列表中，请重新探测 DDMA 通道。");
            case KSWORD_ARK_DDMA_READ_STATUS_RANGE_REJECTED:
                return QStringLiteral("物理区间被驱动拒绝：长度为 0、超过一页或跨页。");
            case KSWORD_ARK_DDMA_READ_STATUS_MAP_FAILED:
                return QStringLiteral("映射目标物理页失败，该物理地址可能不存在。");
            case KSWORD_ARK_DDMA_READ_STATUS_BACKUP_FAILED:
                return QStringLiteral(
                    "备份暂存扇区失败，本次未对磁盘做任何写入。请确认该 LBA 在磁盘容量范围内。");
            case KSWORD_ARK_DDMA_READ_STATUS_STAGE_OUT_FAILED:
                return QStringLiteral("把目标物理页 DMA 写到暂存扇区失败。");
            case KSWORD_ARK_DDMA_READ_STATUS_STAGE_IN_FAILED:
                return QStringLiteral("从暂存扇区 DMA 读回失败。");
            case KSWORD_ARK_DDMA_READ_STATUS_IRQL_REJECTED:
                return QStringLiteral("驱动在非 PASSIVE_LEVEL 下拒绝了本次请求。");
            case KSWORD_ARK_DDMA_READ_STATUS_BUFFER_TOO_SMALL:
                return QStringLiteral("响应缓冲不足，这是客户端缺陷，请上报。");
            default:
                break;
            }
            return QStringLiteral("DDMA 读取失败，readStatus=%1。").arg(result.readStatus);
        }

        // describeDdmaWriteFailure：写路径的状态文案，与读路径分开是因为
        // FORCE_REQUIRED 只在写路径出现，且需要引导用户去点确认而不是改配置。
        QString describeDdmaWriteFailure(const ksword::ark::DdmaWriteResult& result)
        {
            switch (result.writeStatus)
            {
            case KSWORD_ARK_DDMA_WRITE_STATUS_SCRATCH_LBA_REQUIRED:
                return QStringLiteral("驱动拒绝：必须显式指定暂存扇区 LBA。");
            case KSWORD_ARK_DDMA_WRITE_STATUS_SCRATCH_NOT_ACKNOWLEDGED:
                return QStringLiteral("驱动拒绝：尚未确认暂存扇区可被临时覆盖。");
            case KSWORD_ARK_DDMA_WRITE_STATUS_FORCE_REQUIRED:
                return QStringLiteral("驱动要求对 DDMA 写入附加强制标志。");
            case KSWORD_ARK_DDMA_WRITE_STATUS_DISK_NOT_FOUND:
                return QStringLiteral("目标磁盘已不在设备列表中，请重新探测 DDMA 通道。");
            case KSWORD_ARK_DDMA_WRITE_STATUS_RANGE_REJECTED:
                return QStringLiteral("物理区间被驱动拒绝：长度为 0、超过一页或跨页。");
            case KSWORD_ARK_DDMA_WRITE_STATUS_MAP_FAILED:
                return QStringLiteral("映射目标物理页失败，该物理地址可能不存在。");
            case KSWORD_ARK_DDMA_WRITE_STATUS_BACKUP_FAILED:
                return QStringLiteral(
                    "备份暂存扇区失败，本次未对磁盘或物理内存做任何写入。");
            case KSWORD_ARK_DDMA_WRITE_STATUS_READBACK_FAILED:
                return QStringLiteral(
                    "读-改-写的读回阶段失败，目标物理页未被修改。");
            case KSWORD_ARK_DDMA_WRITE_STATUS_STAGE_OUT_FAILED:
                return QStringLiteral("把数据 DMA 写到暂存扇区失败，目标物理页未被修改。");
            case KSWORD_ARK_DDMA_WRITE_STATUS_STAGE_IN_FAILED:
                return QStringLiteral(
                    "从暂存扇区 DMA 写入目标物理页失败，该页可能处于半写状态。");
            case KSWORD_ARK_DDMA_WRITE_STATUS_ACCESS_DENIED:
                return QStringLiteral("驱动安全策略拒绝了本次 DDMA 写入。");
            case KSWORD_ARK_DDMA_WRITE_STATUS_IRQL_REJECTED:
                return QStringLiteral("驱动在非 PASSIVE_LEVEL 下拒绝了本次请求。");
            default:
                break;
            }
            return QStringLiteral("DDMA 写入失败，writeStatus=%1。").arg(result.writeStatus);
        }

        // formatHex：统一的 0x 十六进制文案，避免各处大小写与位宽不一致。
        QString formatHex(const std::uint64_t value)
        {
            return QStringLiteral("0x%1").arg(value, 0, 16).toUpper().replace(
                QStringLiteral("0X"), QStringLiteral("0x"));
        }

        // ddmaReadOnePage：
        // - 读取一个物理页内的一段（调用方保证不跨页）；
        // - 把 R0 的多段状态压成 AccessOutcome 的少数几个布尔。
        AccessOutcome ddmaReadOnePage(
            const ksword::ark::DriverClient& client,
            const DdmaSession& session,
            const std::uint64_t physicalAddress,
            const std::uint32_t lengthBytes)
        {
            AccessOutcome outcome;
            const ksword::ark::DdmaReadResult result = client.ddmaReadPhysicalMemory(
                session.diskIndex,
                physicalAddress,
                lengthBytes,
                session.scratchLba,
                ddmaFlagsForRead());

            // 暂存扇区是否还原要先取：即使读取本身失败也必须把脏扇区报出去。
            outcome.scratchDirty = !result.scratchRestored() &&
                (result.readStatus != KSWORD_ARK_DDMA_READ_STATUS_SCRATCH_LBA_REQUIRED) &&
                (result.readStatus != KSWORD_ARK_DDMA_READ_STATUS_SCRATCH_NOT_ACKNOWLEDGED) &&
                (result.readStatus != KSWORD_ARK_DDMA_READ_STATUS_BACKUP_FAILED) &&
                (result.readStatus != KSWORD_ARK_DDMA_READ_STATUS_UNAVAILABLE);

            if (!result.io.ok)
            {
                outcome.failureText = result.unsupported
                    ? QStringLiteral("当前驱动不支持 DDMA，请更新 KswordARK 驱动。")
                    : QStringLiteral("DDMA 读取通信失败：%1")
                          .arg(QString::fromStdString(result.io.message));
                return outcome;
            }
            if (result.readStatus != KSWORD_ARK_DDMA_READ_STATUS_OK ||
                result.data.size() != static_cast<std::size_t>(lengthBytes))
            {
                outcome.failureText = describeDdmaReadFailure(result);
                return outcome;
            }

            outcome.ok = true;
            outcome.bytesDone = lengthBytes;
            outcome.data = QByteArray(
                reinterpret_cast<const char*>(result.data.data()),
                static_cast<qsizetype>(result.data.size()));
            return outcome;
        }

        // ddmaWriteOnePage：写入一个物理页内的一段（调用方保证不跨页）。
        AccessOutcome ddmaWriteOnePage(
            const ksword::ark::DriverClient& client,
            const DdmaSession& session,
            const std::uint64_t physicalAddress,
            const QByteArray& chunk,
            const bool forceApproved)
        {
            AccessOutcome outcome;
            const std::vector<std::uint8_t> payload(
                reinterpret_cast<const std::uint8_t*>(chunk.constData()),
                reinterpret_cast<const std::uint8_t*>(chunk.constData()) + chunk.size());

            const ksword::ark::DdmaWriteResult result = client.ddmaWritePhysicalMemory(
                session.diskIndex,
                physicalAddress,
                payload,
                session.scratchLba,
                ddmaFlagsForWrite(forceApproved));

            outcome.scratchDirty = !result.scratchRestored() &&
                (result.writeStatus != KSWORD_ARK_DDMA_WRITE_STATUS_SCRATCH_LBA_REQUIRED) &&
                (result.writeStatus != KSWORD_ARK_DDMA_WRITE_STATUS_SCRATCH_NOT_ACKNOWLEDGED) &&
                (result.writeStatus != KSWORD_ARK_DDMA_WRITE_STATUS_FORCE_REQUIRED) &&
                (result.writeStatus != KSWORD_ARK_DDMA_WRITE_STATUS_BACKUP_FAILED) &&
                (result.writeStatus != KSWORD_ARK_DDMA_WRITE_STATUS_UNAVAILABLE);
            outcome.lostUpdateWindow = result.readModifyWriteUsed();

            if (!result.io.ok)
            {
                outcome.failureText = result.unsupported
                    ? QStringLiteral("当前驱动不支持 DDMA，请更新 KswordARK 驱动。")
                    : QStringLiteral("DDMA 写入通信失败：%1")
                          .arg(QString::fromStdString(result.io.message));
                return outcome;
            }
            if (result.writeStatus == KSWORD_ARK_DDMA_WRITE_STATUS_FORCE_REQUIRED)
            {
                // 这一条要与其它失败区分开：调用方应当弹确认后带 force 重试，
                // 而不是让用户回去改 DDMA 配置。
                outcome.forceRequired = true;
                outcome.failureText = describeDdmaWriteFailure(result);
                return outcome;
            }
            if (result.writeStatus != KSWORD_ARK_DDMA_WRITE_STATUS_OK ||
                result.bytesWritten != static_cast<std::uint32_t>(chunk.size()))
            {
                outcome.failureText = describeDdmaWriteFailure(result);
                return outcome;
            }

            outcome.ok = true;
            outcome.bytesDone = static_cast<std::uint64_t>(chunk.size());
            return outcome;
        }

        // mergeChunkOutcome：把逐页结果里的告警位合并进总结果。
        // 告警位是"只要出现过一次就要保留"的语义，不能被后续成功页覆盖掉。
        void mergeChunkOutcome(AccessOutcome& total, const AccessOutcome& chunk)
        {
            total.scratchDirty = total.scratchDirty || chunk.scratchDirty;
            total.lostUpdateWindow = total.lostUpdateWindow || chunk.lostUpdateWindow;
            total.partial = total.partial || chunk.partial;
        }
    }

    namespace
    {
        // 进程级 DDMA 会话。写入只发生在 UI 线程（DDMA 子页），读取也全部在 UI
        // 线程，唯一的例外是内存搜索的后台 worker——但它在启动前就把会话按值
        // 复制走了（见 MemoryDock.SearchFlow.cpp），不会读这里的活对象。
        DdmaSession& mutableCurrentDdmaSession()
        {
            static DdmaSession session;
            return session;
        }

        std::uint64_t& mutableDdmaSessionGeneration()
        {
            static std::uint64_t generation = 0ULL;
            return generation;
        }
    }

    const DdmaSession& currentDdmaSession()
    {
        return mutableCurrentDdmaSession();
    }

    void setCurrentDdmaSession(const DdmaSession& session)
    {
        mutableCurrentDdmaSession() = session;
        ++mutableDdmaSessionGeneration();
    }

    std::uint64_t ddmaSessionGeneration()
    {
        return mutableDdmaSessionGeneration();
    }

    std::uint32_t ddmaTransferBytes()
    {
        return static_cast<std::uint32_t>(KSWORD_ARK_DDMA_TRANSFER_BYTES);
    }

    QString backendDisplayName(const MemoryAccessBackend backend)
    {
        if (backend == MemoryAccessBackend::Ddma)
        {
            return QStringLiteral("DDMA（磁盘 DMA）");
        }
        return QStringLiteral("标准驱动通道");
    }

    bool isKernelVirtualAddress(const std::uint64_t virtualAddress)
    {
        return virtualAddress >= kKernelSpaceStart;
    }

    bool isDdmaUsable(const DdmaSession& session, QString* const reasonOut)
    {
        // 判定顺序本身是判据的一部分（内核调试必须排在"还没配好"前面），
        // 因此顺序由共用纯函数决定，本函数只负责把结论翻成可展示的文案。
        const int gate = KswordArkDdmaEvaluateGate(
            session.configured ? 1 : 0,
            session.kernelDebuggerEnabled ? 1 : 0,
            session.scratchLbaValid ? 1 : 0,
            session.scratchAcknowledged ? 1 : 0);

        QString reason;
        switch (gate)
        {
        case KSWORD_ARK_DDMA_GATE_ALLOWED:
            if (reasonOut != nullptr)
            {
                reasonOut->clear();
            }
            return true;
        case KSWORD_ARK_DDMA_GATE_NOT_CONFIGURED:
            reason = QStringLiteral(
                "尚未配置 DDMA 通道。请先到“内存 → DDMA”页探测磁盘并指定暂存扇区。");
            break;
        case KSWORD_ARK_DDMA_GATE_KERNEL_DEBUGGER:
            // 这一条不是"用不了"，是"用了会蓝屏"：DDMA 用 MmMapIoSpace 映射
            // 普通 RAM，开着内核调试时会命中 MiShowBadMapper。
            reason = QStringLiteral(
                "本机启用了内核调试。DDMA 需要映射普通物理页，这种机器上会命中 "
                "MiShowBadMapper 直接蓝屏，因此禁止使用。请关闭内核调试后重试。");
            break;
        case KSWORD_ARK_DDMA_GATE_SCRATCH_LBA_MISSING:
            reason = QStringLiteral(
                "尚未指定暂存扇区 LBA。DDMA 必须借用磁盘扇区中转，本工具不提供默认值。");
            break;
        case KSWORD_ARK_DDMA_GATE_SCRATCH_NOT_ACKNOWLEDGED:
        default:
            reason = QStringLiteral(
                "尚未确认暂存扇区可被临时覆盖。请在 DDMA 页勾选确认后再使用。");
            break;
        }

        if (reasonOut != nullptr)
        {
            *reasonOut = reason;
        }
        return false;
    }

    AccessOutcome readPhysical(
        const MemoryAccessBackend backend,
        const DdmaSession& session,
        const std::uint64_t physicalAddress,
        const std::uint64_t lengthBytes)
    {
        AccessOutcome outcome;
        if (lengthBytes == 0ULL)
        {
            outcome.failureText = QStringLiteral("读取长度为 0。");
            return outcome;
        }

        const ksword::ark::DriverClient client;

        if (backend == MemoryAccessBackend::StandardDriver)
        {
            if (lengthBytes > kStandardPhysicalReadMax)
            {
                outcome.failureText = QStringLiteral(
                    "标准通道单次物理读上限 %1 字节，请缩小范围。")
                    .arg(kStandardPhysicalReadMax);
                return outcome;
            }
            const ksword::ark::PhysicalMemoryReadResult result = client.readPhysicalMemory(
                physicalAddress,
                static_cast<std::uint32_t>(lengthBytes),
                0UL);
            if (!result.io.ok)
            {
                outcome.failureText = QStringLiteral("物理内存读取失败：%1")
                    .arg(QString::fromStdString(result.io.message));
                return outcome;
            }
            const bool usable =
                (result.readStatus == KSWORD_ARK_MEMORY_PHYSICAL_READ_STATUS_OK ||
                 result.readStatus == KSWORD_ARK_MEMORY_PHYSICAL_READ_STATUS_PARTIAL);
            if (!usable || result.data.empty())
            {
                outcome.failureText = QStringLiteral(
                    "物理内存读取未返回可用数据，readStatus=%1。").arg(result.readStatus);
                return outcome;
            }
            outcome.ok = true;
            outcome.partial =
                (result.readStatus == KSWORD_ARK_MEMORY_PHYSICAL_READ_STATUS_PARTIAL);
            outcome.bytesDone = static_cast<std::uint64_t>(result.data.size());
            outcome.data = QByteArray(
                reinterpret_cast<const char*>(result.data.data()),
                static_cast<qsizetype>(result.data.size()));
            return outcome;
        }

        QString unusableReason;
        if (!isDdmaUsable(session, &unusableReason))
        {
            outcome.failureText = unusableReason;
            return outcome;
        }

        // DDMA 的传输粒度是一页，按页边界切片逐页读取。
        outcome.data.reserve(static_cast<qsizetype>(lengthBytes));
        std::uint64_t cursor = physicalAddress;
        std::uint64_t remaining = lengthBytes;
        while (remaining > 0ULL)
        {
            // 切片长度用共用纯函数算，四处循环共用同一个判据。
            const std::uint64_t chunkLength =
                static_cast<std::uint64_t>(KswordArkDdmaChunkLength(cursor, remaining));

            const AccessOutcome chunk = ddmaReadOnePage(
                client, session, cursor, static_cast<std::uint32_t>(chunkLength));
            mergeChunkOutcome(outcome, chunk);
            if (!chunk.ok)
            {
                outcome.failureText = QStringLiteral("物理地址 %1：%2")
                    .arg(formatHex(cursor))
                    .arg(chunk.failureText);
                outcome.bytesDone = static_cast<std::uint64_t>(outcome.data.size());
                return outcome;
            }
            outcome.data.append(chunk.data);
            cursor += chunkLength;
            remaining -= chunkLength;
        }

        outcome.ok = true;
        outcome.bytesDone = static_cast<std::uint64_t>(outcome.data.size());
        return outcome;
    }

    AccessOutcome writePhysical(
        const MemoryAccessBackend backend,
        const DdmaSession& session,
        const std::uint64_t physicalAddress,
        const QByteArray& bytes,
        const bool forceApproved)
    {
        AccessOutcome outcome;
        if (bytes.isEmpty())
        {
            outcome.failureText = QStringLiteral("写入长度为 0。");
            return outcome;
        }

        const ksword::ark::DriverClient client;

        if (backend == MemoryAccessBackend::StandardDriver)
        {
            // 标准物理写单次上限 4KB，超过按上限切片逐块提交。
            qsizetype offset = 0;
            while (offset < bytes.size())
            {
                const qsizetype chunkSize = std::min<qsizetype>(
                    static_cast<qsizetype>(kStandardPhysicalWriteMax),
                    bytes.size() - offset);
                const QByteArray chunk = bytes.mid(offset, chunkSize);
                const std::vector<std::uint8_t> payload(
                    reinterpret_cast<const std::uint8_t*>(chunk.constData()),
                    reinterpret_cast<const std::uint8_t*>(chunk.constData()) + chunk.size());

                unsigned long writeFlags = KSWORD_ARK_PHYSICAL_WRITE_FLAG_UI_CONFIRMED;
                if (forceApproved)
                {
                    writeFlags |= KSWORD_ARK_PHYSICAL_WRITE_FLAG_FORCE;
                }
                const ksword::ark::PhysicalMemoryWriteResult result =
                    client.writePhysicalMemory(
                        physicalAddress + static_cast<std::uint64_t>(offset),
                        payload,
                        writeFlags);

                if (result.io.ok &&
                    result.writeStatus == KSWORD_ARK_MEMORY_PHYSICAL_WRITE_STATUS_FORCE_REQUIRED)
                {
                    outcome.forceRequired = true;
                    outcome.failureText = QStringLiteral("驱动要求对物理内存写入附加强制标志。");
                    outcome.bytesDone = static_cast<std::uint64_t>(offset);
                    return outcome;
                }
                const bool chunkOk = result.io.ok &&
                    result.writeStatus == KSWORD_ARK_MEMORY_PHYSICAL_WRITE_STATUS_OK &&
                    result.bytesWritten == static_cast<std::uint32_t>(chunk.size());
                if (!chunkOk)
                {
                    outcome.failureText = QStringLiteral(
                        "物理内存写入失败。地址 %1，writeStatus=%2；本轮已写入 %3 字节，"
                        "失败前的改动不会自动回滚。")
                        .arg(formatHex(physicalAddress + static_cast<std::uint64_t>(offset)))
                        .arg(result.writeStatus)
                        .arg(offset);
                    outcome.bytesDone = static_cast<std::uint64_t>(offset);
                    return outcome;
                }
                offset += chunkSize;
            }
            outcome.ok = true;
            outcome.bytesDone = static_cast<std::uint64_t>(bytes.size());
            return outcome;
        }

        QString unusableReason;
        if (!isDdmaUsable(session, &unusableReason))
        {
            outcome.failureText = unusableReason;
            return outcome;
        }

        qsizetype offset = 0;
        while (offset < bytes.size())
        {
            const std::uint64_t cursor = physicalAddress + static_cast<std::uint64_t>(offset);
            // 切片长度用共用纯函数算，四处循环共用同一个判据。
            const qsizetype chunkSize = static_cast<qsizetype>(KswordArkDdmaChunkLength(
                cursor,
                static_cast<std::uint64_t>(bytes.size() - offset)));
            const QByteArray chunk = bytes.mid(offset, chunkSize);

            const AccessOutcome chunkOutcome =
                ddmaWriteOnePage(client, session, cursor, chunk, forceApproved);
            mergeChunkOutcome(outcome, chunkOutcome);
            if (!chunkOutcome.ok)
            {
                outcome.forceRequired = chunkOutcome.forceRequired;
                outcome.failureText = QStringLiteral("物理地址 %1：%2 本轮已写入 %3 字节。")
                    .arg(formatHex(cursor))
                    .arg(chunkOutcome.failureText)
                    .arg(offset);
                outcome.bytesDone = static_cast<std::uint64_t>(offset);
                return outcome;
            }
            offset += chunkSize;
        }

        outcome.ok = true;
        outcome.bytesDone = static_cast<std::uint64_t>(bytes.size());
        return outcome;
    }

    AccessOutcome readVirtual(
        const MemoryAccessBackend backend,
        const DdmaSession& session,
        const std::uint32_t processId,
        const std::uint64_t virtualAddress,
        const std::uint64_t lengthBytes)
    {
        AccessOutcome outcome;
        if (lengthBytes == 0ULL)
        {
            outcome.failureText = QStringLiteral("读取长度为 0。");
            return outcome;
        }

        const ksword::ark::DriverClient client;

        if (backend == MemoryAccessBackend::StandardDriver)
        {
            if (lengthBytes > kStandardVirtualReadMax)
            {
                outcome.failureText = QStringLiteral(
                    "标准通道单次虚拟读上限 %1 字节，请缩小范围。")
                    .arg(kStandardVirtualReadMax);
                return outcome;
            }
            unsigned long readFlags = KSWORD_ARK_MEMORY_READ_FLAG_ZERO_FILL_UNREADABLE;
            if (isKernelVirtualAddress(virtualAddress))
            {
                readFlags |= KSWORD_ARK_MEMORY_READ_FLAG_KERNEL_ADDRESS;
            }
            const ksword::ark::VirtualMemoryReadResult result = client.readVirtualMemory(
                processId,
                virtualAddress,
                static_cast<std::uint32_t>(lengthBytes),
                readFlags);
            if (!result.io.ok)
            {
                outcome.failureText = QStringLiteral("虚拟内存读取失败：%1")
                    .arg(QString::fromStdString(result.io.message));
                return outcome;
            }
            if (result.data.empty())
            {
                outcome.failureText = QStringLiteral(
                    "虚拟内存读取未返回数据，readStatus=%1。").arg(result.readStatus);
                return outcome;
            }
            outcome.ok = true;
            outcome.partial =
                (result.readStatus == KSWORD_ARK_MEMORY_READ_STATUS_PARTIAL_COPY);
            outcome.bytesDone = static_cast<std::uint64_t>(result.data.size());
            outcome.data = QByteArray(
                reinterpret_cast<const char*>(result.data.data()),
                static_cast<qsizetype>(result.data.size()));
            return outcome;
        }

        QString unusableReason;
        if (!isDdmaUsable(session, &unusableReason))
        {
            outcome.failureText = unusableReason;
            return outcome;
        }

        // DDMA 只认物理地址，虚拟地址必须逐页翻译。翻译走的是 R0 既有的
        // 只读页表游走后端，这里不重新实现任何页表解析。
        outcome.data.reserve(static_cast<qsizetype>(lengthBytes));
        std::uint64_t cursor = virtualAddress;
        std::uint64_t remaining = lengthBytes;
        while (remaining > 0ULL)
        {
            // 切片长度用共用纯函数算，四处循环共用同一个判据。
            const std::uint64_t chunkLength =
                static_cast<std::uint64_t>(KswordArkDdmaChunkLength(cursor, remaining));

            const ksword::ark::VirtualAddressTranslateResult translation =
                client.translateVirtualAddress(processId, cursor);
            if (!translation.io.ok || !translation.resolved)
            {
                // 这一页翻译不出物理地址：按不可读处理，零填充并标记为部分结果。
                // 不静默跳过，也不整体失败——内存查看器需要能继续展示后面的页。
                outcome.data.append(static_cast<qsizetype>(chunkLength), '\0');
                outcome.partial = true;
                cursor += chunkLength;
                remaining -= chunkLength;
                continue;
            }

            // translateVirtualAddress 返回的是整个映射的物理基址加页内偏移，
            // 大页情形下 pageSize 会大于 4KB，但物理地址本身已经算好了偏移，
            // 直接用即可；DDMA 的一次传输仍然只覆盖 4KB。
            const AccessOutcome chunk = ddmaReadOnePage(
                client,
                session,
                translation.physicalAddress,
                static_cast<std::uint32_t>(chunkLength));
            mergeChunkOutcome(outcome, chunk);
            if (!chunk.ok)
            {
                outcome.failureText = QStringLiteral("虚拟地址 %1（物理 %2）：%3")
                    .arg(formatHex(cursor))
                    .arg(formatHex(translation.physicalAddress))
                    .arg(chunk.failureText);
                outcome.bytesDone = static_cast<std::uint64_t>(outcome.data.size());
                return outcome;
            }
            outcome.data.append(chunk.data);
            cursor += chunkLength;
            remaining -= chunkLength;
        }

        outcome.ok = true;
        outcome.bytesDone = static_cast<std::uint64_t>(outcome.data.size());
        return outcome;
    }

    AccessOutcome writeVirtual(
        const MemoryAccessBackend backend,
        const DdmaSession& session,
        const std::uint32_t processId,
        const std::uint64_t virtualAddress,
        const QByteArray& bytes,
        const bool forceApproved)
    {
        AccessOutcome outcome;
        if (bytes.isEmpty())
        {
            outcome.failureText = QStringLiteral("写入长度为 0。");
            return outcome;
        }

        const ksword::ark::DriverClient client;

        if (backend == MemoryAccessBackend::StandardDriver)
        {
            qsizetype offset = 0;
            while (offset < bytes.size())
            {
                const qsizetype chunkSize = std::min<qsizetype>(
                    static_cast<qsizetype>(kStandardVirtualWriteMax),
                    bytes.size() - offset);
                const QByteArray chunk = bytes.mid(offset, chunkSize);
                const std::vector<std::uint8_t> payload(
                    reinterpret_cast<const std::uint8_t*>(chunk.constData()),
                    reinterpret_cast<const std::uint8_t*>(chunk.constData()) + chunk.size());
                const std::uint64_t target =
                    virtualAddress + static_cast<std::uint64_t>(offset);

                unsigned long writeFlags = KSWORD_ARK_MEMORY_WRITE_FLAG_UI_CONFIRMED;
                if (isKernelVirtualAddress(target))
                {
                    writeFlags |= KSWORD_ARK_MEMORY_WRITE_FLAG_KERNEL_ADDRESS;
                }
                if (forceApproved)
                {
                    writeFlags |= KSWORD_ARK_MEMORY_WRITE_FLAG_FORCE;
                }
                const ksword::ark::VirtualMemoryWriteResult result =
                    client.writeVirtualMemory(processId, target, payload, writeFlags);

                if (result.io.ok &&
                    result.writeStatus == KSWORD_ARK_MEMORY_WRITE_STATUS_FORCE_REQUIRED)
                {
                    outcome.forceRequired = true;
                    outcome.failureText = QStringLiteral("驱动要求对本次写入附加强制标志。");
                    outcome.bytesDone = static_cast<std::uint64_t>(offset);
                    return outcome;
                }
                const bool chunkOk = result.io.ok &&
                    result.writeStatus == KSWORD_ARK_MEMORY_WRITE_STATUS_OK &&
                    result.bytesWritten == static_cast<std::uint32_t>(chunk.size());
                if (!chunkOk)
                {
                    outcome.failureText = QStringLiteral(
                        "虚拟内存写入失败。地址 %1，writeStatus=%2；本轮已写入 %3 字节。")
                        .arg(formatHex(target))
                        .arg(result.writeStatus)
                        .arg(offset);
                    outcome.bytesDone = static_cast<std::uint64_t>(offset);
                    return outcome;
                }
                offset += chunkSize;
            }
            outcome.ok = true;
            outcome.bytesDone = static_cast<std::uint64_t>(bytes.size());
            return outcome;
        }

        QString unusableReason;
        if (!isDdmaUsable(session, &unusableReason))
        {
            outcome.failureText = unusableReason;
            return outcome;
        }

        qsizetype offset = 0;
        while (offset < bytes.size())
        {
            const std::uint64_t cursor = virtualAddress + static_cast<std::uint64_t>(offset);
            // 切片长度用共用纯函数算，四处循环共用同一个判据。
            const qsizetype chunkSize = static_cast<qsizetype>(KswordArkDdmaChunkLength(
                cursor,
                static_cast<std::uint64_t>(bytes.size() - offset)));
            const QByteArray chunk = bytes.mid(offset, chunkSize);

            const ksword::ark::VirtualAddressTranslateResult translation =
                client.translateVirtualAddress(processId, cursor);
            if (!translation.io.ok || !translation.resolved)
            {
                // 写入路径上翻译失败必须整体停下。跳过一页继续写会造成
                // "看起来写成功了、其实中间有一段没写进去"的静默损坏。
                outcome.failureText = QStringLiteral(
                    "虚拟地址 %1 无法翻译成物理地址，DDMA 写入已停止；本轮已写入 %2 字节。")
                    .arg(formatHex(cursor))
                    .arg(offset);
                outcome.bytesDone = static_cast<std::uint64_t>(offset);
                return outcome;
            }

            const AccessOutcome chunkOutcome = ddmaWriteOnePage(
                client, session, translation.physicalAddress, chunk, forceApproved);
            mergeChunkOutcome(outcome, chunkOutcome);
            if (!chunkOutcome.ok)
            {
                outcome.forceRequired = chunkOutcome.forceRequired;
                outcome.failureText = QStringLiteral("虚拟地址 %1（物理 %2）：%3 本轮已写入 %4 字节。")
                    .arg(formatHex(cursor))
                    .arg(formatHex(translation.physicalAddress))
                    .arg(chunkOutcome.failureText)
                    .arg(offset);
                outcome.bytesDone = static_cast<std::uint64_t>(offset);
                return outcome;
            }
            offset += chunkSize;
        }

        outcome.ok = true;
        outcome.bytesDone = static_cast<std::uint64_t>(bytes.size());
        return outcome;
    }
}
