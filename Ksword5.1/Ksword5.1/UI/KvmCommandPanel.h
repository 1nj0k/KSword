#pragma once

#include <QWidget>
#include <QStringList>
#include <functional>
#include "../ArkDriverClient/HvmCommandCatalog.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;
namespace ksword::ark { class HvmCommandProcess; }

class KvmCommandPanel final : public QWidget
{
public:
    explicit KvmCommandPanel(QWidget* parent = nullptr);
    std::function<void(bool)> onBusyChanged;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void selectCommand(const HVM_COMMAND_SPEC* command);
    void run(bool validateOnly);
    void launch(const HVM_COMMAND_SPEC& command, const QStringList& arguments, bool validateOnly);
    QStringList arguments() const;
    void setBusy(bool busy);

    const HVM_COMMAND_SPEC* m_selected = nullptr;
    const HVM_COMMAND_SPEC* m_runningCommand = nullptr;
    ksword::ark::HvmCommandProcess* m_process = nullptr;
    QTreeWidget* m_commands = nullptr;
    QLineEdit* m_search = nullptr;
    QLabel* m_title = nullptr;
    QLabel* m_description = nullptr;
    QLabel* m_result = nullptr;
    QLineEdit* m_arguments[KSW_HVM_COMMAND_MAX_ARGS]{};
    QLabel* m_argumentLabels[KSW_HVM_COMMAND_MAX_ARGS]{};
    QComboBox* m_roots = nullptr;
    QPushButton* m_readRoots = nullptr;
    QPushButton* m_run = nullptr;
    QPushButton* m_validate = nullptr;
    QPlainTextEdit* m_output = nullptr;
    QString m_lastOutput;
    bool m_validating = false;
};
