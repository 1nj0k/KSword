#include "ArkDriverClient.h"
// 区间判据与 R0 共用同一份纯函数实现，不在 R3 再写一遍。
#include "../../../shared/driver/KswordArkDdmaPlan.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// ============================================================
// ArkDriverDdma.cpp
// 作用：
// - 封装 VA → PA 翻译（复用 R0 既有的页表游走后端）；
// - 封装 DDMA（磁盘直接内存访问）的能力查询与物理读写。
//
// DDMA 是什么：借助磁盘控制器的总线主控 DMA 读写任意物理地址。数据通路走
// HBA 而不是 CPU 页表，因此不受 SLAT/EPT 约束，能看到被上层虚拟化重定向的
// 物理页。代价是必须借一块磁盘扇区当中转站，所以每次调用都要显式给出暂存
// LBA，本文件不提供任何默认值。
// ============================================================

namespace ksword::ark
{
    namespace
    {
        // 头长度一律取 offsetof。协议侧已经让 R0 用 FIELD_OFFSET 计算头长度，
        // R3 这边必须用同一个数字，否则解析整体错位。
        constexpr std::size_t kDdmaQueryResponseHeaderSize =
            offsetof(KSWORD_ARK_DDMA_QUERY_CAPABILITY_RESPONSE, entries);
        constexpr std::size_t kDdmaReadResponseHeaderSize =
            offsetof(KSWORD_ARK_DDMA_READ_PHYSICAL_RESPONSE, data);
        constexpr std::size_t kDdmaWriteRequestHeaderSize =
            offsetof(KSWORD_ARK_DDMA_WRITE_PHYSICAL_REQUEST, data);

        // 48 位 LBA 上限。取共用协议常量，不在这里另写一个字面量。
        constexpr std::uint64_t kDdmaLbaMax = KSWORD_ARK_DDMA_LBA48_LIMIT;

        // copyFixedWideString 用途：把协议里的定长 wchar_t 数组转成 std::wstring，
        // 并且在没有结尾 NUL 时按数组容量截断，不越界扫描。
        std::wstring copyFixedWideString(const wchar_t* const buffer, const std::size_t capacityChars)
        {
            if (buffer == nullptr || capacityChars == 0U)
            {
                return std::wstring();
            }
            std::size_t length = 0U;
            while (length < capacityChars && buffer[length] != L'\0')
            {
                ++length;
            }
            return std::wstring(buffer, length);
        }

        // isDdmaUnsupported 用途：把"驱动根本不认识这个 IOCTL"与"认识但拒绝了"
        // 区分开。未知 IOCTL 在 dispatch 层返回 STATUS_INVALID_DEVICE_REQUEST，
        // 映射到 Win32 的 ERROR_INVALID_FUNCTION。
        bool isDdmaUnsupported(const IoResult& io)
        {
            return !io.ok &&
                (io.win32Error == static_cast<unsigned long>(ERROR_INVALID_FUNCTION) ||
                 io.win32Error == static_cast<unsigned long>(ERROR_NOT_SUPPORTED));
        }

        // isDdmaRangeAcceptable 用途：在发出 IOCTL 之前用与 R0 完全相同的判据
        // 先拦一次。判据本体是 KswordArkDdmaPlan.h 里的共用纯函数，两侧不各写
        // 一份——区间判据一旦走散，R3 放行而 R0 拒绝只是浪费一次往返，反过来
        // 则会让越界请求真的发到磁盘上去。
        bool isDdmaRangeAcceptable(const std::uint64_t physicalAddress, const std::uint32_t length)
        {
            return KswordArkDdmaIsPhysicalRangeValid(physicalAddress, length) != 0;
        }
    }

