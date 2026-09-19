#include "KvmWatchPanel.h"

#include "KernelDisassemblyDialog.h"
#include "KvmControl.h"
#include "../Framework/DestructiveActionConfirmation.h"
#include "../Internationalization/LanguageManager.h"
#include "../theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QShowEvent>
#include <QTableWidget>
#include <QTextEdit>
#include <QVBoxLayout>

#include <thread>

namespace
{
    enum WatchColumn
    {
        WatchColumnId = 0,
        WatchColumnTarget,
        WatchColumnRequestedRange,
        WatchColumnPage,
        WatchColumnRequestedAccess,
        WatchColumnEffectiveAccess,
        WatchColumnMode,
        WatchColumnState,
        WatchColumnHits,
        WatchColumnLastRip,
        WatchColumnModule,
        WatchColumnCount
    };

    QString hex64(const unsigned long long value)
    {
        return QStringLiteral("0x%1")
            .arg(value, 16, 16, QLatin1Char('0')).toUpper();
    }

    QString text(const QString& source)
    {
        return ks::i18n::sourceText(source);
    }

    /* 解析可带 0x 前缀的十六进制。 */
    bool parseHex(const QString& input, unsigned long long* valueOut)
    {
        QString compact = input.trimmed();
        if (compact.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
        {
            compact = compact.mid(2);
        }
        // 允许分隔用的反引号：调试器和本程序自己都用它显示 64 位地址，
        // 用户从别处复制过来的地址十有八九带着它。
        compact.remove(QLatin1Char('`'));
        if (compact.isEmpty())
        {
            return false;
        }
        bool converted = false;
        const unsigned long long value = compact.toULongLong(&converted, 16);
        if (!converted)
        {
            return false;
        }
        *valueOut = value;
        return true;
    }

    /*
     * 安装对话框。
     *
     * 下方那三行说明不是装饰：它把"你要求的"与"硬件实际会监视的"两件事并排写出来，
     * 并随输入实时更新。没有它，同一个界面就会让用户以为自己建了一个 8 字节断点。
     */
    class KvmWatchAddDialog final : public QDialog
    {
    public:
        explicit KvmWatchAddDialog(QWidget* const parent)
            : QDialog(parent)
        {
            setWindowTitle(text(QStringLiteral("添加内存监视")));
            setObjectName(QStringLiteral("KvmWatchAddDialog"));
            auto* const rootLayout = new QVBoxLayout(this);
            auto* const form = new QFormLayout();

            m_addressKind = new QComboBox(this);
            m_addressKind->addItem(
                text(QStringLiteral("内核虚拟地址")), true);
            m_addressKind->addItem(
                text(QStringLiteral("物理地址")), false);
            form->addRow(text(QStringLiteral("地址类型")), m_addressKind);

            m_address = new QLineEdit(this);
            m_address->setPlaceholderText(QStringLiteral("FFFFF80112345678"));
            form->addRow(
                text(QStringLiteral("地址（十六进制）")), m_address);

            m_length = new QLineEdit(this);
            m_length->setPlaceholderText(QStringLiteral("8"));
            m_length->setToolTip(text(QStringLiteral("你真正关心的字节数。它不改变硬件监视的范围（那永远是整页），只决定命中后能不能判断这次访问落在你关心的那几个字节上。留空表示整页。")));
            form->addRow(
                text(QStringLiteral("关心的长度（十进制字节）")), m_length);
            rootLayout->addLayout(form);

            auto* const accessRow = new QGridLayout();
            m_read = new QCheckBox(text(QStringLiteral("读")), this);
            m_write = new QCheckBox(text(QStringLiteral("写")), this);
            m_execute = new QCheckBox(text(QStringLiteral("执行")), this);
            m_write->setChecked(true);
            accessRow->addWidget(
                new QLabel(text(QStringLiteral("监视的访问类型")), this), 0, 0);
            accessRow->addWidget(m_read, 0, 1);
            accessRow->addWidget(m_write, 0, 2);
            accessRow->addWidget(m_execute, 0, 3);
            rootLayout->addLayout(accessRow);

            auto* const modeLabel = new QLabel(
                text(QStringLiteral("模式：首次访问（当前唯一支持）")), this);
            rootLayout->addWidget(modeLabel);

            m_granularity = new QLabel(this);
            m_granularity->setWordWrap(true);
            m_granularity->setStyleSheet(
                QStringLiteral("color:%1;")
                    .arg(KswordTheme::TextSecondaryHex()));
            rootLayout->addWidget(m_granularity);

            auto* const buttons = new QDialogButtonBox(
                QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
            buttons->button(QDialogButtonBox::Ok)->setText(
                text(QStringLiteral("武装")));
            rootLayout->addWidget(buttons);
            connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
            connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
            connect(m_length, &QLineEdit::textChanged, this, [this](const QString&) {
                updateGranularity();
            });
            connect(m_read, &QCheckBox::toggled, this, [this](bool) {
                updateGranularity();
            });
            updateGranularity();
            resize(520, 320);
        }

        ksword::kvm::KvmWatchTarget target() const
        {
            ksword::kvm::KvmWatchTarget result;
            result.virtualAddress = m_addressKind->currentData().toBool();
            unsigned long long address = 0;
            if (parseHex(m_address->text(), &address))
            {
                result.address = address;
            }
            bool converted = false;
            const unsigned long long length =
                m_length->text().trimmed().toULongLong(&converted, 10);
            result.length = converted ? length : 0ULL;
            result.access =
                (m_read->isChecked() ? KSWORD_ARK_HVM_EPT_ACCESS_READ : 0UL) |
                (m_write->isChecked() ? KSWORD_ARK_HVM_EPT_ACCESS_WRITE : 0UL) |
                (m_execute->isChecked() ? KSWORD_ARK_HVM_EPT_ACCESS_EXECUTE : 0UL);
            return result;
        }

        bool addressValid() const
        {
            unsigned long long address = 0;
            return parseHex(m_address->text(), &address) && address != 0;
        }

    private:
        void updateGranularity()
        {
            bool converted = false;
            const unsigned long long length =
                m_length->text().trimmed().toULongLong(&converted, 10);
            const unsigned long long requested =
                converted && length != 0ULL ? length : 4096ULL;
            QString note = text(QStringLiteral("EPT 的监视单位是页：你请求 %1 字节，实际装到硬件上的是它所在的整个 4096 字节页。命中后如果 CPU 报告了有效的客户线性地址，界面会另外告诉你这次访问是否落在你请求的那一段里。"))
                .arg(requested);
            if (m_read->isChecked())
            {
                // 这句必须在勾"读"的时候就出现，而不是等安装完才在表里被发现：
                // 用户是在这一刻决定要不要接受"连写也会被监视"的。
                note += QLatin1Char('\n');
                note += text(QStringLiteral("已勾选“读”：EPT 不允许可写而不可读，所以实际生效的监视一定同时包含写；处理器不支持仅执行叶项时还会连带包含执行。表格里的“实际访问”一栏显示归一化后的结果。"));
            }
            m_granularity->setText(note);
        }

        QComboBox* m_addressKind = nullptr;
        QLineEdit* m_address = nullptr;
        QLineEdit* m_length = nullptr;
        QCheckBox* m_read = nullptr;
        QCheckBox* m_write = nullptr;
        QCheckBox* m_execute = nullptr;
        QLabel* m_granularity = nullptr;
    };
}

KvmWatchPanel::KvmWatchPanel(QWidget* const parent)
    : QWidget(parent)
{
    buildUi();
    updateEnabledState();
}

void KvmWatchPanel::buildUi()
{
    auto* const rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(6, 6, 6, 6);
    rootLayout->setSpacing(6);

    m_hintLabel = new QLabel(
        text(QStringLiteral("监视一个目标页的下一次访问，命中时记下访问者的现场，然后自动解除并让原访问正常继续 —— 常驻不会因此退出。这是观察与归因，不是保护：命中不阻止访问，监视单位是 4 KiB 页而不是你选的字节数，DMA 改写不经过 CPU 的 EPT，目标把自己那一页换个物理页就不在被监视的页上了。")),
        this);
    m_hintLabel->setWordWrap(true);
    rootLayout->addWidget(m_hintLabel);

    m_table = new QTableWidget(0, WatchColumnCount, this);
    m_table->setObjectName(QStringLiteral("KvmWatchTable"));
    m_table->setHorizontalHeaderLabels(QStringList()
        << text(QStringLiteral("编号"))
        << text(QStringLiteral("目标"))
        << text(QStringLiteral("请求范围"))
        << text(QStringLiteral("监视页"))
        << text(QStringLiteral("请求访问"))
        << text(QStringLiteral("实际访问"))
        << text(QStringLiteral("模式"))
        << text(QStringLiteral("状态"))
        << text(QStringLiteral("命中"))
        << text(QStringLiteral("最近 RIP"))
        << text(QStringLiteral("模块")));
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->verticalHeader()->setVisible(false);
    rootLayout->addWidget(m_table, 3);

    auto* const buttons = new QGridLayout();
    m_addButton = new QPushButton(
        text(QStringLiteral("添加监视...")), this);
    m_rearmButton = new QPushButton(
        text(QStringLiteral("重新武装")), this);
    m_rearmButton->setToolTip(text(QStringLiteral("保留编号与累计命中次数，让这条监视再等下一次访问。已命中和已失效的都可以重新武装。")));
    m_removeButton = new QPushButton(
        text(QStringLiteral("移除")), this);
    m_refreshButton = new QPushButton(
        text(QStringLiteral("刷新")), this);
    m_disassembleButton = new QPushButton(
        text(QStringLiteral("查看写入者反汇编")), this);
    m_copyButton = new QPushButton(
        text(QStringLiteral("复制证据")), this);
    buttons->addWidget(m_addButton, 0, 0);
    buttons->addWidget(m_rearmButton, 0, 1);
    buttons->addWidget(m_removeButton, 0, 2);
    buttons->addWidget(m_refreshButton, 0, 3);
    buttons->addWidget(m_disassembleButton, 0, 4);
    buttons->addWidget(m_copyButton, 0, 5);
    rootLayout->addLayout(buttons);

    m_detail = new QTextEdit(this);
    m_detail->setReadOnly(true);
    m_detail->setLineWrapMode(QTextEdit::NoWrap);
    m_detail->setPlaceholderText(
        text(QStringLiteral("选中一条监视查看它的完整现场与归因。")));
    rootLayout->addWidget(m_detail, 2);

    m_statusLabel = new QLabel(QString(), this);
    m_statusLabel->setWordWrap(true);
    rootLayout->addWidget(m_statusLabel);

    connect(m_addButton, &QPushButton::clicked, this, [this]() { startAdd(); });
    connect(m_rearmButton, &QPushButton::clicked, this, [this]() { startRearm(); });
    connect(m_removeButton, &QPushButton::clicked, this, [this]() { startRemove(); });
    connect(m_refreshButton, &QPushButton::clicked, this, [this]() { refreshAsync(); });
    connect(m_disassembleButton, &QPushButton::clicked, this, [this]() {
        openWriterDisassembly();
    });
    connect(m_copyButton, &QPushButton::clicked, this, [this]() { copyEvidence(); });
    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this]() {
        ksword::kvm::KvmWatchEntry entry;
        if (selectedWatch(&entry))
        {
            showDetail(entry);
        }
        updateEnabledState();
    });
}

