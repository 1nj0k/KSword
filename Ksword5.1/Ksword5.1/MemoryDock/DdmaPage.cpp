#include "DdmaPage.h"

#include "../theme.h"
#include "../UI/HexEditorWidget.h"
#include "../UI/VisibleTableWidget.h"

#include <QCheckBox>
#include <QEvent>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>

// ============================================================
// DdmaPage.cpp
// 作用：
// - 实现 DDMA 通道的配置、自身读写与"标准通道 vs DDMA"同址复核。
// ============================================================

namespace
{
    // kDiskTableColumns：磁盘表列定义。
    enum class DiskColumn : int
    {
        Index = 0,
        DeviceName,
        State,
        ProbeStatus,
        SectorSize,
        Count
    };

    // kCompareSampleRows：复核差异明细最多展示多少行，避免整页 4096 个差异
    // 把表格撑爆。超出部分只在结论里给总数。
    constexpr int kCompareSampleRows = 256;

    // formatNtStatus：把 NTSTATUS 渲染成 8 位大写十六进制。
    QString formatNtStatus(const long status)
    {
        return QStringLiteral("0x%1")
            .arg(static_cast<qulonglong>(static_cast<std::uint32_t>(status)), 8, 16, QChar('0'))
            .toUpper()
            .replace(QStringLiteral("0X"), QStringLiteral("0x"));
    }
}

DdmaPage::DdmaPage(QWidget* const parent)
    : QWidget(parent)
{
    initializeUi();
    refreshSessionState();
}

void DdmaPage::setSessionChangedCallback(std::function<void()> callback)
{
    m_sessionChangedCallback = std::move(callback);
}

void DdmaPage::changeEvent(QEvent* const event)
{
    QWidget::changeEvent(event);
    if (event == nullptr)
    {
        return;
    }
    // 语义色是调用瞬间的快照，深浅色切换后必须重新下发，否则颜色停在旧主题。
    if (event->type() == QEvent::ApplicationPaletteChange
        || event->type() == QEvent::PaletteChange)
    {
        applySemanticStyles();
    }
}

void DdmaPage::initializeUi()
{
    QVBoxLayout* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(6, 6, 6, 6);
    rootLayout->setSpacing(6);

    rootLayout->addWidget(buildIntroGroup());
    rootLayout->addWidget(buildChannelGroup());
    rootLayout->addWidget(buildAccessGroup(), 1);
    rootLayout->addWidget(buildCompareGroup());

    applySemanticStyles();
}

QGroupBox* DdmaPage::buildIntroGroup()
{
    QGroupBox* group = new QGroupBox("DDMA 是什么", this);
    QVBoxLayout* layout = new QVBoxLayout(group);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(4);

    QLabel* introLabel = new QLabel(group);
    introLabel->setWordWrap(true);
    introLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    introLabel->setText(
        "DDMA 让磁盘控制器用总线主控 DMA 直接读写物理地址。数据通路走 HBA 而不经过 CPU 页表，"
        "因此不受 SLAT / EPT 约束，能读到被上层虚拟化重定向或隐藏的物理页——"
        "标准通道在这些页上只会读到全 FF 或被替换过的内容。\n"
        "代价有三条，都无法绕开：\n"
        "一、必须借用一块磁盘扇区当中转站。本工具不提供默认扇区，必须由你显式指定 LBA 并确认；"
        "每次读写都在同一次请求内完成“备份→使用→还原”，但还原失败时磁盘上会留下脏扇区。\n"
        "二、开启内核调试的机器上会命中 MiShowBadMapper 直接蓝屏，此时整条通道被禁用。\n"
        "三、只支持 ATA 通道；部分 HBA 不支持 64 位寻址，高物理内存可能访问不到。");
    layout->addWidget(introLabel);

    return group;
}

