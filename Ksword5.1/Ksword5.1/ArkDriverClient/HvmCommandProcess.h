#pragma once

#include "HvmCommandCatalog.h"
#include <QObject>
#include <QProcess>
#include <QStringDecoder>
#include <QStringList>
#include <functional>

namespace ksword::ark
{
    // Run before QApplication, single-instance handling, or driver auto-loading.
    bool tryRunHvmCommandLine(int* exitCode);

    class HvmCommandProcess final : public QObject
    {
    public:
        explicit HvmCommandProcess(QObject* parent = nullptr);
        static bool validate(const HVM_COMMAND_SPEC& command, const QStringList& arguments, QString* error);
        bool start(const HVM_COMMAND_SPEC& command, const QStringList& arguments,
                   bool validateOnly, QString* error);
        bool running() const { return m_running; }
        std::function<void(const QString&)> onOutput;
        std::function<void(int, bool)> onFinished;

    private:
        QProcess m_process;
        QStringDecoder m_decoder{QStringDecoder::Utf8};
        bool m_running = false;
    };
}