void KvmWatchPanel::showEvent(QShowEvent* const event)
{
    QWidget::showEvent(event);
    refreshAsync();
}

void KvmWatchPanel::setBusy(const bool busy)
{
    m_busy = busy;
    updateEnabledState();
    if (onBusyChanged)
    {
        onBusyChanged(busy);
    }
}

void KvmWatchPanel::updateEnabledState()
{
    const bool writeAllowed = ksword::kvm::isWriteAccessEnabled();
    const QString writeHint = writeAllowed
        ? QString()
        : text(QStringLiteral("R-1 写权限未开启：在 KVM 按钮右键菜单中开启后才能安装或撤销监视"));
    ksword::kvm::KvmWatchEntry entry;
    const bool hasSelection = selectedWatch(&entry);

    if (m_addButton != nullptr)
    {
        m_addButton->setEnabled(writeAllowed && !m_busy);
        m_addButton->setToolTip(writeHint);
    }
    if (m_rearmButton != nullptr)
    {
        m_rearmButton->setEnabled(writeAllowed && !m_busy && hasSelection);
        m_rearmButton->setToolTip(writeHint);
    }
    if (m_removeButton != nullptr)
    {
        m_removeButton->setEnabled(writeAllowed && !m_busy && hasSelection);
        m_removeButton->setToolTip(writeHint);
    }
    if (m_refreshButton != nullptr)
    {
        m_refreshButton->setEnabled(!m_busy);
    }
    // 反汇编与复制证据只在真的有一次命中之后才有东西可看。
    const bool hasHit = hasSelection && entry.hitCount != 0UL;
    if (m_disassembleButton != nullptr)
    {
        m_disassembleButton->setEnabled(!m_busy && hasHit && entry.lastHitRip != 0ULL);
    }
    if (m_copyButton != nullptr)
    {
        m_copyButton->setEnabled(hasSelection);
    }
}