QGroupBox* DdmaPage::buildChannelGroup()
{
    QGroupBox* group = new QGroupBox("通道配置", this);
    QVBoxLayout* outerLayout = new QVBoxLayout(group);
    outerLayout->setContentsMargins(8, 8, 8, 8);
    outerLayout->setSpacing(6);

    QGridLayout* formLayout = new QGridLayout();
    formLayout->setHorizontalSpacing(8);
    formLayout->setVerticalSpacing(6);

    m_scratchLbaEdit = new QLineEdit(group);
    m_scratchLbaEdit->setPlaceholderText("必填，例如 0x100000 或 1048576");
    m_scratchLbaEdit->setClearButtonEnabled(true);
    m_scratchLbaEdit->setToolTip(
        "暂存扇区起始 LBA。DDMA 必须借磁盘扇区中转，本工具不提供默认值，"
        "留空则整条通道不可用。注意 LBA 0 起的前几个扇区是 MBR / GPT 保护扇区。");

    m_scratchImpactLabel = new QLabel(group);
    m_scratchImpactLabel->setWordWrap(true);
    m_scratchImpactLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_scratchAckCheck = new QCheckBox("我确认上述扇区上的数据可以被临时覆盖", group);
    m_scratchAckCheck->setToolTip(
        "每次 DDMA 读写都会先备份这几个扇区、用完立刻还原。"
        "但操作期间发生蓝屏或断电时，这几个扇区的原始数据会丢失。");

    m_probeButton = new QPushButton(
        QIcon(QStringLiteral(":/Icon/disk_analyze.svg")), "探测可用磁盘", group);
    m_probeButton->setToolTip(
        "枚举 \\Driver\\Disk 上的磁盘设备。已填写暂存 LBA 时，"
        "会对每块盘真的发一次 ATA DMA 读命令来判定通道是否可用（只读，不写盘）。");

    m_activateButton = new QPushButton(
        QIcon(QStringLiteral(":/Icon/disk_save.svg")), "启用选中磁盘为 DDMA 通道", group);
    m_activateButton->setEnabled(false);
    m_activateButton->setToolTip(
        "把选中磁盘写入 DDMA 会话。启用后，内存搜索、内存查看器、"
        "驱动内存读写与系统内存审计四个页面都能选择 DDMA 后端。");

    m_clearButton = new QPushButton(
        QIcon(QStringLiteral(":/Icon/log_clear.svg")), "清除通道配置", group);
    m_clearButton->setToolTip("清空 DDMA 会话，所有页面立刻退回标准驱动通道。");

    formLayout->addWidget(new QLabel("暂存扇区 LBA", group), 0, 0);
    formLayout->addWidget(m_scratchLbaEdit, 0, 1);
    formLayout->addWidget(m_probeButton, 0, 2);
    formLayout->addWidget(m_scratchImpactLabel, 1, 0, 1, 3);
    formLayout->addWidget(m_scratchAckCheck, 2, 0, 1, 3);
    formLayout->setColumnStretch(1, 1);
    outerLayout->addLayout(formLayout);

    m_diskTable = new ks::ui::VisibleTableWidget(group);
    m_diskTable->setColumnCount(static_cast<int>(DiskColumn::Count));
    m_diskTable->setHorizontalHeaderLabels(
        QStringList{ "序号", "设备名", "通道状态", "探测 NTSTATUS", "扇区大小" });
    m_diskTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_diskTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_diskTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_diskTable->setAlternatingRowColors(true);
    m_diskTable->verticalHeader()->setVisible(false);
    m_diskTable->verticalHeader()->setDefaultSectionSize(22);
    m_diskTable->setMinimumHeight(120);
    outerLayout->addWidget(m_diskTable);

    QHBoxLayout* actionLayout = new QHBoxLayout();
    actionLayout->setContentsMargins(0, 0, 0, 0);
    actionLayout->setSpacing(6);
    actionLayout->addWidget(m_activateButton);
    actionLayout->addWidget(m_clearButton);
    actionLayout->addStretch(1);
    outerLayout->addLayout(actionLayout);

    m_capabilityLabel = new QLabel("尚未探测。", group);
    m_capabilityLabel->setWordWrap(true);
    m_capabilityLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    outerLayout->addWidget(m_capabilityLabel);

    m_sessionStateLabel = new QLabel(group);
    m_sessionStateLabel->setWordWrap(true);
    m_sessionStateLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    outerLayout->addWidget(m_sessionStateLabel);

    connect(m_probeButton, &QPushButton::clicked, this, [this]() { probeChannels(); });
    connect(m_activateButton, &QPushButton::clicked, this, [this]() { activateSelectedDisk(); });
    connect(m_clearButton, &QPushButton::clicked, this, [this]() { clearSession(); });
    connect(m_scratchLbaEdit, &QLineEdit::textChanged, this, [this](const QString&) {
        // LBA 变了就必须重新确认：用户很可能是把目标换到了另一段扇区上。
        if (m_scratchAckCheck != nullptr && m_scratchAckCheck->isChecked())
        {
            m_scratchAckCheck->setChecked(false);
        }
        refreshSessionState();
        });
    connect(m_scratchAckCheck, &QCheckBox::toggled, this, [this](bool) { refreshSessionState(); });
    connect(m_diskTable, &QTableWidget::itemSelectionChanged, this, [this]() {
        if (m_activateButton != nullptr)
        {
            m_activateButton->setEnabled(m_diskTable->currentRow() >= 0);
        }
        });

    return group;
}

