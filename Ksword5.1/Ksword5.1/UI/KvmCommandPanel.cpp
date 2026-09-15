#include "KvmCommandPanel.h"
#include "KvmWriteAccessGate.h"
#include "UI_All.h"
#include "../ArkDriverClient/HvmCommandProcess.h"
#include "../Internationalization/LanguageManager.h"
#include "../Framework/DestructiveActionConfirmation.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QTextCursor>
#include <QVBoxLayout>

namespace
{
    QString text(const char* source)
    {
        return ks::i18n::sourceText(QString::fromUtf8(source));
    }
}

KvmCommandPanel::KvmCommandPanel(QWidget* const parent) : QWidget(parent)
{
    setWindowTitle(text("KVM 完整命令面板"));
    setObjectName(QStringLiteral("KvmCommandPanel"));
    m_process = new ksword::ark::HvmCommandProcess(this);
    auto* layout = new QVBoxLayout(this);
    auto* splitter = new QSplitter(this);
    auto* navigation = new QWidget(splitter);
    auto* navLayout = new QVBoxLayout(navigation);
    m_search = new QLineEdit(navigation);
    m_search->setPlaceholderText(text("搜索操作或命令名"));
    m_commands = new QTreeWidget(navigation);
    m_commands->setHeaderHidden(true);
    navLayout->addWidget(m_search);
    navLayout->addWidget(m_commands, 1);
    auto* scroll = new QScrollArea(splitter);
    scroll->setWidgetResizable(true);
    auto* detail = new QWidget(scroll);
    auto* details = new QVBoxLayout(detail);
    m_title = new QLabel(detail);
    auto titleFont = m_title->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 2);
    m_title->setFont(titleFont);
    m_title->setWordWrap(true);
    m_description = new QLabel(detail);
    m_description->setWordWrap(true);
    details->addWidget(m_title);
    details->addWidget(m_description);
    auto* form = new QFormLayout;
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    for (unsigned int i = 0; i < KSW_HVM_COMMAND_MAX_ARGS; ++i)
    {
        m_argumentLabels[i] = new QLabel(detail);
        m_arguments[i] = new QLineEdit(detail);
        m_arguments[i]->setObjectName(QStringLiteral("hvm_argument_%1").arg(i));
        form->addRow(m_argumentLabels[i], m_arguments[i]);
    }
    details->addLayout(form);
    auto* rootsRow = new QHBoxLayout;
    m_readRoots = new QPushButton(text("读取当前 EPT12 根"), detail);
    m_roots = new QComboBox(detail);
    m_roots->setPlaceholderText(text("选择目标 EPT12 根"));
    m_roots->setMinimumContentsLength(18);
    rootsRow->addWidget(m_readRoots);
    rootsRow->addWidget(m_roots, 1);
    details->addLayout(rootsRow);
    auto* actions = new QHBoxLayout;
    m_validate = new QPushButton(text("仅校验参数"), detail);
    m_run = new QPushButton(text("执行操作"), detail);
    m_run->setAutoDefault(false);
    m_validate->setAutoDefault(false);
    actions->addWidget(m_validate);
    actions->addWidget(m_run);
    actions->addStretch();
    details->addLayout(actions);
    m_result = new QLabel(text("尚未执行"), detail);
    m_result->setWordWrap(true);
    details->addWidget(m_result);
    m_output = new QPlainTextEdit(detail);
    m_output->setReadOnly(true);
    m_output->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_output->setMinimumHeight(160);
    details->addWidget(m_output, 1);
    auto* outputActions = new QHBoxLayout;
    auto* copy = new QPushButton(text("复制完整输出"), detail);
    auto* save = new QPushButton(text("保存完整输出"), detail);
    outputActions->addWidget(copy);
    outputActions->addWidget(save);
    outputActions->addStretch();
    details->addLayout(outputActions);
    scroll->setWidget(detail);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);
    splitter->setChildrenCollapsible(false);
    layout->addWidget(splitter);
    size_t count = 0;
    const auto* commands = KswordHvmCommands(&count);
    QHash<QString, QTreeWidgetItem*> groups;
    for (size_t i = 0; i < count; ++i)
    {
        const auto& c = commands[i];
        const QString group = text(c.group);
        if (!groups.contains(group))
        {
            groups[group] = new QTreeWidgetItem(m_commands, {group});
            groups[group]->setFlags(groups[group]->flags() & ~Qt::ItemIsSelectable);
        }
        auto* item = new QTreeWidgetItem(groups[group], {text(c.title)});
        item->setData(0, Qt::UserRole, QString::fromLatin1(c.name));
        item->setToolTip(0, QString::fromLatin1(c.name));
    }
    m_commands->expandAll();
    connect(m_commands, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        if (item && item->parent())
        {
            const QByteArray name = item->data(0, Qt::UserRole).toString().toLatin1();
            selectCommand(KswordHvmFindCommand(name.constData()));
        }
    });
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString& query) {
        for (int i = 0; i < m_commands->topLevelItemCount(); ++i)
        {
            auto* group = m_commands->topLevelItem(i);
            bool any = false;
            for (int j = 0; j < group->childCount(); ++j)
            {
                auto* item = group->child(j);
                const bool match = (item->text(0) + item->data(0, Qt::UserRole).toString())
                    .contains(query, Qt::CaseInsensitive);
                item->setHidden(!match);
                any |= match;
            }
            group->setHidden(!any);
        }
    });
    connect(m_validate, &QPushButton::clicked, this, [this]() { run(true); });
    connect(m_run, &QPushButton::clicked, this, [this]() { run(false); });
    connect(m_readRoots, &QPushButton::clicked, this, [this]() {
        m_roots->clear();
        m_arguments[0]->clear();
        launch(*KswordHvmFindCommand("nested-page-query"), {}, false);
    });
    connect(m_roots, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index >= 0 && m_selected && m_selected->handler == HvmPageMap)
        {
            m_arguments[0]->setText(m_roots->itemText(index));
        }
    });
    connect(copy, &QPushButton::clicked, this, [this]() {
        QApplication::clipboard()->setText(m_lastOutput);
    });
    connect(save, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getSaveFileName(this, text("保存完整输出"));
        if (path.isEmpty()) { return; }
        QFile file(path);
        const QByteArray bytes = m_lastOutput.toUtf8();
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        {
            m_result->setText(text("保存失败：%1").arg(file.errorString()));
        }
    });
    m_process->onOutput = [this](const QString& output) {
        m_lastOutput += output;
        m_output->moveCursor(QTextCursor::End);
        m_output->insertPlainText(output);
    };
    m_process->onFinished = [this](int code, bool normal) {
        setBusy(false);
        const QString outcome = !normal ? text("执行进程异常结束。") :
            code == 0 ? (m_validating ? text("参数校验通过，未执行操作。") : text("操作完成（退出码 0）。")) :
            code == 3 ? text("未执行验证：前置条件未满足（退出码 3）。") :
            text("操作失败（退出码 %1），详见输出。").arg(code);
        m_result->setText((m_runningCommand ? text(m_runningCommand->title) + QStringLiteral(": ") : QString()) + outcome);
        if (normal && code == 0 && m_runningCommand && m_runningCommand->handler == HvmPageQuery)
        {
            const auto result = QJsonDocument::fromJson(m_lastOutput.toUtf8()).object();
            if (result.value(QStringLiteral("status")).toInt(-1) == 0)
            {
                const QSignalBlocker block(m_roots);
                m_roots->clear();
                for (const auto& root : result.value(QStringLiteral("roots")).toArray())
                {
                    m_roots->addItem(root.toString());
                }
                m_roots->setCurrentIndex(m_roots->count() == 1 ? 0 : -1);
                if (m_selected && m_selected->handler == HvmPageMap)
                {
                    m_arguments[0]->setText(m_roots->count() == 1 ? m_roots->currentText() : QString());
                }
            }
        }
        if (m_completed) { m_completed(code, normal); }
    };
    m_commands->setCurrentItem(groups[text("查询与观测")]->child(0));
}

