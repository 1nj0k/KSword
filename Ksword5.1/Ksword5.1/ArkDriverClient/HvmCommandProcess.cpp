#include "HvmCommandProcess.h"

#include <QCoreApplication>
#include <QVector>
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <shellapi.h>

namespace ksword::ark
{
    namespace
    {
        bool bindPipe(FILE* stream, const DWORD kind)
        {
            const HANDLE handle = GetStdHandle(kind);
            HANDLE copy = INVALID_HANDLE_VALUE;
            if (!handle || handle == INVALID_HANDLE_VALUE ||
                !DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(),
                                 &copy, 0, FALSE, DUPLICATE_SAME_ACCESS)) { return false; }
            const int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(copy), _O_TEXT);
            if (descriptor < 0) { CloseHandle(copy); return false; }
            FILE* bound = nullptr;
            const bool ok = freopen_s(&bound, "NUL", "w", stream) == 0 &&
                            _dup2(descriptor, _fileno(stream)) == 0;
            _close(descriptor);
            return ok;
        }
    }

    bool tryRunHvmCommandLine(int* const exitCode)
    {
        int count = 0;
        wchar_t** const args = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!args) { return false; }
        if (count < 2 || wcscmp(args[1], L"--ksword-hvm-command") != 0)
        {
            LocalFree(args);
            return false;
        }
        // A Windows-subsystem process has no CRT console streams. Bind only the
        // inherited pipes; never allocate a visible console or touch GUI state.
        if (!bindPipe(stdout, STD_OUTPUT_HANDLE) || !bindPipe(stderr, STD_ERROR_HANDLE))
        {
            *exitCode = 1;
        }
        else
        {
            *exitCode = KswordHvmCommandMainWide(count - 1, args + 1);
            fflush(stdout);
            fflush(stderr);
        }
        LocalFree(args);
        return true;
    }

    HvmCommandProcess::HvmCommandProcess(QObject* const parent) : QObject(parent)
    {
        m_process.setProcessChannelMode(QProcess::MergedChannels);
        connect(&m_process, &QProcess::readyReadStandardOutput, this, [this]() {
            const QString text = m_decoder(m_process.readAllStandardOutput());
            if (onOutput) { onOutput(text); }
        });
        connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart || !m_running) { return; }
            m_running = false;
            if (onOutput) { onOutput(m_process.errorString()); }
            if (onFinished) { onFinished(1, false); }
        });
        connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
            if (!m_running) { return; }
            const QString remaining = m_decoder(m_process.readAllStandardOutput());
            if (onOutput && !remaining.isEmpty()) { onOutput(remaining); }
            m_running = false;
            if (onFinished) { onFinished(code, status == QProcess::NormalExit); }
        });
    }

    bool HvmCommandProcess::validate(const HVM_COMMAND_SPEC& command, const QStringList& arguments,
                                    QString* const error)
    {
        QVector<QByteArray> encoded;
        QVector<const char*> pointers;
        for (const QString& value : arguments) { encoded.append(value.toUtf8()); }
        for (const QByteArray& value : encoded) { pointers.append(value.constData()); }
        unsigned long long values[KSW_HVM_COMMAND_MAX_ARGS]{};
        char invalid[256]{};
        if (KswordHvmValidateArguments(&command, static_cast<int>(pointers.size()), pointers.constData(),
                                       values, invalid, sizeof(invalid)) != 0)
        {
            if (error) { *error = QString::fromUtf8(invalid); }
            return false;
        }
        return true;
    }

    bool HvmCommandProcess::start(const HVM_COMMAND_SPEC& command, const QStringList& arguments,
                                  const bool validateOnly, QString* const error)
    {
        if (m_running || !validate(command, arguments, error)) { return false; }
        QStringList args{QStringLiteral("--ksword-hvm-command"), QStringLiteral("--json")};
        if (validateOnly) { args.append(QStringLiteral("--validate")); }
        args.append(QString::fromLatin1(command.name));
        args.append(arguments);
        m_decoder.resetState();
        m_running = true;
        // The GUI invokes its own compiled engine, so no stale sidecar executable
        // or PATH lookup can silently change command semantics. No shell is used.
        m_process.start(QCoreApplication::applicationFilePath(), args);
        return true;
    }
}