bool KvmWatchPanel::selectedWatch(
    ksword::kvm::KvmWatchEntry* const entryOut) const
{
    if (m_table == nullptr)
    {
        return false;
    }
    const int row = m_table->currentRow();
    if (row < 0 || m_table->item(row, WatchColumnId) == nullptr)
    {
        return false;
    }
    const QVariant stored =
        m_table->item(row, WatchColumnId)->data(Qt::UserRole);
    if (!stored.isValid())
    {
        return false;
    }
    // 整条快照存在行上而不是逐列反解析：表格里的每一列都是给人读的文字，
    // 从文字反推回数值会在第一个本地化的词上出错。
    *entryOut = stored.value<ksword::kvm::KvmWatchEntry>();
    return true;
}

void KvmWatchPanel::refreshAsync()
{
    if (m_queryInFlight || m_busy)
    {
        return;
    }
    m_queryInFlight = true;
    QPointer<KvmWatchPanel> safeThis(this);
    std::thread([safeThis]() {
        const ksword::kvm::KvmWatchResult result = ksword::kvm::listWatches();
        if (safeThis == nullptr)
        {
            return;
        }
        QMetaObject::invokeMethod(
            safeThis,
            [safeThis, result]() {
                if (safeThis == nullptr)
                {
                    return;
                }
                safeThis->m_queryInFlight = false;
                if (!result.ok)
                {
                    safeThis->m_statusLabel->setText(result.message);
                    return;
                }
                safeThis->applyWatches(result.watches);
                safeThis->m_statusLabel->setText(
                    text(QStringLiteral("当前有 %1 条内存监视。"))
                        .arg(result.watchCount));
            },
            Qt::QueuedConnection);
    }).detach();
}