    VirtualAddressTranslateResult DriverClient::translateVirtualAddress(
        const std::uint32_t processId,
        const std::uint64_t virtualAddress,
        DriverHandle* const existingHandle) const
    {
        VirtualAddressTranslateResult translateResult{};
        KSWORD_ARK_TRANSLATE_VIRTUAL_ADDRESS_REQUEST request{};
        KSWORD_ARK_TRANSLATE_VIRTUAL_ADDRESS_RESPONSE response{};

        // 协议要求 flags/reserved 为零，R0 对任何非零位直接判无效参数。
        request.flags = 0UL;
        request.processId = processId;
        request.virtualAddress = virtualAddress;
        request.reserved = 0UL;

        translateResult.io = deviceIoControl(
            IOCTL_KSWORD_ARK_TRANSLATE_VIRTUAL_ADDRESS,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            &response,
            static_cast<unsigned long>(sizeof(response)),
            existingHandle);
        if (!translateResult.io.ok)
        {
            translateResult.io.message =
                "DeviceIoControl(IOCTL_KSWORD_ARK_TRANSLATE_VIRTUAL_ADDRESS) failed, error=" +
                std::to_string(translateResult.io.win32Error);
            return translateResult;
        }
        if (translateResult.io.bytesReturned < sizeof(response))
        {
            translateResult.io.ok = false;
            translateResult.io.message =
                "translate-virtual-address response too small, bytesReturned=" +
                std::to_string(translateResult.io.bytesReturned);
            return translateResult;
        }

        const KSWORD_ARK_PAGE_TABLE_ENTRY_INFO& info = response.info;
        translateResult.version = static_cast<std::uint32_t>(info.version);
        translateResult.processId = static_cast<std::uint32_t>(info.processId);
        translateResult.fieldFlags = static_cast<std::uint32_t>(info.fieldFlags);
        translateResult.queryStatus = static_cast<std::uint32_t>(info.queryStatus);
        translateResult.lookupStatus = static_cast<long>(info.lookupStatus);
        translateResult.walkStatus = static_cast<long>(info.walkStatus);
        translateResult.virtualAddress = static_cast<std::uint64_t>(info.virtualAddress);
        translateResult.physicalAddress = static_cast<std::uint64_t>(info.physicalAddress);
        translateResult.cr3PhysicalAddress = static_cast<std::uint64_t>(info.cr3PhysicalAddress);
        translateResult.pageSize = static_cast<std::uint32_t>(info.pageSize);
        translateResult.largePageType = static_cast<std::uint32_t>(info.largePageType);
        translateResult.protection = static_cast<std::uint32_t>(info.protection);
        translateResult.confidence = static_cast<std::uint32_t>(info.confidence);

        // resolved 必须同时满足三个条件：R0 自报解析成功、聚合状态为 OK、
        // 并且 fieldFlags 里确实带了物理地址位。只看 resolved 一个字段会在
        // "走到 not-present 表项" 的路径上拿到一个没有意义的物理地址。
        translateResult.resolved =
            (info.resolved != 0UL) &&
            (translateResult.queryStatus == KSWORD_ARK_MEMORY_TRANSLATE_STATUS_OK) &&
            ((translateResult.fieldFlags & KSWORD_ARK_MEMORY_FIELD_PHYSICAL_ADDRESS_PRESENT) != 0UL);

        return translateResult;
    }

