#ifndef FILELOGGER_H
#define FILELOGGER_H

#include <QString>

namespace FileLogger {
void initialize();
void write(const QString &level, const QString &message);
void installQtMessageHandler();
}

#endif // FILELOGGER_H