void KvmWatchPanel::applyWatches(
    const QVector<ksword::kvm::KvmWatchEntry>& watches)
{
    m_table->setRowCount(watches.size());
    for (int row = 0; row < watches.size(); ++row)
    {
        const ksword::kvm::KvmWatchEntry& entry = watches.at(row);
        const auto setCell = [this, row](const int column, const QString& value) {
            auto* const item = new QTableWidgetItem(value);
            item->setToolTip(value);
            m_table->setItem(row, column, item);
            return item;
        };
        auto* const idItem = setCell(
            WatchColumnId, QString::number(entry.watchId));
        idItem->setData(Qt::UserRole, QVariant::fromValue(entry));
        setCell(WatchColumnTarget,
            entry.addressKind == KSWORD_ARK_HVM_WATCH_ADDRESS_VIRTUAL
                ? text(QStringLiteral("虚拟 %1")).arg(hex64(entry.requestedAddress))
                : text(QStringLiteral("物理 %1")).arg(hex64(entry.requestedAddress)));
        setCell(WatchColumnRequestedRange,
            text(QStringLiteral("%1 字节")).arg(entry.requestedLength));
        // 监视页那一栏把 4096 明写出来：两栏并排才说得清粒度差别。
        setCell(WatchColumnPage,
            text(QStringLiteral("%1（4096 字节）")).arg(hex64(entry.physicalPage)));
        setCell(WatchColumnRequestedAccess,
            ksword::kvm::describeWatchAccess(entry.requestedAccess));
        setCell(WatchColumnEffectiveAccess,
            ksword::kvm::describeWatchAccess(entry.effectiveAccess));
        setCell(WatchColumnMode, text(QStringLiteral("首次访问")));
        setCell(WatchColumnState,
            ksword::kvm::describeWatchState(entry.state));
        // 命中列同时承载"证据在不在"：命中过但事件丢了，与从未命中，
        // 在事件列表里长得一样而结论相反。
        setCell(WatchColumnHits,
            entry.lastHitStatus == KSWORD_ARK_HVM_EPT_WATCH_HIT_EVENT_LOST
                ? text(QStringLiteral("%1（事件已丢失）")).arg(entry.hitCount)
                : QString::number(entry.hitCount));
        setCell(WatchColumnLastRip,
            entry.lastHitRip != 0ULL ? hex64(entry.lastHitRip) : QStringLiteral("-"));
        QString moduleText = QStringLiteral("-");
        if (entry.lastHitRip != 0ULL)
        {
            const ksword::kvm::KvmWatchAttribution attribution =
                ksword::kvm::attributeKernelAddress(entry.lastHitRip);
            moduleText = attribution.resolved
                ? QStringLiteral("%1+0x%2")
                    .arg(attribution.moduleName)
                    .arg(attribution.relativeAddress, 0, 16)
                : text(QStringLiteral("未知可执行区域"));
        }
        setCell(WatchColumnModule, moduleText);
    }
    m_table->resizeColumnsToContents();
    updateEnabledState();
}