QGroupBox* DdmaPage::buildAccessGroup()
{
    QGroupBox* group = new QGroupBox("DDMA 物理读写", this);
    QVBoxLayout* outerLayout = new QVBoxLayout(group);
    outerLayout->setContentsMargins(8, 8, 8, 8);
    outerLayout->setSpacing(6);

    QHBoxLayout* barLayout = new QHBoxLayout();
    barLayout->setContentsMargins(0, 0, 0, 0);
    barLayout->setSpacing(6);

    m_accessAddressEdit = new QLineEdit(group);
    m_accessAddressEdit->setPlaceholderText("物理地址，例如 0x1000");
    m_accessAddressEdit->setClearButtonEnabled(true);

    m_accessLengthSpin = new QSpinBox(group);
    m_accessLengthSpin->setRange(1, 64 * 1024);
    m_accessLengthSpin->setValue(4096);
    m_accessLengthSpin->setSuffix(" B");
    m_accessLengthSpin->setToolTip(
        "读取长度。DDMA 的一次 DMA 传输就是一页，超过一页会按页边界自动切片，"
        "每一页都是一次完整的“备份→传输→还原”，所以长度越大越慢。");

    m_accessReadButton = new QPushButton(
        QIcon(QStringLiteral(":/Icon/process_details.svg")), "DDMA 读取", group);
    m_accessWriteButton = new QPushButton(
        QIcon(QStringLiteral(":/Icon/disk_save.svg")), "DDMA 写回差异", group);
    m_accessWriteButton->setEnabled(false);
    m_accessWriteButton->setToolTip(
        "把下方编辑器中改动过的字节用 DDMA 写回物理内存。"
        "非整页写入时驱动会做读-改-写，同页其它字节存在覆盖窗口。");

    barLayout->addWidget(new QLabel("物理地址", group));
    barLayout->addWidget(m_accessAddressEdit, 1);
    barLayout->addWidget(new QLabel("长度", group));
    barLayout->addWidget(m_accessLengthSpin);
    barLayout->addWidget(m_accessReadButton);
    barLayout->addWidget(m_accessWriteButton);
    outerLayout->addLayout(barLayout);

    m_accessHexEditor = new HexEditorWidget(group);
    m_accessHexEditor->setBytesPerRow(16);
    m_accessHexEditor->setEditable(true);
    outerLayout->addWidget(m_accessHexEditor, 1);

    m_accessStatusLabel = new QLabel("等待读取。", group);
    m_accessStatusLabel->setWordWrap(true);
    m_accessStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    outerLayout->addWidget(m_accessStatusLabel);

    connect(m_accessReadButton, &QPushButton::clicked, this, [this]() { readPhysicalFromUi(); });
    connect(m_accessWriteButton, &QPushButton::clicked, this, [this]() { writePhysicalFromUi(); });
    connect(m_accessHexEditor, &HexEditorWidget::byteEdited, this,
        [this](const std::uint64_t absoluteAddress,
               const std::uint8_t oldValue,
               const std::uint8_t newValue) {
            Q_UNUSED(oldValue);
            // 编辑只改本地缓存，真正写回仍然要点“DDMA 写回差异”。
            if (!m_hasSnapshot || absoluteAddress < m_snapshotAddress)
            {
                return;
            }
            const std::uint64_t offset = absoluteAddress - m_snapshotAddress;
            if (offset >= static_cast<std::uint64_t>(m_editedBytes.size()))
            {
                return;
            }
            m_editedBytes[static_cast<qsizetype>(offset)] = static_cast<char>(newValue);
            if (m_accessWriteButton != nullptr)
            {
                m_accessWriteButton->setEnabled(m_editedBytes != m_originalBytes);
            }
        });

    return group;
}

QGroupBox* DdmaPage::buildCompareGroup()
{
    QGroupBox* group = new QGroupBox("标准通道 vs DDMA 同址复核", this);
    QVBoxLayout* outerLayout = new QVBoxLayout(group);
    outerLayout->setContentsMargins(8, 8, 8, 8);
    outerLayout->setSpacing(6);

    QLabel* hintLabel = new QLabel(group);
    hintLabel->setWordWrap(true);
    hintLabel->setText(
        "对同一物理地址各读一页：标准通道走 MmCopyMemory，受 SLAT 约束；"
        "DDMA 走设备 DMA，不受约束。两者不一致就是这一页被重定向或隐藏的直接证据。");
    outerLayout->addWidget(hintLabel);

    QHBoxLayout* barLayout = new QHBoxLayout();
    barLayout->setContentsMargins(0, 0, 0, 0);
    barLayout->setSpacing(6);

    m_compareAddressEdit = new QLineEdit(group);
    m_compareAddressEdit->setPlaceholderText("物理页地址，例如 0x1000");
    m_compareAddressEdit->setClearButtonEnabled(true);

    m_compareButton = new QPushButton(
        QIcon(QStringLiteral(":/Icon/file_find.svg")), "复核这一页", group);

    barLayout->addWidget(new QLabel("物理地址", group));
    barLayout->addWidget(m_compareAddressEdit, 1);
    barLayout->addWidget(m_compareButton);
    outerLayout->addLayout(barLayout);

    m_compareResultLabel = new QLabel("尚未复核。", group);
    m_compareResultLabel->setWordWrap(true);
    m_compareResultLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    outerLayout->addWidget(m_compareResultLabel);

    m_compareTable = new QTableWidget(group);
    m_compareTable->setColumnCount(3);
    m_compareTable->setHorizontalHeaderLabels(QStringList{ "偏移", "标准通道", "DDMA" });
    m_compareTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_compareTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_compareTable->setAlternatingRowColors(true);
    m_compareTable->verticalHeader()->setVisible(false);
    m_compareTable->verticalHeader()->setDefaultSectionSize(20);
    m_compareTable->setMaximumHeight(160);
    outerLayout->addWidget(m_compareTable);

    connect(m_compareButton, &QPushButton::clicked, this, [this]() { compareBackendsFromUi(); });

    return group;
}

