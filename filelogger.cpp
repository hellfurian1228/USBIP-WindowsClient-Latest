#include "filelogger.h"

#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QtGlobal>
#include <windows.h>

namespace {
QMutex loggerMutex;
QString logsDirectory;

QString dailyLogPath() {
    return QDir(logsDirectory).filePath(
        QStringLiteral("USBIPClient_%1.txt").arg(QDate::currentDate().toString("yyyyMMdd")));
}

void writeLine(const QString &line) {
    QMutexLocker locker(&loggerMutex);
    if (logsDirectory.isEmpty())
        return;

    QFile file(dailyLogPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;
    file.write(line.toUtf8());
    file.flush();
}

void qtMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message) {
    Q_UNUSED(context);
    QString level = QStringLiteral("INFO");
    if (type == QtWarningMsg)
        level = QStringLiteral("WARNING");
    else if (type == QtCriticalMsg || type == QtFatalMsg)
        level = QStringLiteral("ERROR");
    writeLine(QStringLiteral("%1 [%2] %3\r\n")
                  .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz"), level, message));
}

wchar_t crashLogPath[MAX_PATH] = {};

LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS *exceptionInfo) {
    if (crashLogPath[0] != L'\0' && exceptionInfo && exceptionInfo->ExceptionRecord) {
        const DWORD code = exceptionInfo->ExceptionRecord->ExceptionCode;
        HANDLE file = CreateFileW(crashLogPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            SYSTEMTIME time;
            GetLocalTime(&time);
            wchar_t line[256] = {};
            const int length = swprintf_s(line, _countof(line),
                L"%04u-%02u-%02u %02u:%02u:%02u.%03u [CRASH] Unhandled Windows exception 0x%08lX\r\n",
                time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
                time.wSecond, time.wMilliseconds, code);
            DWORD written = 0;
            WriteFile(file, line, static_cast<DWORD>(length * sizeof(wchar_t)), &written, nullptr);
            CloseHandle(file);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
}

namespace FileLogger {
void initialize() {
    logsDirectory = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("Logs"));
    QDir().mkpath(logsDirectory);

    const QString crashPath = QDir(logsDirectory).filePath(QStringLiteral("USBIPClient_Crash.txt"));
    wcsncpy_s(crashLogPath, crashPath.toStdWString().c_str(), _TRUNCATE);
    SetUnhandledExceptionFilter(unhandledExceptionFilter);
}

void write(const QString &level, const QString &message) {
    writeLine(QStringLiteral("%1 [%2] %3\r\n")
                  .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz"), level, message));
}

void installQtMessageHandler() {
    qInstallMessageHandler(qtMessageHandler);
}
}
