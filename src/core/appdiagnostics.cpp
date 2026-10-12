#include "appdiagnostics.h"

#include "diagnosticlogging.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageLogContext>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>

#include <cstdio>
#include <cstdlib>
#include <exception>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

#ifdef _MSC_VER
#pragma comment(lib, "psapi.lib")
#endif

namespace {

constexpr qint64 DIAGNOSTIC_LOG_MAX_BYTES = 8 * 1024 * 1024;

QMutex gLogMutex;
QFile gLogFile;
qint64 gLogBytesWritten = 0;
int gLogLinesUntilFlush = 0;

const char *messageTypeName(QtMsgType type) {
    switch (type) {
    case QtDebugMsg:
        return "DEBUG";
    case QtInfoMsg:
        return "INFO";
    case QtWarningMsg:
        return "WARN";
    case QtCriticalMsg:
        return "CRITICAL";
    case QtFatalMsg:
        return "FATAL";
    }
    return "LOG";
}

bool shouldWriteDiagnosticLogLine(QtMsgType type, const QString &message) {
    if ((type != QtDebugMsg && type != QtInfoMsg) || fobosVerboseLoggingEnabled()) {
        return true;
    }

    return message.contains(QStringLiteral("[Log]")) ||
           message.contains(QStringLiteral("[Crash]"));
}

void diagnosticMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message) {
    const bool writeDiagnosticOutput = shouldWriteDiagnosticLogLine(type, message);
    const bool echoDebugOutput =
        (type != QtDebugMsg && type != QtInfoMsg) || fobosVerboseLoggingEnabled();
    if (!writeDiagnosticOutput && !echoDebugOutput) {
        return;
    }
    const QString line = QString("%1 [%2] [tid 0x%3] %4%5")
                             .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz"))
                             .arg(messageTypeName(type))
                             .arg(reinterpret_cast<quintptr>(QThread::currentThreadId()), 0, 16)
                             .arg(message)
                             .arg(context.file ? QString(" (%1:%2)").arg(context.file).arg(context.line) : QString());

    if (writeDiagnosticOutput) {
        QMutexLocker lock(&gLogMutex);
        if (gLogFile.isOpen()) {
            if (gLogBytesWritten >= DIAGNOSTIC_LOG_MAX_BYTES) {
                const QString currentPath = gLogFile.fileName();
                gLogFile.close();
                QFile::remove(currentPath + QStringLiteral(".1"));
                QFile::rename(currentPath, currentPath + QStringLiteral(".1"));
                gLogFile.setFileName(currentPath);
                gLogFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Append);
                gLogBytesWritten = 0;
                gLogLinesUntilFlush = 0;
            }

            const QByteArray encodedLine = line.toUtf8();
            gLogFile.write(encodedLine);
            gLogFile.write("\n");
            gLogBytesWritten += encodedLine.size() + 1;
            ++gLogLinesUntilFlush;
            if (type != QtDebugMsg || gLogLinesUntilFlush >= 64) {
                gLogFile.flush();
                gLogLinesUntilFlush = 0;
            }
        }
    }

    if (echoDebugOutput) {
        const QByteArray localLine = line.toLocal8Bit();
        std::fprintf(stderr, "%s\n", localLine.constData());
        std::fflush(stderr);
    }

#ifdef _WIN32
    if (echoDebugOutput) {
        const std::wstring debugLine = (line + "\n").toStdWString();
        OutputDebugStringW(debugLine.c_str());
    }
#endif

    if (type == QtFatalMsg) {
        std::abort();
    }
}