void KvmCommandPanel::selectCommand(const HVM_COMMAND_SPEC* command)
{
    if (!command || m_process->running()) { return; }
    m_selected = command;
    m_title->setText(text(command->title));
    m_description->setText(text(command->description) + QLatin1Char('\n') + QString::fromLatin1(command->name));
    for (unsigned int i = 0; i < KSW_HVM_COMMAND_MAX_ARGS; ++i)
    {
        const bool visible = i < command->argumentCount;
        m_arguments[i]->setVisible(visible);
        m_argumentLabels[i]->setVisible(visible);
        if (!visible) { continue; }
        const auto& arg = command->arguments[i];
        m_argumentLabels[i]->setText(text(arg.name));
        m_arguments[i]->setText(arg.defaultValue ? QString::fromUtf8(arg.defaultValue) : QString());
        m_arguments[i]->setPlaceholderText(arg.defaultValue ? QString() : text("必填"));
    }
    m_readRoots->setVisible(command->handler == HvmPageMap);
    m_roots->setVisible(command->handler == HvmPageMap);
    if (command->handler == HvmPageMap && m_roots->currentIndex() >= 0)
    {
        m_arguments[0]->setText(m_roots->currentText());
    }
}

QStringList KvmCommandPanel::arguments() const
{
    QStringList args;
    if (!m_selected) { return args; }
    for (unsigned int i = 0; i < m_selected->argumentCount; ++i)
    {
        const auto& arg = m_selected->arguments[i];
        QString value = arg.kind == HvmPath ? m_arguments[i]->text() : m_arguments[i]->text().trimmed();
        if (value.isEmpty() && arg.defaultValue) { value = QString::fromUtf8(arg.defaultValue); }
        args.append(value);
    }
    return args;
}

