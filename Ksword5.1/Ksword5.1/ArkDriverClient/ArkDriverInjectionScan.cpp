#include "ArkDriverClient.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace ksword::ark
{
    namespace
    {
        // 响应缓冲上限。VAD 每条 56 字节、PTE 段每条 48 字节，按各自的条目上限
        // 算出来的最坏情况都在 1 MiB 量级；这里统一留 2 MiB 顶。
        constexpr std::size_t kMaxInjectionResponseBytes = 2U * 1024U * 1024U;

        template <typename Header, typename Entry>
        bool validateVariableResponse(
            const std::vector<std::uint8_t>& buffer,
            const unsigned long bytesReturned,
            const std::size_t headerSize,
            IoResult& io,
            const char* label,
            const Header*& headerOut,
            std::size_t& availableEntriesOut)
        {
            headerOut = nullptr;
            availableEntriesOut = 0U;
            if (bytesReturned < headerSize)
            {
                io.ok = false;
                io.message = std::string(label) + " response too small, bytesReturned=" +
                             std::to_string(bytesReturned);
                return false;
            }
            const auto* header = reinterpret_cast<const Header*>(buffer.data());
            if (header->entrySize < sizeof(Entry))
            {
                io.ok = false;
                io.message = std::string(label) + " entry size invalid, entrySize=" +
                             std::to_string(header->entrySize);
                return false;
            }
            // 条目数以**实际返回的字节**为准，不信 returnedCount：驱动报大了就会
            // 让这里越界读自己的缓冲。
            availableEntriesOut =
                (static_cast<std::size_t>(bytesReturned) - headerSize) / header->entrySize;
            headerOut = header;
            return true;
        }
    }

    ProcessVadEnumResult DriverClient::enumerateProcessVad(
        const std::uint32_t processId,
        const std::uint64_t startAddress,
        const std::uint64_t endAddress,
        const std::uint64_t cursorVpn,
        const unsigned long maxEntries,
        const unsigned long flags) const
    {
        // 作用：枚举目标进程的 VAD 树，作为 R3 VirtualQueryEx 之外的独立区域视图。
        // 处理：输入只有 PID 与范围/游标；驱动不接受任何内核地址作为凭据。
        // 返回：ProcessVadEnumResult；profileVerified 为 0 时结果不可用于缺项推断。
        ProcessVadEnumResult result{};
        KSWORD_ARK_ENUMERATE_PROCESS_VAD_REQUEST request{};
        request.flags = flags;
        request.processId = processId;
        request.maxEntries = maxEntries;
        request.startAddress = startAddress;
        request.endAddress = endAddress;
        request.cursorVpn = cursorVpn;

        const unsigned long boundedEntries =
            std::min<unsigned long>(
                std::max<unsigned long>(maxEntries, 1UL),
                KSWORD_ARK_INJECTION_VAD_LIMIT_MAX);
        std::size_t bufferBytes =
            KSWORD_ARK_INJECTION_VAD_RESPONSE_HEADER_SIZE +
            (static_cast<std::size_t>(boundedEntries) * sizeof(KSWORD_ARK_PROCESS_VAD_ENTRY));
        bufferBytes = std::min(bufferBytes, kMaxInjectionResponseBytes);

        std::vector<std::uint8_t> responseBuffer(bufferBytes, 0U);
        result.io = deviceIoControl(
            IOCTL_KSWORD_ARK_ENUMERATE_PROCESS_VAD,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            responseBuffer.data(),
            static_cast<unsigned long>(responseBuffer.size()));
        if (!result.io.ok)
        {
            result.io.message =
                "DeviceIoControl(IOCTL_KSWORD_ARK_ENUMERATE_PROCESS_VAD) failed, error=" +
                std::to_string(result.io.win32Error);
            return result;
        }

        const KSWORD_ARK_ENUMERATE_PROCESS_VAD_RESPONSE* header = nullptr;
        std::size_t available = 0U;
        if (!validateVariableResponse<
                KSWORD_ARK_ENUMERATE_PROCESS_VAD_RESPONSE,
                KSWORD_ARK_PROCESS_VAD_ENTRY>(
                responseBuffer,
                result.io.bytesReturned,
                KSWORD_ARK_INJECTION_VAD_RESPONSE_HEADER_SIZE,
                result.io,
                "enumerate-process-vad",
                header,
                available))
        {
            return result;
        }

        result.version = static_cast<std::uint32_t>(header->version);
        result.processId = static_cast<std::uint32_t>(header->processId);
        result.fieldFlags = static_cast<std::uint32_t>(header->fieldFlags);
        result.status = static_cast<std::uint32_t>(header->status);
        result.lastStatus = header->lastStatus;
        result.returnedCount = static_cast<std::uint32_t>(header->returnedCount);
        result.visitedCount = static_cast<std::uint32_t>(header->visitedCount);
        result.unreadableNodeCount = static_cast<std::uint32_t>(header->unreadableNodeCount);
        result.profileVerified = static_cast<std::uint32_t>(header->profileVerified);
        result.vadRootOffset = static_cast<std::uint32_t>(header->vadRootOffset);
        result.vadRootAddress = header->vadRootAddress;
        result.nextCursorVpn = header->nextCursorVpn;

        const std::size_t parsed =
            std::min<std::size_t>(available, static_cast<std::size_t>(header->returnedCount));
        result.entries.reserve(parsed);
        for (std::size_t i = 0; i < parsed; ++i)
        {
            const auto* raw = reinterpret_cast<const KSWORD_ARK_PROCESS_VAD_ENTRY*>(
                responseBuffer.data() + KSWORD_ARK_INJECTION_VAD_RESPONSE_HEADER_SIZE +
                (i * header->entrySize));
            ProcessVadEntry entry{};
            entry.startVa = raw->startVa;
            entry.endVaExclusive = raw->endVaExclusive;
            entry.vadNodeAddress = raw->vadNodeAddress;
            entry.controlArea = raw->controlArea;
            entry.firstPrototypePte = raw->firstPrototypePte;
            entry.vadFlagsRaw = static_cast<std::uint32_t>(raw->vadFlagsRaw);
            entry.protection = static_cast<std::uint32_t>(raw->protection);
            entry.vadType = static_cast<std::uint32_t>(raw->vadType);
            entry.entryFlags = static_cast<std::uint32_t>(raw->entryFlags);
            result.entries.push_back(entry);
        }
        return result;
    }

    ProcessExecutablePteScanResult DriverClient::scanProcessExecutablePte(
        const std::uint32_t processId,
        const std::uint64_t startAddress,
        const std::uint64_t endAddress,
        const std::uint64_t cursorAddress,
        const unsigned long maxEntries,
        const unsigned long maxTableReads,
        const unsigned long flags) const
    {
        // 作用：在目标进程页表上做范围扫描，报出用户态可执行叶子页。
        // 处理：这是"处理器实际怎么看"的视图，与 VAD/VirtualQueryEx 的保护属性独立。
        // 返回：ProcessExecutablePteScanResult；TRUNCATED 时用 nextCursorAddress 续扫。
        ProcessExecutablePteScanResult result{};
        KSWORD_ARK_SCAN_PROCESS_EXECUTABLE_PTE_REQUEST request{};
        request.flags = flags;
        request.processId = processId;
        request.maxEntries = maxEntries;
        request.maxTableReads = maxTableReads;
        request.startAddress = startAddress;
        request.endAddress = endAddress;
        request.cursorAddress = cursorAddress;

        const unsigned long boundedEntries =
            std::min<unsigned long>(
                std::max<unsigned long>(maxEntries, 1UL),
                KSWORD_ARK_INJECTION_PTE_LIMIT_MAX);
        std::size_t bufferBytes =
            KSWORD_ARK_INJECTION_PTE_RESPONSE_HEADER_SIZE +
            (static_cast<std::size_t>(boundedEntries) *
             sizeof(KSWORD_ARK_PROCESS_EXECUTABLE_PTE_ENTRY));
        bufferBytes = std::min(bufferBytes, kMaxInjectionResponseBytes);

        std::vector<std::uint8_t> responseBuffer(bufferBytes, 0U);
        result.io = deviceIoControl(
            IOCTL_KSWORD_ARK_SCAN_PROCESS_EXECUTABLE_PTE,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            responseBuffer.data(),
            static_cast<unsigned long>(responseBuffer.size()));
        if (!result.io.ok)
        {
            result.io.message =
                "DeviceIoControl(IOCTL_KSWORD_ARK_SCAN_PROCESS_EXECUTABLE_PTE) failed, error=" +
                std::to_string(result.io.win32Error);
            return result;
        }

        const KSWORD_ARK_SCAN_PROCESS_EXECUTABLE_PTE_RESPONSE* header = nullptr;
        std::size_t available = 0U;
        if (!validateVariableResponse<
                KSWORD_ARK_SCAN_PROCESS_EXECUTABLE_PTE_RESPONSE,
                KSWORD_ARK_PROCESS_EXECUTABLE_PTE_ENTRY>(
                responseBuffer,
                result.io.bytesReturned,
                KSWORD_ARK_INJECTION_PTE_RESPONSE_HEADER_SIZE,
                result.io,
                "scan-process-executable-pte",
                header,
                available))
        {
            return result;
        }

        result.version = static_cast<std::uint32_t>(header->version);
        result.processId = static_cast<std::uint32_t>(header->processId);
        result.fieldFlags = static_cast<std::uint32_t>(header->fieldFlags);
        result.status = static_cast<std::uint32_t>(header->status);
        result.lastStatus = header->lastStatus;
        result.returnedCount = static_cast<std::uint32_t>(header->returnedCount);
        result.tableReads = static_cast<std::uint32_t>(header->tableReads);
        result.failedTableReads = static_cast<std::uint32_t>(header->failedTableReads);
        result.executablePageCount = static_cast<std::uint32_t>(header->executablePageCount);
        result.scannedBegin = header->scannedBegin;
        result.scannedEnd = header->scannedEnd;
        result.nextCursorAddress = header->nextCursorAddress;
        result.cr3PhysicalAddress = header->cr3PhysicalAddress;

        const std::size_t parsed =
            std::min<std::size_t>(available, static_cast<std::size_t>(header->returnedCount));
        result.entries.reserve(parsed);
        for (std::size_t i = 0; i < parsed; ++i)
        {
            const auto* raw =
                reinterpret_cast<const KSWORD_ARK_PROCESS_EXECUTABLE_PTE_ENTRY*>(
                    responseBuffer.data() + KSWORD_ARK_INJECTION_PTE_RESPONSE_HEADER_SIZE +
                    (i * header->entrySize));
            ProcessExecutablePteEntry entry{};
            entry.startVa = raw->startVa;
            entry.byteLength = raw->byteLength;
            entry.firstPhysicalAddress = raw->firstPhysicalAddress;
            entry.firstEntryValue = raw->firstEntryValue;
            entry.pageSize = static_cast<std::uint32_t>(raw->pageSize);
            entry.pageCount = static_cast<std::uint32_t>(raw->pageCount);
            entry.effectiveFlags = static_cast<std::uint32_t>(raw->effectiveFlags);
            entry.entryFlags = static_cast<std::uint32_t>(raw->entryFlags);
            result.entries.push_back(entry);
        }
        return result;
    }
}
