// KvmWatch：R-1 内存监视（首次访问归因）的门面实现。
//
// 单独成文件而不是塞进 KvmControl.cpp：这一组是唯一需要拉进 Windows 模块枚举
// 的门面成员，而 KvmControl.cpp 至今只依赖 ArkDriverClient 与 Qt。把
// NtQuerySystemInformation 拖进那个翻译单元，等于让每一个只想读 KVM 状态的
// 调用点都跟着背上它。

#include "KvmControl.h"

#include "../Internationalization/LanguageManager.h"

#include <QLibrary>

#include <algorithm>
#include <vector>

namespace ksword::kvm
{
    namespace
    {
        /* SystemModuleInformation。ntdll 不导出这个常量，只能写死。 */
        constexpr unsigned long kSystemModuleInformationClass = 11UL;

        /*
         * RTL_PROCESS_MODULE_INFORMATION 的本地复刻。
         *
         * 不从 WDK 头里取：这是用户态代码，而那个结构在公开的用户态 SDK 里没有
         * 定义。字段布局自 Windows XP 起未变，越界风险由下面按 count 逐行读、
         * 且缓冲区长度由内核自己回报来兜住。
         */
        struct SystemModuleRow
        {
            void* section;
            void* mappedBase;
            void* imageBase;
            unsigned long imageSize;
            unsigned long flags;
            unsigned short loadOrderIndex;
            unsigned short initOrderIndex;
            unsigned short loadCount;
            unsigned short fileNameOffset;
            unsigned char fullPathName[256];
        };

        struct SystemModuleList
        {
            unsigned long count;
            SystemModuleRow rows[1];
        };

        using NtQuerySystemInformationFunction =
            long(__stdcall*)(unsigned long, void*, unsigned long, unsigned long*);

        QString ruleStatusText(const unsigned long status)
        {
            switch (status)
            {
            case KSWORD_ARK_HVM_EPT_RULE_STATUS_INVALID_REQUEST:
                return ks::i18n::sourceText(QStringLiteral("请求不合法"));
            case KSWORD_ARK_HVM_EPT_RULE_STATUS_CONFIRMATION_REQUIRED:
                return ks::i18n::sourceText(QStringLiteral("需要显式确认"));
            case KSWORD_ARK_HVM_EPT_RULE_STATUS_NOT_PREPARED:
                return ks::i18n::sourceText(QStringLiteral("资源尚未准备"));
            case KSWORD_ARK_HVM_EPT_RULE_STATUS_NOT_FOUND:
                return ks::i18n::sourceText(QStringLiteral("没有这条监视"));
            case KSWORD_ARK_HVM_EPT_RULE_STATUS_TABLE_FULL:
                return ks::i18n::sourceText(QStringLiteral("规则表已满"));
            case KSWORD_ARK_HVM_EPT_RULE_STATUS_SPLIT_FAILED:
                return ks::i18n::sourceText(QStringLiteral("目标页无法拆成 4 KiB 叶"));
            case KSWORD_ARK_HVM_EPT_RULE_STATUS_PARTIAL:
                return ks::i18n::sourceText(QStringLiteral("部分处理器未能完成失效"));
            case KSWORD_ARK_HVM_EPT_RULE_STATUS_UNIMPLEMENTED:
                return ks::i18n::sourceText(QStringLiteral("这条处置当前未实现"));
            case KSWORD_ARK_HVM_EPT_RULE_STATUS_MULTIPROCESSOR_UNSAFE:
                return ks::i18n::sourceText(QStringLiteral("这台机器上无法安全实现该处置"));
            case KSWORD_ARK_HVM_EPT_RULE_STATUS_LEAF_CONFLICT:
                return ks::i18n::sourceText(QStringLiteral("这一页已经被别的 EPT 机制占着"));
            case KSWORD_ARK_HVM_EPT_RULE_STATUS_RESIDENT_FROZEN:
                return ks::i18n::sourceText(QStringLiteral("常驻运行中：EPT 规则表在常驻期间冻结，安装与撤销都要先停止常驻"));
            default:
                break;
            }
            return ks::i18n::sourceText(QStringLiteral("协议状态 %1")).arg(status);
        }