void KvmWatchPanel::showDetail(const ksword::kvm::KvmWatchEntry& entry)
{
    QStringList lines;
    lines << text(QStringLiteral("目标"));
    lines << text(QStringLiteral("  请求地址        %1"))
        .arg(hex64(entry.requestedAddress));
    lines << text(QStringLiteral("  请求长度        %1 字节"))
        .arg(entry.requestedLength);
    lines << text(QStringLiteral("  实际监视页      %1，4096 字节"))
        .arg(hex64(entry.physicalPage));
    lines << text(QStringLiteral("  请求访问        %1"))
        .arg(ksword::kvm::describeWatchAccess(entry.requestedAccess));
    lines << text(QStringLiteral("  实际访问        %1"))
        .arg(ksword::kvm::describeWatchAccess(entry.effectiveAccess));
    lines << text(QStringLiteral("  模式            首次访问"));
    lines << QString();

    // 虚拟地址重映射检测。
    //
    // 这条监视绑死在武装那一刻解析出来的物理页上，不会跟着 VA 的新映射走。
    // 检测不出来时**不能**继续显示成"正在监视该虚拟地址"——那是一句读起来
    // 正确、实际可能完全不成立的话。
    if (entry.addressKind == KSWORD_ARK_HVM_WATCH_ADDRESS_VIRTUAL &&
        entry.requestedAddress != 0ULL)
    {
        const ksword::kvm::KvmMemoryResult current =
            ksword::kvm::translate(0, entry.requestedAddress);
        if (current.ok && current.physicalAddress != 0ULL)
        {
            const unsigned long long currentPage =
                current.physicalAddress & ~0xFFFULL;
            lines << (currentPage == entry.physicalPage
                ? text(QStringLiteral("映射核对        当前虚拟地址仍然落在被监视的那一页上。"))
                : text(QStringLiteral("映射核对        **当前虚拟地址已经指向 %1，与武装时的 %2 不是同一页。这条监视仍然盯着武装时那一页，不再对应该虚拟地址。**"))
                    .arg(hex64(currentPage))
                    .arg(hex64(entry.physicalPage)));
        }
        else
        {
            lines << text(QStringLiteral("映射核对        当前翻译不出物理页，无法核对该虚拟地址是否还指向被监视的那一页。"));
        }
        lines << QString();
    }

    lines << text(QStringLiteral("命中"));
    if (entry.hitCount == 0UL)
    {
        lines << text(QStringLiteral("  尚未命中。"));
    }
    else
    {
        lines << text(QStringLiteral("  累计命中        %1 次"))
            .arg(entry.hitCount);
        lines << text(QStringLiteral("  事件序号        %1"))
            .arg(entry.lastHitSequence);
        lines << text(QStringLiteral("  证据状态        %1"))
            .arg(entry.lastHitStatus ==
                    KSWORD_ARK_HVM_EPT_WATCH_HIT_PUBLISHED
                ? text(QStringLiteral("事件已发布"))
                : text(QStringLiteral("已命中，但事件环没接住这条证据")));
        lines << text(QStringLiteral("  处理器          %1:%2"))
            .arg(entry.lastHitProcessorGroup)
            .arg(entry.lastHitProcessorNumber);
        lines << text(QStringLiteral("  客户物理地址    %1"))
            .arg(hex64(entry.lastHitGuestPhysicalAddress));
        lines << text(QStringLiteral("  客户线性地址    %1"))
            .arg(entry.lastHitGuestLinearValid
                ? hex64(entry.lastHitGuestLinearAddress)
                : text(QStringLiteral("处理器未报告")));
        // 范围命中只在 CPU 给了有效线性地址时才有意义；给不出时说"无法判断"，
        // 而不是默认成"不在范围内"。
        lines << text(QStringLiteral("  落在请求范围内  %1"))
            .arg(!entry.lastHitGuestLinearValid
                ? text(QStringLiteral("无法判断（没有有效的客户线性地址）"))
                : entry.lastHitRangeMatch
                    ? text(QStringLiteral("是"))
                    : text(QStringLiteral("否，落在同一页的其它偏移上")));
        lines << text(QStringLiteral("  RIP             %1"))
            .arg(hex64(entry.lastHitRip));
        lines << text(QStringLiteral("  RSP             %1"))
            .arg(hex64(entry.lastHitRsp));
        lines << text(QStringLiteral("  CR3             %1"))
            .arg(entry.lastHitCr3 != 0ULL
                ? hex64(entry.lastHitCr3)
                : text(QStringLiteral("未采集")));
        lines << QString();
        lines << text(QStringLiteral("归因"));
        const ksword::kvm::KvmWatchAttribution attribution =
            ksword::kvm::attributeKernelAddress(entry.lastHitRip);
        if (attribution.resolved)
        {
            lines << text(QStringLiteral("  模块            %1"))
                .arg(attribution.moduleName);
            lines << text(QStringLiteral("  模块路径        %1"))
                .arg(attribution.modulePath);
            lines << text(QStringLiteral("  模块内偏移      +0x%1"))
                .arg(attribution.relativeAddress, 0, 16);
            lines << text(QStringLiteral("  符号            本版本不解析符号，请用“查看写入者反汇编”。"));
        }
        else
        {
            // 归不到模块是一条结论而不是失败：它本身就是可疑的读数。
            lines << text(QStringLiteral("  模块            未知可执行区域 —— 这个 RIP 不落在任何已加载内核模块的映像范围内。"));
            lines << text(QStringLiteral("                  用“查看写入者反汇编”直接看那一段代码。"));
        }
        lines << text(QStringLiteral("  进程            本版本不从 CR3 反解进程；CR3 原值在上面，解析属于后处理。"));
    }
    lines << QString();
    lines << text(QStringLiteral("HVM"));
    lines << text(QStringLiteral("  监视状态        %1"))
        .arg(ksword::kvm::describeWatchState(entry.state));
    lines << text(QStringLiteral("  武装代次        %1"))
        .arg(entry.armedGeneration);
    m_detail->setPlainText(lines.join(QLatin1Char('\n')));
}