    DdmaCapabilityResult DriverClient::queryDdmaCapability(
        const bool probeTransfer,
        const std::uint64_t scratchLba,
        const bool scratchLbaValid,
        const unsigned long maxDisks,
        DriverHandle* const existingHandle) const
    {
        DdmaCapabilityResult capabilityResult{};
        KSWORD_ARK_DDMA_QUERY_CAPABILITY_REQUEST request{};

        unsigned long requestedDisks = maxDisks;
        if (requestedDisks == 0UL)
        {
            requestedDisks = KSWORD_ARK_DDMA_DISK_LIMIT_DEFAULT;
        }
        if (requestedDisks > KSWORD_ARK_DDMA_DISK_LIMIT_HARD)
        {
            requestedDisks = KSWORD_ARK_DDMA_DISK_LIMIT_HARD;
        }

        if (scratchLbaValid && scratchLba >= kDdmaLbaMax)
        {
            capabilityResult.io.ok = false;
            capabilityResult.io.win32Error = ERROR_INVALID_PARAMETER;
            capabilityResult.io.message = "暂存 LBA 超出 48 位上限";
            return capabilityResult;
        }

        request.flags = 0UL;
        if (probeTransfer)
        {
            request.flags |= KSWORD_ARK_DDMA_QUERY_FLAG_PROBE_TRANSFER;
        }
        // 探测会往指定 LBA 发读命令，所以同样要求调用方显式声明这个 LBA。
        // 没声明时驱动只枚举设备不发命令，这是一条有意保留的降级路径。
        if (scratchLbaValid)
        {
            request.flags |= KSWORD_ARK_DDMA_FLAG_SCRATCH_LBA_VALID;
            request.scratchLba = scratchLba;
        }
        request.maxDisks = requestedDisks;

        std::vector<std::uint8_t> responseBuffer(
            kDdmaQueryResponseHeaderSize +
                (static_cast<std::size_t>(requestedDisks) * sizeof(KSWORD_ARK_DDMA_DISK_ENTRY)),
            0U);
        capabilityResult.io = deviceIoControl(
            IOCTL_KSWORD_ARK_DDMA_QUERY_CAPABILITY,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            responseBuffer.data(),
            static_cast<unsigned long>(responseBuffer.size()),
            existingHandle);
        if (!capabilityResult.io.ok)
        {
            capabilityResult.unsupported = isDdmaUnsupported(capabilityResult.io);
            capabilityResult.io.message = capabilityResult.unsupported
                ? std::string("当前驱动不支持 DDMA（IOCTL_KSWORD_ARK_DDMA_QUERY_CAPABILITY 未注册）")
                : ("DeviceIoControl(IOCTL_KSWORD_ARK_DDMA_QUERY_CAPABILITY) failed, error=" +
                   std::to_string(capabilityResult.io.win32Error));
            return capabilityResult;
        }
        if (capabilityResult.io.bytesReturned < kDdmaQueryResponseHeaderSize)
        {
            capabilityResult.io.ok = false;
            capabilityResult.io.message =
                "ddma query response too small, bytesReturned=" +
                std::to_string(capabilityResult.io.bytesReturned);
            return capabilityResult;
        }

        const auto* responseHeader =
            reinterpret_cast<const KSWORD_ARK_DDMA_QUERY_CAPABILITY_RESPONSE*>(responseBuffer.data());
        capabilityResult.version = static_cast<std::uint32_t>(responseHeader->version);
        capabilityResult.status = static_cast<std::uint32_t>(responseHeader->status);
        capabilityResult.capabilityFlags = static_cast<std::uint32_t>(responseHeader->capabilityFlags);
        capabilityResult.totalDisks = static_cast<std::uint32_t>(responseHeader->totalDisks);
        capabilityResult.readyDisks = static_cast<std::uint32_t>(responseHeader->readyDisks);
        capabilityResult.transferBytes = static_cast<std::uint32_t>(responseHeader->transferBytes);
        capabilityResult.scratchSectorCount =
            static_cast<std::uint32_t>(responseHeader->scratchSectorCount);
        capabilityResult.lastStatus = static_cast<long>(responseHeader->lastStatus);
        capabilityResult.io.ntStatus = capabilityResult.lastStatus;

        // entrySize 由 R0 自报，解析步长必须按它走而不是按本地 sizeof，
        // 这样将来协议加字段时旧 R3 也不会整排错位。
        const std::size_t entrySize = (responseHeader->entrySize != 0UL)
            ? static_cast<std::size_t>(responseHeader->entrySize)
            : sizeof(KSWORD_ARK_DDMA_DISK_ENTRY);
        if (entrySize < sizeof(KSWORD_ARK_DDMA_DISK_ENTRY))
        {
            capabilityResult.io.ok = false;
            capabilityResult.io.message =
                "ddma query entrySize smaller than known layout, entrySize=" +
                std::to_string(entrySize);
            return capabilityResult;
        }

        const std::size_t availableCount =
            (static_cast<std::size_t>(capabilityResult.io.bytesReturned) - kDdmaQueryResponseHeaderSize) /
            entrySize;
        const std::size_t parsedCount = std::min<std::size_t>(
            static_cast<std::size_t>(responseHeader->returnedDisks),
            availableCount);

        capabilityResult.disks.reserve(parsedCount);
        for (std::size_t index = 0U; index < parsedCount; ++index)
        {
            const std::size_t entryOffset = kDdmaQueryResponseHeaderSize + (index * entrySize);
            const auto* entry = reinterpret_cast<const KSWORD_ARK_DDMA_DISK_ENTRY*>(
                responseBuffer.data() + entryOffset);

            DdmaDiskEntry diskEntry{};
            diskEntry.deviceIndex = static_cast<std::uint32_t>(entry->deviceIndex);
            diskEntry.diskFlags = static_cast<std::uint32_t>(entry->diskFlags);
            diskEntry.probeStatus = static_cast<long>(entry->probeStatus);
            diskEntry.scsiProbeStatus = static_cast<long>(entry->scsiProbeStatus);
            diskEntry.sectorSize = static_cast<std::uint32_t>(entry->sectorSize);
            if ((diskEntry.diskFlags & KSWORD_ARK_DDMA_DISK_FLAG_NAME_PRESENT) != 0UL)
            {
                diskEntry.deviceName =
                    copyFixedWideString(entry->deviceName, KSWORD_ARK_DDMA_DEVICE_NAME_CHARS);
            }
            capabilityResult.disks.push_back(diskEntry);
        }

        return capabilityResult;
    }