void KvmCommandPanel::run(const bool validateOnly)
{
    if (!m_selected || m_process->running()) { return; }
    const QStringList args = arguments();
    QString invalid;
    if (!ksword::ark::HvmCommandProcess::validate(*m_selected, args, &invalid))
    {
        m_result->setText(text("参数无效或缺失：%1").arg(text(invalid.toUtf8().constData())));
        if (m_completed) { m_completed(2, true); }
        return;
    }
    if (!validateOnly && !m_selected->readOnly)
    {
        if (!ks::ui::requestKvmWriteAccess(this)) { return; }
        if (!ks::ui::confirmDestructiveAction(this, QStringLiteral("Kvm/Command/") +
            QString::fromLatin1(m_selected->name), text(m_selected->title),
            QString::fromLatin1(m_selected->name) + QLatin1Char(' ') + args.join(QLatin1Char(' ')),
            text(m_selected->description))) { return; }
    }
    launch(*m_selected, args, validateOnly);
}

void KvmCommandPanel::launch(const HVM_COMMAND_SPEC& command, const QStringList& args, const bool validateOnly)
{
    if (m_process->running()) { return; }
    m_lastOutput.clear();
    m_output->clear();
    m_runningCommand = &command;
    m_validating = validateOnly;
    window()->installEventFilter(this);
    QString error;
    setBusy(true);
    if (!m_process->start(command, args, validateOnly, &error))
    {
        setBusy(false);
        m_result->setText(text("参数无效或缺失：%1").arg(text(error.toUtf8().constData())));
        if (m_completed) { m_completed(2, true); }
    }
}

void KvmCommandPanel::setBusy(const bool busy)
{
    m_commands->setEnabled(!busy);
    m_search->setEnabled(!busy);
    m_run->setEnabled(!busy);
    m_validate->setEnabled(!busy);
    m_readRoots->setEnabled(!busy);
    m_roots->setEnabled(!busy);
    for (auto* arg : m_arguments) { arg->setEnabled(!busy); }
    if (busy) { m_result->setText(text("操作执行中，请等待完成。")); }
    if (onBusyChanged) { onBusyChanged(busy); }
}