void KvmWatchPanel::startAdd()
{
    KvmWatchAddDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted)
    {
        return;
    }
    if (!dialog.addressValid())
    {
        m_statusLabel->setText(
            text(QStringLiteral("地址不是合法的非零十六进制数。")));
        return;
    }
    const ksword::kvm::KvmWatchTarget target = dialog.target();
    if (target.access == 0UL)
    {
        m_statusLabel->setText(
            text(QStringLiteral("请至少选择一种要监视的访问类型。")));
        return;
    }
    setBusy(true);
    m_statusLabel->setText(text(QStringLiteral("正在安装监视...")));
    QPointer<KvmWatchPanel> safeThis(this);
    std::thread([safeThis, target]() {
        const ksword::kvm::KvmWatchResult result =
            ksword::kvm::addWatch(target);
        if (safeThis == nullptr)
        {
            return;
        }
        QMetaObject::invokeMethod(
            safeThis,
            [safeThis, result]() {
                if (safeThis == nullptr)
                {
                    return;
                }
                safeThis->setBusy(false);
                safeThis->m_statusLabel->setText(result.message);
                safeThis->refreshAsync();
            },
            Qt::QueuedConnection);
    }).detach();
}

void KvmWatchPanel::startRearm()
{
    ksword::kvm::KvmWatchEntry entry;
    if (!selectedWatch(&entry))
    {
        m_statusLabel->setText(
            text(QStringLiteral("请先在表中选择一条监视。")));
        return;
    }
    setBusy(true);
    m_statusLabel->setText(text(QStringLiteral("正在重新武装...")));
    const unsigned long watchId = entry.watchId;
    QPointer<KvmWatchPanel> safeThis(this);
    std::thread([safeThis, watchId]() {
        const ksword::kvm::KvmWatchResult result =
            ksword::kvm::rearmWatch(watchId);
        if (safeThis == nullptr)
        {
            return;
        }
        QMetaObject::invokeMethod(
            safeThis,
            [safeThis, result]() {
                if (safeThis == nullptr)
                {
                    return;
                }
                safeThis->setBusy(false);
                safeThis->m_statusLabel->setText(result.message);
                safeThis->refreshAsync();
            },
            Qt::QueuedConnection);
    }).detach();
}