void DdmaPage::applySemanticStyles()
{
    // 会话状态标签的颜色由 refreshSessionState 按可用性决定，这里只保证
    // 高危按钮在两种主题下都有一致的错误色描边。
    if (m_accessWriteButton != nullptr)
    {
        m_accessWriteButton->setStyleSheet(
            QStringLiteral(
                "QPushButton{border:1px solid %1;border-radius:3px;color:%1;padding:4px 10px;}"
                "QPushButton:disabled{border:1px solid %2;color:%2;}")
                .arg(KswordTheme::ErrorHex())
                .arg(KswordTheme::TextSecondaryHex()));
    }
    refreshSessionState();
}

bool DdmaPage::parseScratchLbaFromUi(std::uint64_t& lbaOut, QString& errorTextOut) const
{
    lbaOut = 0ULL;
    errorTextOut.clear();

    if (m_scratchLbaEdit == nullptr)
    {
        errorTextOut = QStringLiteral("界面尚未初始化。");
        return false;
    }

    const QString text = m_scratchLbaEdit->text().trimmed();
    if (text.isEmpty())
    {
        // 空输入一律判为"未填写"。绝不能退化成 0——LBA 0 是 MBR 所在扇区，
        // 靠默认值选中它正是这个功能最需要避免的事。
        errorTextOut = QStringLiteral("尚未填写暂存扇区 LBA。");
        return false;
    }

    std::uint64_t value = 0ULL;
    if (!parseAddressText(text, value))
    {
        errorTextOut = QStringLiteral("暂存扇区 LBA 解析失败，请填写十进制或 0x 十六进制数值。");
        return false;
    }
    // 协议按 48 位 LBA 处理，超出范围的值 R0 会拒绝，这里先本地拦下。
    constexpr std::uint64_t kLbaMax = 0x0001000000000000ULL;
    if (value >= kLbaMax)
    {
        errorTextOut = QStringLiteral("暂存扇区 LBA 超出 48 位上限。");
        return false;
    }

    lbaOut = value;
    return true;
}

void DdmaPage::probeChannels()
{
    std::uint64_t scratchLba = 0ULL;
    QString lbaError;
    const bool lbaValid = parseScratchLbaFromUi(scratchLba, lbaError);

    if (m_capabilityLabel != nullptr)
    {
        m_capabilityLabel->setText(QStringLiteral("正在探测 DDMA 通道..."));
    }

    const ksword::ark::DriverClient client;
    const ksword::ark::DdmaCapabilityResult result =
        client.queryDdmaCapability(lbaValid, scratchLba, lbaValid);

    if (!result.io.ok)
    {
        m_probeCompleted = false;
        m_diskCache.clear();
        if (m_diskTable != nullptr)
        {
            m_diskTable->setRowCount(0);
        }
        if (m_capabilityLabel != nullptr)
        {
            m_capabilityLabel->setText(
                QStringLiteral("探测失败：%1").arg(QString::fromStdString(result.io.message)));
        }
        refreshSessionState();
        return;
    }

    m_probeCompleted = true;
    m_diskCache = result.disks;
    m_kernelDebuggerEnabled = result.kernelDebuggerEnabled();
    m_transferBytes = result.transferBytes;
    m_scratchSectorCount = result.scratchSectorCount;

    // 探测结果变了，之前启用的通道未必还成立，先把会话里的探测面收回。
    m_session.kernelDebuggerEnabled = m_kernelDebuggerEnabled;
    m_session.transferBytes = m_transferBytes;
    m_session.scratchSectorCount = m_scratchSectorCount;

    if (m_diskTable != nullptr)
    {
        m_diskTable->setRowCount(static_cast<int>(m_diskCache.size()));
        for (int row = 0; row < static_cast<int>(m_diskCache.size()); ++row)
        {
            const ksword::ark::DdmaDiskEntry& entry = m_diskCache[static_cast<std::size_t>(row)];

            QString stateText;
            if (entry.ready())
            {
                stateText = QStringLiteral("可用");
            }
            else if ((entry.diskFlags & KSWORD_ARK_DDMA_DISK_FLAG_PROBE_SKIPPED) != 0UL)
            {
                stateText = QStringLiteral("未探测（需先填写暂存 LBA）");
            }
            else
            {
                stateText = QStringLiteral("不可用");
            }

            m_diskTable->setItem(row, static_cast<int>(DiskColumn::Index),
                new QTableWidgetItem(QString::number(entry.deviceIndex)));
            m_diskTable->setItem(row, static_cast<int>(DiskColumn::DeviceName),
                new QTableWidgetItem(entry.deviceName.empty()
                    ? QStringLiteral("(名称不可用)")
                    : QString::fromStdWString(entry.deviceName)));
            m_diskTable->setItem(row, static_cast<int>(DiskColumn::State),
                new QTableWidgetItem(stateText));
            m_diskTable->setItem(row, static_cast<int>(DiskColumn::ProbeStatus),
                new QTableWidgetItem(formatNtStatus(entry.probeStatus)));
            m_diskTable->setItem(row, static_cast<int>(DiskColumn::SectorSize),
                new QTableWidgetItem(QString::number(entry.sectorSize)));
        }
        m_diskTable->resizeColumnsToContents();
    }

    QString capabilityText = QStringLiteral(
        "枚举到 %1 块磁盘，其中 %2 块通过 DMA 探测。一次传输 %3 字节，占用 %4 个扇区。")
        .arg(result.totalDisks)
        .arg(result.readyDisks)
        .arg(result.transferBytes)
        .arg(result.scratchSectorCount);
    if (!lbaValid)
    {
        capabilityText += QStringLiteral(" 本次未做传输探测：%1").arg(lbaError);
    }
    if (m_kernelDebuggerEnabled)
    {
        capabilityText += QStringLiteral(
            " 本机启用了内核调试，DDMA 会命中 MiShowBadMapper 蓝屏，通道已被禁用。");
    }
    if (m_capabilityLabel != nullptr)
    {
        m_capabilityLabel->setText(capabilityText);
    }

    refreshSessionState();
}