    DdmaReadResult DriverClient::ddmaReadPhysicalMemory(
        const std::uint32_t diskIndex,
        const std::uint64_t physicalAddress,
        const std::uint32_t bytesToRead,
        const std::uint64_t scratchLba,
        const unsigned long flags,
        DriverHandle* const existingHandle) const
    {
        DdmaReadResult readResult{};
        KSWORD_ARK_DDMA_READ_PHYSICAL_REQUEST request{};

        if ((flags & ~KSWORD_ARK_DDMA_READ_FLAG_ALLOWED) != 0UL)
        {
            readResult.io.ok = false;
            readResult.io.win32Error = ERROR_INVALID_PARAMETER;
            readResult.io.message = "DDMA 读取收到未知 flags 位";
            return readResult;
        }
        // 暂存 LBA 必须显式声明。这里本地就拦住，不让请求白跑一趟 IOCTL；
        // 判据是 flag 位而不是 scratchLba 是否为零，因为 LBA 0 是合法取值。
        if ((flags & KSWORD_ARK_DDMA_FLAG_SCRATCH_LBA_VALID) == 0UL)
        {
            readResult.io.ok = false;
            readResult.io.win32Error = ERROR_INVALID_PARAMETER;
            readResult.io.message = "DDMA 读取必须显式指定暂存扇区 LBA";
            return readResult;
        }
        if (scratchLba >= kDdmaLbaMax)
        {
            readResult.io.ok = false;
            readResult.io.win32Error = ERROR_INVALID_PARAMETER;
            readResult.io.message = "暂存 LBA 超出 48 位上限";
            return readResult;
        }
        if ((flags & KSWORD_ARK_DDMA_FLAG_SCRATCH_ACKNOWLEDGED) == 0UL)
        {
            readResult.io.ok = false;
            readResult.io.win32Error = ERROR_INVALID_PARAMETER;
            readResult.io.message = "DDMA 读取会临时覆盖暂存扇区，必须先确认";
            return readResult;
        }
        if (!isDdmaRangeAcceptable(physicalAddress, bytesToRead))
        {
            readResult.io.ok = false;
            readResult.io.win32Error = ERROR_INVALID_PARAMETER;
            readResult.io.message = "DDMA 读取区间无效：长度为 0、超过一页或跨页";
            return readResult;
        }

        request.flags = flags;
        request.diskIndex = diskIndex;
        request.physicalAddress = physicalAddress;
        request.scratchLba = scratchLba;
        request.bytesToRead = bytesToRead;
        request.reserved0 = 0UL;

        std::vector<std::uint8_t> responseBuffer(
            kDdmaReadResponseHeaderSize + static_cast<std::size_t>(bytesToRead),
            0U);
        readResult.io = deviceIoControl(
            IOCTL_KSWORD_ARK_DDMA_READ_PHYSICAL,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            responseBuffer.data(),
            static_cast<unsigned long>(responseBuffer.size()),
            existingHandle);
        if (!readResult.io.ok)
        {
            readResult.unsupported = isDdmaUnsupported(readResult.io);
            readResult.io.message = readResult.unsupported
                ? std::string("当前驱动不支持 DDMA（IOCTL_KSWORD_ARK_DDMA_READ_PHYSICAL 未注册）")
                : ("DeviceIoControl(IOCTL_KSWORD_ARK_DDMA_READ_PHYSICAL) failed, error=" +
                   std::to_string(readResult.io.win32Error));
            return readResult;
        }
        if (readResult.io.bytesReturned < kDdmaReadResponseHeaderSize)
        {
            readResult.io.ok = false;
            readResult.io.message =
                "ddma read response too small, bytesReturned=" +
                std::to_string(readResult.io.bytesReturned);
            return readResult;
        }

        const auto* responseHeader =
            reinterpret_cast<const KSWORD_ARK_DDMA_READ_PHYSICAL_RESPONSE*>(responseBuffer.data());
        readResult.version = static_cast<std::uint32_t>(responseHeader->version);
        readResult.fieldFlags = static_cast<std::uint32_t>(responseHeader->fieldFlags);
        readResult.readStatus = static_cast<std::uint32_t>(responseHeader->readStatus);
        readResult.mapStatus = static_cast<long>(responseHeader->mapStatus);
        readResult.backupStatus = static_cast<long>(responseHeader->backupStatus);
        readResult.stageOutStatus = static_cast<long>(responseHeader->stageOutStatus);
        readResult.stageInStatus = static_cast<long>(responseHeader->stageInStatus);
        readResult.restoreStatus = static_cast<long>(responseHeader->restoreStatus);
        readResult.requestedPhysicalAddress =
            static_cast<std::uint64_t>(responseHeader->requestedPhysicalAddress);
        readResult.scratchLba = static_cast<std::uint64_t>(responseHeader->scratchLba);
        readResult.diskIndex = static_cast<std::uint32_t>(responseHeader->diskIndex);
        readResult.requestedBytes = static_cast<std::uint32_t>(responseHeader->requestedBytes);
        readResult.bytesRead = static_cast<std::uint32_t>(responseHeader->bytesRead);
        readResult.maxBytesPerRequest =
            static_cast<std::uint32_t>(responseHeader->maxBytesPerRequest);
        if ((readResult.fieldFlags & KSWORD_ARK_DDMA_FIELD_DEVICE_NAME_PRESENT) != 0UL)
        {
            readResult.deviceName =
                copyFixedWideString(responseHeader->deviceName, KSWORD_ARK_DDMA_DEVICE_NAME_CHARS);
        }

        // 负载长度只按 bytesRead 与缓冲余量取小，不能由 bytesReturned 反推。
        const std::size_t dataCapacity = responseBuffer.size() - kDdmaReadResponseHeaderSize;
        const std::size_t dataBytes = std::min<std::size_t>(
            static_cast<std::size_t>(readResult.bytesRead),
            dataCapacity);
        if (dataBytes > 0U)
        {
            const std::uint8_t* const dataStart =
                responseBuffer.data() + kDdmaReadResponseHeaderSize;
            readResult.data.assign(dataStart, dataStart + dataBytes);
        }

        return readResult;
    }

