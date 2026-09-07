#ifndef UPDATEMANAGER_H
#define UPDATEMANAGER_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>

class QWidget;

class UpdateManager : public QObject {
    Q_OBJECT

public:
    explicit UpdateManager(QObject *parent = nullptr) : QObject(parent) {}
    void checkForUpdates(QWidget *parent);

signals:
    void updateCheckFinished(bool updateAvailable);

private slots:
    void handleReleaseReply();
    void handleInstallerReply();

private:
    void showUpdateDialog(QWidget *parent);
    void downloadInstaller();

    QNetworkAccessManager networkManager;
    QNetworkReply *releaseReply = nullptr;
    QNetworkReply *installerReply = nullptr;
    QWidget *dialogParent = nullptr;
    QString installerUrl;
    QString releaseNotes;
    QString releaseVersion;
    QString releasePageUrl;
};

#endif // UPDATEMANAGER_H