void DdmaPage::activateSelectedDisk()
{
    if (m_diskTable == nullptr)
    {
        return;
    }
    const int row = m_diskTable->currentRow();
    if (row < 0 || row >= static_cast<int>(m_diskCache.size()))
    {
        QMessageBox::warning(this, QStringLiteral("DDMA"), QStringLiteral("请先选中一块磁盘。"));
        return;
    }

    const ksword::ark::DdmaDiskEntry& entry = m_diskCache[static_cast<std::size_t>(row)];
    if (!entry.ready())
    {
        QMessageBox::warning(
            this,
            QStringLiteral("DDMA"),
            QStringLiteral(
                "这块磁盘没有通过 DMA 探测，不能作为 DDMA 通道。\n"
                "请先填写暂存扇区 LBA 再重新探测；探测 NTSTATUS=%1。")
                .arg(formatNtStatus(entry.probeStatus)));
        return;
    }

    std::uint64_t scratchLba = 0ULL;
    QString lbaError;
    if (!parseScratchLbaFromUi(scratchLba, lbaError))
    {
        QMessageBox::warning(this, QStringLiteral("DDMA"), lbaError);
        return;
    }

    m_session.configured = true;
    m_session.diskIndex = entry.deviceIndex;
    m_session.deviceName = entry.deviceName;
    m_session.scratchLba = scratchLba;
    m_session.scratchLbaValid = true;
    m_session.scratchAcknowledged =
        (m_scratchAckCheck != nullptr) && m_scratchAckCheck->isChecked();
    m_session.kernelDebuggerEnabled = m_kernelDebuggerEnabled;
    m_session.transferBytes = m_transferBytes;
    m_session.scratchSectorCount = m_scratchSectorCount;

    refreshSessionState();
}

void DdmaPage::clearSession()
{
    m_session = ksword::memory_backend::DdmaSession{};
    // 探测出来的机器属性与会话是否启用无关，保留下来供状态展示继续使用。
    m_session.kernelDebuggerEnabled = m_kernelDebuggerEnabled;
    m_session.transferBytes = m_transferBytes;
    m_session.scratchSectorCount = m_scratchSectorCount;
    refreshSessionState();
}