#ifdef _WIN32
QString modulePathForAddress(void *address) {
    if (!address) {
        return QString();
    }

    MEMORY_BASIC_INFORMATION memoryInfo;
    ZeroMemory(&memoryInfo, sizeof(memoryInfo));
    if (!VirtualQuery(address, &memoryInfo, sizeof(memoryInfo)) || !memoryInfo.AllocationBase) {
        return QString();
    }

    wchar_t modulePath[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(static_cast<HMODULE>(memoryInfo.AllocationBase),
                                            modulePath,
                                            MAX_PATH);
    if (length == 0) {
        return QString();
    }
    return QString::fromWCharArray(modulePath, static_cast<int>(length));
}

LONG WINAPI diagnosticUnhandledExceptionFilter(EXCEPTION_POINTERS *exceptionInfo) {
    if (!exceptionInfo || !exceptionInfo->ExceptionRecord) {
        qCritical() << "[Crash] unhandled Windows exception without exception record";
        return EXCEPTION_EXECUTE_HANDLER;
    }

    EXCEPTION_RECORD *record = exceptionInfo->ExceptionRecord;
    void *address = record->ExceptionAddress;
    const QString modulePath = modulePathForAddress(address);
    qCritical() << "[Crash] unhandled Windows exception"
                << "code" << QString("0x%1").arg(static_cast<quint32>(record->ExceptionCode), 8, 16, QChar('0'))
                << "address" << address
                << "module" << (modulePath.isEmpty() ? QString("unknown") : QDir::toNativeSeparators(modulePath))
                << "parameters" << static_cast<quint32>(record->NumberParameters);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

void diagnosticTerminateHandler() {
    qCritical() << "[Crash] std::terminate called";
    std::abort();
}

} // namespace

void installDiagnosticLogger() {
    const QString logPath = QDir(QCoreApplication::applicationDirPath()).filePath("ObriiSDR_diagnostic.log");
    if (QFileInfo(logPath).size() >= DIAGNOSTIC_LOG_MAX_BYTES) {
        QFile::remove(logPath + QStringLiteral(".1"));
        QFile::rename(logPath, logPath + QStringLiteral(".1"));
    }
    gLogFile.setFileName(logPath);
    gLogFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Append);
    gLogBytesWritten = gLogFile.isOpen() ? gLogFile.size() : 0;
    gLogLinesUntilFlush = 0;
    qInstallMessageHandler(diagnosticMessageHandler);
    qDebug() << "[Log] ===== Diagnostic session started =====";
    qDebug() << "[Log] Diagnostic log path:" << QDir::toNativeSeparators(logPath)
             << "fileOpen" << gLogFile.isOpen();
    qDebug() << "[Log] Verbose diagnostic logging"
             << (fobosVerboseLoggingEnabled() ? "enabled" : "disabled");
}

void installCrashLogger() {
#ifdef _WIN32
    SetUnhandledExceptionFilter(diagnosticUnhandledExceptionFilter);
#endif
    std::set_terminate(diagnosticTerminateHandler);
    qDebug() << "[Log] Crash logger installed";
}

void logMemorySnapshot(const char *tag) {
    if (!fobosVerboseLoggingEnabled()) {
        return;
    }

#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters;
    ZeroMemory(&counters, sizeof(counters));
    counters.cb = sizeof(counters);

    MEMORYSTATUSEX memoryStatus;
    ZeroMemory(&memoryStatus, sizeof(memoryStatus));
    memoryStatus.dwLength = sizeof(memoryStatus);

    const bool processOk = GetProcessMemoryInfo(GetCurrentProcess(),
                                                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                                sizeof(counters)) != 0;
    const bool systemOk = GlobalMemoryStatusEx(&memoryStatus) != 0;
    if (!processOk && !systemOk) {
        qDebug() << "[Memory]" << tag << "unavailable";
        return;
    }

    const auto toMb = [](quint64 bytes) {
        return static_cast<double>(bytes) / (1024.0 * 1024.0);
    };

    qDebug() << "[Memory]" << tag
             << "workingSetMB" << (processOk ? toMb(counters.WorkingSetSize) : -1.0)
             << "privateMB" << (processOk ? toMb(counters.PrivateUsage) : -1.0)
             << "availPhysMB" << (systemOk ? toMb(memoryStatus.ullAvailPhys) : -1.0)
             << "memoryLoadPct" << (systemOk ? static_cast<int>(memoryStatus.dwMemoryLoad) : -1);
#else
    qDebug() << "[Memory]" << tag << "snapshot unavailable on this platform";
#endif
}