    DdmaWriteResult DriverClient::ddmaWritePhysicalMemory(
        const std::uint32_t diskIndex,
        const std::uint64_t physicalAddress,
        const std::vector<std::uint8_t>& bytes,
        const std::uint64_t scratchLba,
        const unsigned long flags,
        DriverHandle* const existingHandle) const
    {
        DdmaWriteResult writeResult{};

        if ((flags & ~KSWORD_ARK_DDMA_WRITE_FLAG_ALLOWED) != 0UL)
        {
            writeResult.io.ok = false;
            writeResult.io.win32Error = ERROR_INVALID_PARAMETER;
            writeResult.io.message = "DDMA 写入收到未知 flags 位";
            return writeResult;
        }
        if ((flags & KSWORD_ARK_DDMA_FLAG_SCRATCH_LBA_VALID) == 0UL)
        {
            writeResult.io.ok = false;
            writeResult.io.win32Error = ERROR_INVALID_PARAMETER;
            writeResult.io.message = "DDMA 写入必须显式指定暂存扇区 LBA";
            return writeResult;
        }
        if (scratchLba >= kDdmaLbaMax)
        {
            writeResult.io.ok = false;
            writeResult.io.win32Error = ERROR_INVALID_PARAMETER;
            writeResult.io.message = "暂存 LBA 超出 48 位上限";
            return writeResult;
        }
        if ((flags & KSWORD_ARK_DDMA_FLAG_SCRATCH_ACKNOWLEDGED) == 0UL)
        {
            writeResult.io.ok = false;
            writeResult.io.win32Error = ERROR_INVALID_PARAMETER;
            writeResult.io.message = "DDMA 写入会临时覆盖暂存扇区，必须先确认";
            return writeResult;
        }
        if (bytes.empty() || bytes.size() > KSWORD_ARK_DDMA_WRITE_MAX_BYTES)
        {
            writeResult.io.ok = false;
            writeResult.io.win32Error = ERROR_INVALID_PARAMETER;
            writeResult.io.message = "DDMA 写入长度必须在 1 到一页之间";
            return writeResult;
        }
        if (!isDdmaRangeAcceptable(physicalAddress, static_cast<std::uint32_t>(bytes.size())))
        {
            writeResult.io.ok = false;
            writeResult.io.win32Error = ERROR_INVALID_PARAMETER;
            writeResult.io.message = "DDMA 写入区间无效：超过一页或跨页";
            return writeResult;
        }

        // 请求头加尾随负载，按协议头长度打底分配。
        std::vector<std::uint8_t> requestBuffer(
            kDdmaWriteRequestHeaderSize + bytes.size(),
            0U);
        auto* request =
            reinterpret_cast<KSWORD_ARK_DDMA_WRITE_PHYSICAL_REQUEST*>(requestBuffer.data());
        request->flags = flags;
        request->diskIndex = diskIndex;
        request->physicalAddress = physicalAddress;
        request->scratchLba = scratchLba;
        request->bytesToWrite = static_cast<unsigned long>(bytes.size());
        request->reserved0 = 0UL;
        std::copy(bytes.begin(), bytes.end(), requestBuffer.data() + kDdmaWriteRequestHeaderSize);

        KSWORD_ARK_DDMA_WRITE_PHYSICAL_RESPONSE response{};
        writeResult.io = deviceIoControl(
            IOCTL_KSWORD_ARK_DDMA_WRITE_PHYSICAL,
            requestBuffer.data(),
            static_cast<unsigned long>(requestBuffer.size()),
            &response,
            static_cast<unsigned long>(sizeof(response)),
            existingHandle);
        if (!writeResult.io.ok)
        {
            writeResult.unsupported = isDdmaUnsupported(writeResult.io);
            writeResult.io.message = writeResult.unsupported
                ? std::string("当前驱动不支持 DDMA（IOCTL_KSWORD_ARK_DDMA_WRITE_PHYSICAL 未注册）")
                : ("DeviceIoControl(IOCTL_KSWORD_ARK_DDMA_WRITE_PHYSICAL) failed, error=" +
                   std::to_string(writeResult.io.win32Error));
            return writeResult;
        }
        if (writeResult.io.bytesReturned < sizeof(response))
        {
            writeResult.io.ok = false;
            writeResult.io.message =
                "ddma write response too small, bytesReturned=" +
                std::to_string(writeResult.io.bytesReturned);
            return writeResult;
        }

        writeResult.version = static_cast<std::uint32_t>(response.version);
        writeResult.fieldFlags = static_cast<std::uint32_t>(response.fieldFlags);
        writeResult.writeStatus = static_cast<std::uint32_t>(response.writeStatus);
        writeResult.mapStatus = static_cast<long>(response.mapStatus);
        writeResult.backupStatus = static_cast<long>(response.backupStatus);
        writeResult.stageOutStatus = static_cast<long>(response.stageOutStatus);
        writeResult.stageInStatus = static_cast<long>(response.stageInStatus);
        writeResult.restoreStatus = static_cast<long>(response.restoreStatus);
        writeResult.readbackStatus = static_cast<long>(response.readbackStatus);
        writeResult.requestedPhysicalAddress =
            static_cast<std::uint64_t>(response.requestedPhysicalAddress);
        writeResult.scratchLba = static_cast<std::uint64_t>(response.scratchLba);
        writeResult.diskIndex = static_cast<std::uint32_t>(response.diskIndex);
        writeResult.requestedBytes = static_cast<std::uint32_t>(response.requestedBytes);
        writeResult.bytesWritten = static_cast<std::uint32_t>(response.bytesWritten);
        writeResult.maxBytesPerRequest = static_cast<std::uint32_t>(response.maxBytesPerRequest);
        if ((writeResult.fieldFlags & KSWORD_ARK_DDMA_FIELD_DEVICE_NAME_PRESENT) != 0UL)
        {
            writeResult.deviceName =
                copyFixedWideString(response.deviceName, KSWORD_ARK_DDMA_DEVICE_NAME_CHARS);
        }

        return writeResult;
    }
}