void DdmaPage::refreshSessionState()
{
    // 勾选状态随时可变，会话里的这两项始终跟随控件，不需要重新点“启用”。
    if (m_session.configured)
    {
        m_session.scratchAcknowledged =
            (m_scratchAckCheck != nullptr) && m_scratchAckCheck->isChecked();

        std::uint64_t scratchLba = 0ULL;
        QString lbaError;
        if (parseScratchLbaFromUi(scratchLba, lbaError))
        {
            m_session.scratchLba = scratchLba;
            m_session.scratchLbaValid = true;
        }
        else
        {
            m_session.scratchLbaValid = false;
        }
    }

    // 影响范围文案要随输入实时更新，用户改 LBA 时立刻能看到会动哪几个扇区。
    if (m_scratchImpactLabel != nullptr)
    {
        std::uint64_t scratchLba = 0ULL;
        QString lbaError;
        const std::uint32_t sectorCount = (m_scratchSectorCount != 0U)
            ? m_scratchSectorCount
            : static_cast<std::uint32_t>(KSWORD_ARK_DDMA_SCRATCH_SECTOR_COUNT);
        if (parseScratchLbaFromUi(scratchLba, lbaError))
        {
            QString impactText = QStringLiteral(
                "每次 DDMA 操作会临时覆盖 LBA %1 到 %2 共 %3 个扇区（字节偏移 %4 起）。")
                .arg(scratchLba)
                .arg(scratchLba + sectorCount - 1ULL)
                .arg(sectorCount)
                .arg(formatAddress(scratchLba * KSWORD_ARK_DDMA_SECTOR_SIZE));
            if (scratchLba < sectorCount)
            {
                impactText += QStringLiteral(
                    " 警告：这段范围覆盖了 LBA 0，也就是 MBR / GPT 保护扇区。"
                    "操作期间发生蓝屏或断电会导致磁盘无法引导。");
            }
            m_scratchImpactLabel->setText(impactText);
        }
        else
        {
            m_scratchImpactLabel->setText(lbaError);
        }
    }

    // 本页是进程级会话的唯一写入者：本地状态一旦变动就立刻推上去，
    // 右上角的常驻指示灯与其它页面的后端下拉都从那一份读。
    ksword::memory_backend::setCurrentDdmaSession(m_session);

    QString reason;
    const bool usable = ksword::memory_backend::isDdmaUsable(m_session, &reason);

    if (m_sessionStateLabel != nullptr)
    {
        if (usable)
        {
            m_sessionStateLabel->setText(QStringLiteral(
                "DDMA 通道已就绪：磁盘 #%1 %2，暂存 LBA %3。"
                "内存搜索、内存查看器、驱动内存读写与系统内存审计四个页面现在都可以选择 DDMA 后端。")
                .arg(m_session.diskIndex)
                .arg(m_session.deviceName.empty()
                    ? QStringLiteral("(名称不可用)")
                    : QString::fromStdWString(m_session.deviceName))
                .arg(m_session.scratchLba));
            m_sessionStateLabel->setStyleSheet(
                QStringLiteral("color:%1;").arg(KswordTheme::SuccessHex()));
        }
        else
        {
            m_sessionStateLabel->setText(QStringLiteral("DDMA 通道不可用：%1").arg(reason));
            // 内核调试那一条是"用了会蓝屏"，比其它"还没配好"严重，用错误色。
            m_sessionStateLabel->setStyleSheet(
                QStringLiteral("color:%1;")
                    .arg(m_session.kernelDebuggerEnabled
                        ? KswordTheme::ErrorHex()
                        : KswordTheme::WarningHex()));
        }
    }

    if (m_accessReadButton != nullptr)
    {
        m_accessReadButton->setEnabled(usable);
    }
    if (m_accessWriteButton != nullptr && !usable)
    {
        m_accessWriteButton->setEnabled(false);
    }
    if (m_compareButton != nullptr)
    {
        m_compareButton->setEnabled(usable);
    }

    if (m_sessionChangedCallback)
    {
        m_sessionChangedCallback();
    }
}

void DdmaPage::readPhysicalFromUi()
{
    if (m_accessAddressEdit == nullptr || m_accessLengthSpin == nullptr)
    {
        return;
    }

    std::uint64_t physicalAddress = 0ULL;
    if (!parseAddressText(m_accessAddressEdit->text(), physicalAddress))
    {
        QMessageBox::warning(
            this, QStringLiteral("DDMA"), QStringLiteral("物理地址解析失败，请填写 0x 十六进制地址。"));
        return;
    }

    const std::uint64_t lengthBytes = static_cast<std::uint64_t>(m_accessLengthSpin->value());
    if (m_accessStatusLabel != nullptr)
    {
        m_accessStatusLabel->setText(QStringLiteral("正在通过 DDMA 读取物理内存..."));
    }

    const ksword::memory_backend::AccessOutcome outcome =
        ksword::memory_backend::readPhysical(
            ksword::memory_backend::MemoryAccessBackend::Ddma,
            m_session,
            physicalAddress,
            lengthBytes);

    if (!outcome.ok)
    {
        m_hasSnapshot = false;
        m_originalBytes.clear();
        m_editedBytes.clear();
        if (m_accessWriteButton != nullptr)
        {
            m_accessWriteButton->setEnabled(false);
        }
        if (m_accessStatusLabel != nullptr)
        {
            m_accessStatusLabel->setText(QStringLiteral("DDMA 读取失败：%1").arg(outcome.failureText));
        }
        QMessageBox::warning(this, QStringLiteral("DDMA"), outcome.failureText);
        return;
    }

    m_snapshotAddress = physicalAddress;
    m_originalBytes = outcome.data;
    m_editedBytes = m_originalBytes;
    m_hasSnapshot = true;

    if (m_accessHexEditor != nullptr)
    {
        m_accessHexEditor->setEditable(true);
        m_accessHexEditor->setByteArray(m_editedBytes, m_snapshotAddress);
    }
    if (m_accessWriteButton != nullptr)
    {
        m_accessWriteButton->setEnabled(false);
    }

    QString statusText = QStringLiteral("DDMA 读取成功，共 %1 字节。").arg(m_originalBytes.size());
    if (outcome.scratchDirty)
    {
        statusText += QStringLiteral(
            " 严重告警：暂存扇区未能还原，磁盘上留下了脏扇区，请立即检查 LBA %1 起的内容。")
            .arg(m_session.scratchLba);
    }
    if (m_accessStatusLabel != nullptr)
    {
        m_accessStatusLabel->setText(statusText);
    }
}

