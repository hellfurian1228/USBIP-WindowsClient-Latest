#include "updatemanager.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QStandardPaths>
#include <QUrl>
#include <QVersionNumber>
#include <QProcess>
#include <QDir>

namespace {
constexpr const char *releaseApiUrl =
    "https://api.github.com/repos/hellfurian1228/USBIP-WindowsClient/releases/latest";
constexpr const char *installerAssetName = "USBIPClient_Installer.exe";
}

void UpdateManager::checkForUpdates(QWidget *parent) {
    if (releaseReply) {
        return;
    }

    dialogParent = parent;
    QNetworkRequest request{QUrl(QString::fromLatin1(releaseApiUrl))};
    request.setHeader(QNetworkRequest::UserAgentHeader, "USBIPClient");
    request.setRawHeader("Accept", "application/vnd.github+json");
    releaseReply = networkManager.get(request);
    connect(releaseReply, &QNetworkReply::finished,
            this, &UpdateManager::handleReleaseReply);
}

void UpdateManager::handleReleaseReply() {
    QNetworkReply *reply = releaseReply;
    releaseReply = nullptr;

    if (reply->error() != QNetworkReply::NoError) {
        emit updateCheckFinished(false);
        reply->deleteLater();
        return;
    }

    const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
    if (!document.isObject()) {
        emit updateCheckFinished(false);
        reply->deleteLater();
        return;
    }

    const QJsonObject release = document.object();
    const QString tagName = release.value("tag_name").toString();
    const QVersionNumber latestVersion = QVersionNumber::fromString(tagName.startsWith('v')
                                                                        ? tagName.mid(1)
                                                                        : tagName);
    const QVersionNumber currentVersion = QVersionNumber::fromString(
        QStringLiteral(USBIP_CLIENT_VERSION));
    
    if (latestVersion.isNull() || currentVersion.isNull() || latestVersion <= currentVersion) {
        emit updateCheckFinished(false);
        reply->deleteLater();
        return;
    }

    releaseVersion = tagName;
    releaseNotes = release.value("body").toString().trimmed();
    releasePageUrl = release.value("html_url").toString();
    const QJsonArray assets = release.value("assets").toArray();
    for (const QJsonValue &assetValue : assets) {
        const QJsonObject asset = assetValue.toObject();
        const QString name = asset.value("name").toString();
        if (name.compare(QString::fromLatin1(installerAssetName), Qt::CaseInsensitive) == 0) {
            installerUrl = asset.value("browser_download_url").toString();
            break;
        }
    }

    emit updateCheckFinished(true);
    showUpdateDialog(dialogParent);
    
    // Clean up safely at the very end
    reply->deleteLater();
}

void UpdateManager::showUpdateDialog(QWidget *parent) {
    QMessageBox messageBox(parent);
    messageBox.setWindowTitle("USBIP Client update available");
    messageBox.setIcon(QMessageBox::Information);
    messageBox.setText(QString("USBIP Client %1 is available (you have %2).")
                           .arg(releaseVersion, QStringLiteral(USBIP_CLIENT_VERSION)));
    messageBox.setInformativeText(releaseNotes.isEmpty()
                                      ? "No change log was provided for this release."
                                      : releaseNotes);
    QPushButton *updateButton = nullptr;
    if (!installerUrl.isEmpty()) {
        updateButton = messageBox.addButton("Download and Install", QMessageBox::AcceptRole);
    } else {
        updateButton = messageBox.addButton("Open Release Page", QMessageBox::AcceptRole);
    }
    messageBox.addButton("Later", QMessageBox::RejectRole);
    messageBox.exec();

    if (messageBox.clickedButton() == updateButton) {
        if (!installerUrl.isEmpty()) {
            downloadInstaller();
        } else {
            QDesktopServices::openUrl(QUrl(releasePageUrl));
        }
    }
}

void UpdateManager::downloadInstaller() {
    QNetworkRequest request{QUrl(installerUrl)};
    request.setHeader(QNetworkRequest::UserAgentHeader, "USBIPClient");
    installerReply = networkManager.get(request);

    auto *progress = new QProgressDialog("Downloading USBIP Client update...", "Cancel", 0, 100,
                                         dialogParent);
    progress->setWindowTitle("USBIP Client update");
    progress->setWindowModality(Qt::WindowModal);
    progress->setAutoClose(false);
    connect(progress, &QProgressDialog::canceled, installerReply, &QNetworkReply::abort);
    connect(installerReply, &QNetworkReply::downloadProgress, progress,
            [progress](qint64 received, qint64 total) {
                progress->setMaximum(total > 0 ? 100 : 0);
                if (total > 0) {
                    progress->setValue(static_cast<int>((received * 100) / total));
                }
            });
    connect(installerReply, &QNetworkReply::finished, this, [this, progress]() {
        progress->deleteLater();
        handleInstallerReply();
    });
    progress->show();
}

void UpdateManager::handleInstallerReply() {
    QNetworkReply *reply = installerReply;
    installerReply = nullptr;

    if (reply->error() != QNetworkReply::NoError) {
        if (reply->error() == QNetworkReply::OperationCanceledError) {
            reply->deleteLater();
            return;
        }
        QMessageBox::warning(dialogParent, "Update failed",
                             QString("The update could not be downloaded: %1")
                                 .arg(reply->errorString()));
        reply->deleteLater();
        return;
    }

    const QString tempDirectory = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    const QString installerPath = QDir(tempDirectory).filePath("USBIPClient_Update.exe");
    QFile installerFile(installerPath);
    if (!installerFile.open(QIODevice::WriteOnly) || installerFile.write(reply->readAll()) < 0) {
        QMessageBox::warning(dialogParent, "Update failed",
                             "The downloaded installer could not be saved.");
        reply->deleteLater();
        return;
    }
    installerFile.close();
    reply->deleteLater();

    if (!QProcess::startDetached(installerPath, {})) {
        QMessageBox::warning(dialogParent, "Update failed",
                             "The downloaded installer could not be started.");
        return;
    }

    QCoreApplication::quit();
}