void KvmWatchPanel::startRemove()
{
    ksword::kvm::KvmWatchEntry entry;
    if (!selectedWatch(&entry))
    {
        m_statusLabel->setText(
            text(QStringLiteral("请先在表中选择一条监视。")));
        return;
    }
    setBusy(true);
    m_statusLabel->setText(text(QStringLiteral("正在移除监视...")));
    const unsigned long watchId = entry.watchId;
    QPointer<KvmWatchPanel> safeThis(this);
    std::thread([safeThis, watchId]() {
        const ksword::kvm::KvmWatchResult result =
            ksword::kvm::removeWatch(watchId);
        if (safeThis == nullptr)
        {
            return;
        }
        QMetaObject::invokeMethod(
            safeThis,
            [safeThis, result]() {
                if (safeThis == nullptr)
                {
                    return;
                }
                safeThis->setBusy(false);
                safeThis->m_statusLabel->setText(result.message);
                safeThis->refreshAsync();
            },
            Qt::QueuedConnection);
    }).detach();
}

void KvmWatchPanel::openWriterDisassembly()
{
    ksword::kvm::KvmWatchEntry entry;
    if (!selectedWatch(&entry) ||
        entry.lastHitRip == 0ULL)
    {
        return;
    }
    /*
     * 从命中的 RIP 往前退一点再开始反汇编。
     *
     * RIP 指向的是**尚未完成**的那条指令：EPT violation 发生在导致访问的指令
     * 退休之前。只从 RIP 开始看，用户看到的是那条指令本身，看不到它前面几条
     * 在算什么地址——而"这个写是怎么被算出来的"往往才是要找的东西。
     */
    const unsigned long long start = entry.lastHitRip >= 0x40ULL
        ? entry.lastHitRip - 0x40ULL
        : entry.lastHitRip;
    ks::ui::KernelDisassemblyDialog::openKernelAddress(
        this,
        start,
        text(QStringLiteral("内存监视 #%1 命中的 RIP %2"))
            .arg(entry.watchId)
            .arg(hex64(entry.lastHitRip)),
        0x200U);
}

void KvmWatchPanel::copyEvidence()
{
    ksword::kvm::KvmWatchEntry entry;
    if (!selectedWatch(&entry))
    {
        return;
    }
    // 直接复制详情框的原文：屏幕上看到的和粘贴出去的必须是同一份东西，
    // 另拼一份格式会让两者随时间漂开。
    showDetail(entry);
    QApplication::clipboard()->setText(m_detail->toPlainText());
    m_statusLabel->setText(
        text(QStringLiteral("已把这条监视的完整证据复制到剪贴板。")));
}