        /* 把一行协议快照翻译成 UI 结构。 */
        KvmWatchEntry toWatchEntry(const KSWORD_ARK_HVM_EPT_WATCH_ROW& row)
        {
            KvmWatchEntry entry;
            entry.watchId = row.watchId;
            entry.state = row.state;
            entry.requestedAccess = row.requestedAccess;
            entry.effectiveAccess = row.effectiveAccess;
            entry.addressKind = row.addressKind;
            entry.hitCount = row.hitCount;
            entry.lastHitSequence = row.lastHitSequence;
            entry.lastHitStatus = row.lastHitStatus;
            entry.armedGeneration = row.armedGeneration;
            entry.requestedAddress = row.requestedAddress;
            entry.requestedLength = row.requestedLength;
            entry.physicalPage = row.physicalPage;
            entry.pageCount = row.pageCount;
            entry.lastHitRip = row.lastHitRip;
            entry.lastHitGuestLinearAddress = row.lastHitGuestLinearAddress;
            entry.lastHitGuestPhysicalAddress = row.lastHitGuestPhysicalAddress;
            entry.lastHitCr3 = row.lastHitCr3;
            entry.lastHitRsp = row.lastHitRsp;
            entry.lastHitTimestamp = row.lastHitTimestamp;
            entry.lastHitProcessorGroup = row.lastHitProcessorGroup;
            entry.lastHitProcessorNumber = row.lastHitProcessorNumber;
            entry.lastHitGuestLinearValid = row.lastHitGuestLinearValid != 0U;
            entry.lastHitRangeMatch = row.lastHitRangeMatch != 0UL;
            return entry;
        }

        /* 把驱动响应翻译成 UI 可直接展示的结论。 */
        KvmWatchResult toWatchResult(
            const ksword::ark::HvmEptRuleResult& result,
            const QString& actionName)
        {
            KvmWatchResult watch;
            watch.protocolStatus = result.response.status;
            watch.lastStatus = result.response.lastStatus;
            watch.conflictOwnerId = result.response.conflictOwnerId;
            watch.conflictOwnerKind = result.response.conflictOwnerKind;
            watch.ok = result.io.ok &&
                result.response.status == KSWORD_ARK_HVM_EPT_RULE_STATUS_OK;
            if (result.io.ok)
            {
                // 表在成功与失败时都回填：失败也要让调用方看见现在装着什么，
                // 否则"表已满"这类拒绝只剩一个数字，没法判断该撤哪一条。
                const unsigned long rows =
                    result.response.returnedWatchRows <=
                        KSWORD_ARK_HVM_MAX_EPT_WATCH_ROWS
                        ? result.response.returnedWatchRows
                        : KSWORD_ARK_HVM_MAX_EPT_WATCH_ROWS;
                for (unsigned long index = 0; index < rows; ++index)
                {
                    watch.watches.append(
                        toWatchEntry(result.response.watchRows[index]));
                }
                // 单条操作把它那一行也带上，调用方不必为了看结果再查一次。
                if (rows == 0 && result.response.watch.watchId != 0)
                {
                    watch.watches.append(toWatchEntry(result.response.watch));
                }
                watch.watchCount = result.response.watchRowCount != 0
                    ? result.response.watchRowCount
                    : static_cast<unsigned long>(watch.watches.size());
            }
            if (watch.ok)
            {
                watch.message = ks::i18n::sourceText(
                    QStringLiteral("%1 成功。")).arg(actionName);
                return watch;
            }
            if (!result.io.ok && result.unsupported)
            {
                watch.message = ks::i18n::sourceText(
                    QStringLiteral("%1 失败：当前驱动不提供该能力。"))
                    .arg(actionName);
                return watch;
            }
            if (result.response.status ==
                    KSWORD_ARK_HVM_EPT_RULE_STATUS_LEAF_CONFLICT)
            {
                // 冲突要指名道姓：只说"冲突"的话，用户没法知道该先撤哪一个。
                watch.message = ks::i18n::sourceText(
                    QStringLiteral("%1 失败：%2。请先移除它，再安装内存监视。"))
                    .arg(actionName)
                    .arg(describeWatchConflict(
                        result.response.conflictOwnerKind,
                        result.response.conflictOwnerId));
                return watch;
            }
            watch.message = ks::i18n::sourceText(
                QStringLiteral("%1 失败：%2。"))
                .arg(actionName)
                .arg(ruleStatusText(result.response.status));
            return watch;
        }

        /* 写权限关闭时统一拒绝，且不发起任何 IOCTL。 */
        KvmWatchResult denyWatchWithoutWriteAccess(const QString& actionName)
        {
            KvmWatchResult watch;
            watch.message = ks::i18n::sourceText(
                QStringLiteral("%1 失败：R-1 写权限未开启。"))
                .arg(actionName);
            return watch;
        }
    }

    KvmWatchResult listWatches()
    {
        ksword::ark::DriverClient client;
        ksword::ark::DriverClient::HvmEptWatchRequest request;
        request.operation = KSWORD_ARK_HVM_EPT_RULE_WATCH_QUERY;
        const auto result = client.controlHvmEptWatch(request);
        return toWatchResult(
            result,
            ks::i18n::sourceText(QStringLiteral("读取内存监视")));
    }

