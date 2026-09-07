#include "logwindow.h"
#include "filelogger.h"
#include <QDateTime>
#include <QGuiApplication>
#include <QClipboard>

LogWindow::LogWindow(QWidget *parent) : QDialog(parent) {
    setWindowTitle("USBIP Client System Logger");
    resize(700, 450);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    QHBoxLayout *topBarLayout = new QHBoxLayout();
    filterComboBox = new QComboBox(this);
    filterComboBox->addItems({"All Logs", "INFO", "WARNING", "ERROR"});
    
    clearButton = new QPushButton("Clear Logs", this);
    copyButton = new QPushButton("Copy to Clipboard", this);

    topBarLayout->addWidget(filterComboBox);
    topBarLayout->addStretch();
    topBarLayout->addWidget(copyButton);
    topBarLayout->addWidget(clearButton);

    logTextEdit = new QTextEdit(this);
    logTextEdit->setReadOnly(true);
    logTextEdit->setStyleSheet("font-family: 'Consolas', 'Courier New', monospace; font-size: 12px; background-color: #0d0e15; color: #00ffcc;");

    mainLayout->addLayout(topBarLayout);
    mainLayout->addWidget(logTextEdit);

    // Wire up the new filter slot
    connect(filterComboBox, &QComboBox::currentTextChanged, this, &LogWindow::applyFilter);
    connect(clearButton, &QPushButton::clicked, this, &LogWindow::clearLogs);
    connect(copyButton, &QPushButton::clicked, this, &LogWindow::copyLogs);
}

void LogWindow::appendLog(const QString &level, const QString &message) {
    QString timeStr = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz");
    FileLogger::write(level, message);
    QString color = "#00ffcc";
    if (level == "WARNING") color = "#ffaa00";
    if (level == "ERROR") color = "#ff3366";

    QString formattedEntry = QString("<span style='color:#777777;'>[%1]</span> <b style='color:%2;'>[%3]</b> %4")
                                .arg(timeStr, color, level, message.toHtmlEscaped());
    
    // Store the log in memory
    logEntries.append({level, formattedEntry});

    // Only append to the UI if it matches the current filter
    QString currentFilter = filterComboBox->currentText();
    if (currentFilter == "All Logs" || currentFilter == level) {
        logTextEdit->append(formattedEntry);
    }
}

void LogWindow::applyFilter(const QString &filter) {
    logTextEdit->clear();
    for (const auto &entry : logEntries) {
        if (filter == "All Logs" || entry.first == filter) {
            logTextEdit->append(entry.second);
        }
    }
}

void LogWindow::clearLogs() {
    logEntries.clear(); // Clear the underlying data structure
    logTextEdit->clear();
}

void LogWindow::copyLogs() {
    QClipboard *clipboard = QGuiApplication::clipboard();
    clipboard->setText(logTextEdit->toPlainText());
}