bool KvmCommandPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == window() && event->type() == QEvent::Close && m_process->running())
    {
        event->ignore();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

int KvmCommandPanel::runCoverageTest(const QString& reportPath, QWidget* captureHost)
{
    auto& panel = *this;
    const bool embedded = !isWindow() && captureHost && captureHost != this && captureHost->isAncestorOf(this);
    size_t count = 0;
    const auto* commands = KswordHvmCommands(&count);
    QJsonArray rows;
    bool passed = embedded;
    for (size_t i = 0; i < count; ++i)
    {
        const auto& command = commands[i];
        bool found = false;
        for (QTreeWidgetItemIterator it(panel.m_commands); *it; ++it)
        {
            if ((*it)->data(0, Qt::UserRole).toString() == QString::fromLatin1(command.name))
            {
                panel.m_commands->setCurrentItem(*it);
                found = panel.m_selected == &command;
                break;
            }
        }
        if (!found) { passed = false; break; }
        for (unsigned int j = 0; j < command.argumentCount; ++j)
        {
            const auto& arg = command.arguments[j];
            QString value;
            if (arg.defaultValue) { value = QString::fromUtf8(arg.defaultValue); }
            else if (arg.kind == HvmPath) { value = QStringLiteral("C:/HVM test/中文 测试.dll"); }
            else if (arg.kind == HvmByte) { value = QStringLiteral("d1"); }
            else if (arg.kind == HvmPageAddress) { value = QStringLiteral("7000000"); }
            else if (arg.kind == HvmDecimal32) { value = QStringLiteral("4242"); }
            else { value = QString::number(0x12345000ULL + j * 0x1000ULL, 16); }
            panel.m_arguments[j]->setText(value);
        }
        QEventLoop wait;
        bool completed = false, ok = false;
        panel.m_completed = [&](int code, bool normal) {
            completed = true;
            ok = normal && code == 0;
            wait.quit();
        };
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &wait, &QEventLoop::quit);
        timer.start(30000);
        panel.m_validate->click();
        if (!completed) { wait.exec(); }
        panel.m_completed = {};
        const auto output = QJsonDocument::fromJson(panel.m_lastOutput.toUtf8()).object();
        ok = ok && output.value(QStringLiteral("kind")).toString() == QStringLiteral("validated") &&
            output.value(QStringLiteral("command")).toString() == QString::fromLatin1(command.name) &&
            panel.m_result->text().contains(text("参数校验通过，未执行操作。"));
        rows.append(QJsonObject{{QStringLiteral("name"), QString::fromLatin1(command.name)},
                                {QStringLiteral("ok"), ok},
                                {QStringLiteral("arguments"), QJsonArray::fromStringList(panel.arguments())},
                                {QStringLiteral("result"), output}});
        passed &= ok;
        if (!completed) { break; }
    }
    for (QTreeWidgetItemIterator it(panel.m_commands); *it; ++it)
    {
        if ((*it)->data(0, Qt::UserRole).toString() == QStringLiteral("nested-page-map"))
        {
            panel.m_commands->setCurrentItem(*it);
            break;
        }
    }
    panel.m_arguments[0]->setText(QStringLiteral("1234501e"));
    panel.m_arguments[1]->setText(QStringLiteral("7000000"));
    panel.m_arguments[2]->setText(QStringLiteral("d1"));
    panel.m_result->setText(text("尚未执行"));
    panel.m_output->clear();
    if (captureHost) { QMetaObject::invokeMethod(captureHost, "focusKvmCommands", Qt::DirectConnection); }
    QApplication::processEvents();
    if (captureHost) { captureHost->grab().save(reportPath + QStringLiteral(".png")); }
    QFile report(reportPath);
    const QByteArray data = QJsonDocument(QJsonObject{
        {QStringLiteral("passed"), passed && rows.size() == static_cast<qsizetype>(count)},
        {QStringLiteral("embedded"), embedded},
        {QStringLiteral("hostClass"), captureHost ? QString::fromLatin1(captureHost->metaObject()->className()) : QString()},
        {QStringLiteral("count"), static_cast<int>(count)},
        {QStringLiteral("rows"), rows}}).toJson();
    if (!report.open(QIODevice::WriteOnly) || report.write(data) != data.size()) { return 1; }
    return passed && rows.size() == static_cast<qsizetype>(count) ? 0 : 2;
}