    KvmWatchResult addWatch(const KvmWatchTarget& target)
    {
        const QString actionName =
            ks::i18n::sourceText(QStringLiteral("安装内存监视"));
        if (!isWriteAccessEnabled())
        {
            return denyWatchWithoutWriteAccess(actionName);
        }
        if (target.access == 0UL ||
            (target.access &
                ~(KSWORD_ARK_HVM_EPT_ACCESS_READ |
                  KSWORD_ARK_HVM_EPT_ACCESS_WRITE |
                  KSWORD_ARK_HVM_EPT_ACCESS_EXECUTE)) != 0UL)
        {
            KvmWatchResult watch;
            watch.message = ks::i18n::sourceText(
                QStringLiteral("%1 失败：至少要选一种访问类型。"))
                .arg(actionName);
            return watch;
        }
        unsigned long long physicalAddress = target.address;
        if (target.virtualAddress)
        {
            // 翻译一次并就此定死。
            //
            // 这条 watch 从此绑定在**这一刻**解析出来的物理页上，之后 guest 把
            // 同一个 VA 重映射到别处也不会跟过去。这不是遗漏：跟踪重映射要监视
            // guest 页表本身，那是另一个数量级的机制。这里能做的是把绑定的时刻
            // 和结果如实记下来，让界面之后能检测出分歧并说出来。
            const KvmMemoryResult translated = translate(0, target.address);
            if (!translated.ok || translated.physicalAddress == 0)
            {
                KvmWatchResult watch;
                watch.message = ks::i18n::sourceText(
                    QStringLiteral("%1 失败：这个内核虚拟地址当前翻译不出物理页（%2）。"))
                    .arg(actionName)
                    .arg(translated.message);
                return watch;
            }
            physicalAddress = translated.physicalAddress;
        }
        ksword::ark::DriverClient client;
        ksword::ark::DriverClient::HvmEptWatchRequest request;
        request.operation = KSWORD_ARK_HVM_EPT_RULE_ADD;
        request.requestedAccess = target.access;
        request.addressKind = target.virtualAddress
            ? KSWORD_ARK_HVM_WATCH_ADDRESS_VIRTUAL
            : KSWORD_ARK_HVM_WATCH_ADDRESS_PHYSICAL;
        // 实际监视的永远是整页；请求的地址与长度另外记，不参与对齐。
        request.physicalPage = physicalAddress & ~0xFFFULL;
        request.requestedAddress = target.address;
        request.requestedLength = target.length;
        const auto result = client.controlHvmEptWatch(request);
        return toWatchResult(result, actionName);
    }

    KvmWatchResult rearmWatch(const unsigned long watchId)
    {
        const QString actionName =
            ks::i18n::sourceText(QStringLiteral("重新武装内存监视"));
        if (!isWriteAccessEnabled())
        {
            return denyWatchWithoutWriteAccess(actionName);
        }
        ksword::ark::DriverClient client;
        ksword::ark::DriverClient::HvmEptWatchRequest request;
        request.operation = KSWORD_ARK_HVM_EPT_RULE_REARM;
        request.watchId = watchId;
        const auto result = client.controlHvmEptWatch(request);
        return toWatchResult(result, actionName);
    }

    KvmWatchResult removeWatch(const unsigned long watchId)
    {
        const QString actionName =
            ks::i18n::sourceText(QStringLiteral("移除内存监视"));
        if (!isWriteAccessEnabled())
        {
            return denyWatchWithoutWriteAccess(actionName);
        }
        ksword::ark::DriverClient client;
        ksword::ark::DriverClient::HvmEptWatchRequest request;
        request.operation = KSWORD_ARK_HVM_EPT_RULE_REMOVE;
        request.watchId = watchId;
        const auto result = client.controlHvmEptWatch(request);
        return toWatchResult(result, actionName);
    }