void DdmaPage::writePhysicalFromUi()
{
    if (!m_hasSnapshot || m_editedBytes == m_originalBytes)
    {
        return;
    }

    const QMessageBox::StandardButton confirm = QMessageBox::warning(
        this,
        QStringLiteral("DDMA 写入确认"),
        QStringLiteral(
            "即将用磁盘 DMA 直接写入物理内存。\n"
            "起始物理地址: %1\n"
            "长度: %2 字节\n\n"
            "这条路径没有事务与回滚，非整页写入还会触发读-改-写，"
            "同页其它字节存在覆盖窗口。确认继续？")
            .arg(formatAddress(m_snapshotAddress))
            .arg(m_editedBytes.size()),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirm != QMessageBox::Yes)
    {
        return;
    }

    // 只提交改动过的那一段，避免把整页原样写回去也算一次写入。
    qsizetype firstDiff = 0;
    while (firstDiff < m_editedBytes.size() && m_editedBytes[firstDiff] == m_originalBytes[firstDiff])
    {
        ++firstDiff;
    }
    qsizetype lastDiff = m_editedBytes.size() - 1;
    while (lastDiff > firstDiff && m_editedBytes[lastDiff] == m_originalBytes[lastDiff])
    {
        --lastDiff;
    }
    const QByteArray payload = m_editedBytes.mid(firstDiff, lastDiff - firstDiff + 1);
    const std::uint64_t targetAddress =
        m_snapshotAddress + static_cast<std::uint64_t>(firstDiff);

    ksword::memory_backend::AccessOutcome outcome =
        ksword::memory_backend::writePhysical(
            ksword::memory_backend::MemoryAccessBackend::Ddma,
            m_session,
            targetAddress,
            payload,
            false);

    if (outcome.forceRequired)
    {
        const QMessageBox::StandardButton forceConfirm = QMessageBox::warning(
            this,
            QStringLiteral("DDMA 强制写入"),
            QStringLiteral("驱动要求对本次 DDMA 写入附加强制标志。确认继续？"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (forceConfirm != QMessageBox::Yes)
        {
            if (m_accessStatusLabel != nullptr)
            {
                m_accessStatusLabel->setText(QStringLiteral("已取消 DDMA 强制写入。"));
            }
            return;
        }
        outcome = ksword::memory_backend::writePhysical(
            ksword::memory_backend::MemoryAccessBackend::Ddma,
            m_session,
            targetAddress,
            payload,
            true);
    }

    QString statusText;
    if (outcome.ok)
    {
        m_originalBytes = m_editedBytes;
        if (m_accessWriteButton != nullptr)
        {
            m_accessWriteButton->setEnabled(false);
        }
        statusText = QStringLiteral("DDMA 写入成功，共 %1 字节。").arg(outcome.bytesDone);
        if (outcome.lostUpdateWindow)
        {
            statusText += QStringLiteral(
                " 本次为非整页写入，驱动做了读-改-写，同页其它字节存在覆盖窗口。");
        }
    }
    else
    {
        statusText = QStringLiteral("DDMA 写入失败：%1").arg(outcome.failureText);
        QMessageBox::warning(this, QStringLiteral("DDMA"), outcome.failureText);
    }
    if (outcome.scratchDirty)
    {
        statusText += QStringLiteral(
            " 严重告警：暂存扇区未能还原，磁盘上留下了脏扇区，请立即检查 LBA %1 起的内容。")
            .arg(m_session.scratchLba);
    }
    if (m_accessStatusLabel != nullptr)
    {
        m_accessStatusLabel->setText(statusText);
    }
}

void DdmaPage::compareBackendsFromUi()
{
    if (m_compareAddressEdit == nullptr || m_compareTable == nullptr)
    {
        return;
    }

    std::uint64_t physicalAddress = 0ULL;
    if (!parseAddressText(m_compareAddressEdit->text(), physicalAddress))
    {
        QMessageBox::warning(
            this, QStringLiteral("DDMA"), QStringLiteral("物理地址解析失败，请填写 0x 十六进制地址。"));
        return;
    }
    // 复核固定比一整页，两个后端读同一段才有可比性。
    const std::uint64_t pageBase =
        physicalAddress & ~static_cast<std::uint64_t>(KSWORD_ARK_DDMA_TRANSFER_BYTES - 1UL);

    m_compareTable->setRowCount(0);
    if (m_compareResultLabel != nullptr)
    {
        m_compareResultLabel->setText(QStringLiteral("正在复核..."));
    }

    const ksword::memory_backend::AccessOutcome standardOutcome =
        ksword::memory_backend::readPhysical(
            ksword::memory_backend::MemoryAccessBackend::StandardDriver,
            m_session,
            pageBase,
            KSWORD_ARK_DDMA_TRANSFER_BYTES);
    const ksword::memory_backend::AccessOutcome ddmaOutcome =
        ksword::memory_backend::readPhysical(
            ksword::memory_backend::MemoryAccessBackend::Ddma,
            m_session,
            pageBase,
            KSWORD_ARK_DDMA_TRANSFER_BYTES);

    if (!standardOutcome.ok || !ddmaOutcome.ok)
    {
        // 一侧失败时不能下"两者一致"或"两者不一致"的结论，只能如实说哪一侧没读到。
        const QString detail = !standardOutcome.ok
            ? QStringLiteral("标准通道读取失败：%1").arg(standardOutcome.failureText)
            : QStringLiteral("DDMA 读取失败：%1").arg(ddmaOutcome.failureText);
        if (m_compareResultLabel != nullptr)
        {
            m_compareResultLabel->setText(
                QStringLiteral("无法比对，%1").arg(detail));
            m_compareResultLabel->setStyleSheet(
                QStringLiteral("color:%1;").arg(KswordTheme::WarningHex()));
        }
        return;
    }

    const qsizetype compareLength =
        std::min<qsizetype>(standardOutcome.data.size(), ddmaOutcome.data.size());
    qsizetype diffCount = 0;
    int shownRows = 0;
    for (qsizetype index = 0; index < compareLength; ++index)
    {
        if (standardOutcome.data[index] == ddmaOutcome.data[index])
        {
            continue;
        }
        ++diffCount;
        if (shownRows >= kCompareSampleRows)
        {
            continue;
        }
        const int row = m_compareTable->rowCount();
        m_compareTable->insertRow(row);
        m_compareTable->setItem(row, 0,
            new QTableWidgetItem(formatAddress(pageBase + static_cast<std::uint64_t>(index))));
        m_compareTable->setItem(row, 1, new QTableWidgetItem(
            QStringLiteral("%1").arg(
                static_cast<std::uint8_t>(standardOutcome.data[index]), 2, 16, QChar('0')).toUpper()));
        m_compareTable->setItem(row, 2, new QTableWidgetItem(
            QStringLiteral("%1").arg(
                static_cast<std::uint8_t>(ddmaOutcome.data[index]), 2, 16, QChar('0')).toUpper()));
        ++shownRows;
    }
    m_compareTable->resizeColumnsToContents();

    if (m_compareResultLabel == nullptr)
    {
        return;
    }
    if (diffCount == 0)
    {
        m_compareResultLabel->setText(QStringLiteral(
            "物理页 %1：两个后端读到的 %2 字节完全一致，没有观察到重定向迹象。")
            .arg(formatAddress(pageBase))
            .arg(compareLength));
        m_compareResultLabel->setStyleSheet(
            QStringLiteral("color:%1;").arg(KswordTheme::SuccessHex()));
    }
    else
    {
        m_compareResultLabel->setText(QStringLiteral(
            "物理页 %1：共 %2 字节中有 %3 字节不一致（表中最多展示 %4 条）。"
            "标准通道受 SLAT 约束，DDMA 不受约束，这种差异通常意味着该页被上层虚拟化重定向或隐藏。")
            .arg(formatAddress(pageBase))
            .arg(compareLength)
            .arg(diffCount)
            .arg(kCompareSampleRows));
        m_compareResultLabel->setStyleSheet(
            QStringLiteral("color:%1;").arg(KswordTheme::WarningHex()));
    }
}

bool DdmaPage::parseAddressText(const QString& text, std::uint64_t& valueOut)
{
    valueOut = 0ULL;
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
    {
        return false;
    }

    bool converted = false;
    if (trimmed.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
    {
        valueOut = trimmed.mid(2).toULongLong(&converted, 16);
    }
    else
    {
        valueOut = trimmed.toULongLong(&converted, 10);
        if (!converted)
        {
            valueOut = trimmed.toULongLong(&converted, 16);
        }
    }
    return converted;
}

QString DdmaPage::formatAddress(const std::uint64_t address)
{
    return QStringLiteral("0x%1")
        .arg(static_cast<qulonglong>(address), 16, 16, QChar('0'))
        .toUpper()
        .replace(QStringLiteral("0X"), QStringLiteral("0x"));
}