    KvmWatchAttribution attributeKernelAddress(const unsigned long long address)
    {
        KvmWatchAttribution attribution;
        static const auto query =
            reinterpret_cast<NtQuerySystemInformationFunction>(
                QLibrary::resolve(
                    QStringLiteral("ntdll"),
                    "NtQuerySystemInformation"));

        if (query == nullptr ||
            address == 0)
        {
            // 解析不出来是一条结论，不是一次失败：调用方据此显示"未知可执行
            // 区域"并给出打开内存/反汇编的入口，而不是只写一个 Unknown。
            return attribution;
        }
        unsigned long needed = 0UL;
        // 先问长度。模块表会变，所以多要一截余量再重试一次，而不是循环到成功
        // ——在一个每秒都可能加载驱动的系统上，那种循环没有终止保证。
        (void)query(kSystemModuleInformationClass, nullptr, 0UL, &needed);
        if (needed == 0UL)
        {
            return attribution;
        }
        std::vector<unsigned char> buffer;
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            buffer.assign(needed + 0x4000U, 0U);
            unsigned long written = 0UL;
            const long status = query(
                kSystemModuleInformationClass,
                buffer.data(),
                static_cast<unsigned long>(buffer.size()),
                &written);
            if (status >= 0)
            {
                break;
            }
            if (attempt == 1)
            {
                return attribution;
            }
            needed = written != 0UL ? written : needed * 2U;
        }
        const auto* const list =
            reinterpret_cast<const SystemModuleList*>(buffer.data());
        const unsigned long count = list->count;
        const size_t capacity =
            (buffer.size() - sizeof(unsigned long)) / sizeof(SystemModuleRow);
        const unsigned long bounded = count <= capacity
            ? count
            : static_cast<unsigned long>(capacity);
        for (unsigned long index = 0; index < bounded; ++index)
        {
            const SystemModuleRow& row = list->rows[index];
            const auto base =
                reinterpret_cast<unsigned long long>(row.imageBase);
            if (row.imageSize == 0UL ||
                address < base ||
                address >= base + row.imageSize)
            {
                continue;
            }
            attribution.resolved = true;
            attribution.moduleBase = base;
            attribution.moduleSize = row.imageSize;
            attribution.relativeAddress = address - base;
            // fullPathName 是 ANSI 的 NT 路径，末尾保证有零；fileNameOffset
            // 指向其中的文件名部分。
            const auto* const path =
                reinterpret_cast<const char*>(row.fullPathName);
            const size_t limit = sizeof(row.fullPathName);
            size_t length = 0;
            while (length < limit && path[length] != '\0')
            {
                ++length;
            }
            attribution.modulePath = QString::fromLatin1(
                path, static_cast<int>(length));
            attribution.moduleName = row.fileNameOffset < length
                ? QString::fromLatin1(
                    path + row.fileNameOffset,
                    static_cast<int>(length - row.fileNameOffset))
                : attribution.modulePath;
            break;
        }
        return attribution;
    }

    QString describeWatchState(const unsigned long state)
    {
        switch (state)
        {
        case KSWORD_ARK_HVM_EPT_WATCH_STATE_ARMED:
            return ks::i18n::sourceText(QStringLiteral("监视中"));
        case KSWORD_ARK_HVM_EPT_WATCH_STATE_TRIGGERED:
            return ks::i18n::sourceText(QStringLiteral("正在处理命中"));
        case KSWORD_ARK_HVM_EPT_WATCH_STATE_DISARMED:
            return ks::i18n::sourceText(QStringLiteral("已命中并解除"));
        case KSWORD_ARK_HVM_EPT_WATCH_STATE_INVALIDATED:
            return ks::i18n::sourceText(QStringLiteral("已失效，需重新武装"));
        case KSWORD_ARK_HVM_EPT_WATCH_STATE_FAULTED:
            return ks::i18n::sourceText(QStringLiteral("安装失败"));
        default:
            break;
        }
        return ks::i18n::sourceText(QStringLiteral("未武装"));
    }

    QString describeWatchAccess(const unsigned long access)
    {
        QStringList parts;
        if ((access & KSWORD_ARK_HVM_EPT_ACCESS_READ) != 0UL)
        {
            parts << ks::i18n::sourceText(QStringLiteral("读"));
        }
        if ((access & KSWORD_ARK_HVM_EPT_ACCESS_WRITE) != 0UL)
        {
            parts << ks::i18n::sourceText(QStringLiteral("写"));
        }
        if ((access & KSWORD_ARK_HVM_EPT_ACCESS_EXECUTE) != 0UL)
        {
            parts << ks::i18n::sourceText(QStringLiteral("执行"));
        }
        if (parts.isEmpty())
        {
            return ks::i18n::sourceText(QStringLiteral("无"));
        }
        return parts.join(ks::i18n::sourceText(QStringLiteral(" + ")));
    }

    QString describeWatchConflict(
        const unsigned long ownerKind,
        const unsigned long ownerId)
    {
        switch (ownerKind)
        {
        case KSWORD_ARK_HVM_WATCH_CONFLICT_VIEW:
            return ks::i18n::sourceText(
                QStringLiteral("这一页已经被 EPT 分离视图 #%1 占着")).arg(ownerId);
        case KSWORD_ARK_HVM_WATCH_CONFLICT_RULE:
            return ks::i18n::sourceText(
                QStringLiteral("这一页已经被 EPT 规则 #%1 占着")).arg(ownerId);
        case KSWORD_ARK_HVM_WATCH_CONFLICT_WATCH:
            return ks::i18n::sourceText(
                QStringLiteral("这一页已经被内存监视 #%1 占着")).arg(ownerId);
        default:
            break;
        }
        return ks::i18n::sourceText(QStringLiteral("这一页已经被别的 EPT 机制占着"));
    }
}
