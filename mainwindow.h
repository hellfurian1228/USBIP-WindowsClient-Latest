#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTabWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QDockWidget>
#include <QComboBox>
#include <QCheckBox>
#include <QLabel>
#include <QSettings>
#include <QCloseEvent>
#include <QHash>
#include <QTimer>
#include <QSystemTrayIcon>
#include <QTcpSocket>
#include <QHostAddress>
#include <QByteArray>
#include <QList>
#include <QStringList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QElapsedTimer>
#include <QPixmap>
#include "logwindow.h"
#include "wifimanager.h"
#include <wtypes.h>
#include <usbspec.h>

namespace usbip { class UsbIds; }

class NsdDiscoveryManager;
class UpdateManager;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

protected:
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void handleConnect();
    void handleScanHost();
    void handleToggleLogWindow();
    void handleResetDeviceConnection(int row);
    void handleToggleDeviceAttach(int row);
    void handleThemeChange(int index);
    void handleNewProfile();
    void handleProfileChange(const QString &profileName);
    void refreshTelemetryStats();
    void handleHostDiscovered(const QString &hostname, const QHostAddress &address,
                              quint16 port, quint16 telemetryPort, int interfaceIndex);
    void checkHostConnection();
    void handleHostConnectionEstablished();
    void handleHostConnectionError(QAbstractSocket::SocketError error);
    void handleWifiScan();
    void handleWifiConnect();
    void handleTelemetryReply();

private:
    void setupUi();
    void applyTheme(const QString &themeName);
    void updateArtworkBackground();
    QWidget* createNetworkTab();

    QWidget* createSettingsTab();
    void addUsbDeviceToTable(const QString &name, const QString &busId, const QString &vidPid, const QString &status, bool attached, USB_DEVICE_SPEED detectedSpeed = UsbHighSpeed);
    bool validatePort(quint16 port);
    void loadSettings();
    void saveSettings();
    void loadProfileSettings(const QString &profileName);
    void saveProfileSettings(const QString &profileName);
    QString getFriendlyDeviceName(quint16 vendorId, quint16 productId);
    void loadUsbIdDatabase();
    // Returns the vhci hub port# for a given busid, or -1 if not attached
    int findAttachedPort(const QString &busid) const;
    QStringList getFavorites() const;
    void setFavorite(const QString &vidPid, bool favorite);
    void clearDeviceTable();
    QString getFreshBusId(const QString &targetVidPid);
    void syncDeviceStates();
    bool probeHost(const QString &ip, quint16 port, QString *error = nullptr);
    void markHostDisconnected(const QString &reason);
    void populateWifiNetworks();

    QTabWidget *tabWidget;
    QLabel *artistCredit;
    QPixmap artworkPixmap;
    QLineEdit *hostIpLineEdit;
    QLineEdit *portLineEdit;
    QPushButton *connectButton;
    QPushButton *scanHostButton;
    QPushButton *loggerButton;
    QTableWidget *usbDeviceTable;
    QLabel *connectionStatusLabel;
    QComboBox *discoveredHostCombo;
    QComboBox *wifiNetworkCombo;
    QPushButton *wifiScanButton;
    QPushButton *wifiConnectButton;
    QPushButton *wifiDisconnectButton;

    // Settings Tab Controls
    QComboBox *themeCombo;
    QCheckBox *minimizeToTrayCheckBox;
    QCheckBox *autoConnectCheckBox;
    QComboBox *profileCombo;

    QDockWidget *telemetryDock;
    QTableWidget *telemetryTable;
    QTimer *telemetryUpdateTimer;
    QTimer *connectionMonitorTimer;
    QTimer *connectionProbeTimeoutTimer;
    QTcpSocket *connectionMonitorSocket;
    bool connectionProbeInProgress = false;

    LogWindow *logWindow;
    bool isLogicallyConnected = false;
    
    QByteArray usbIdsData;
    usbip::UsbIds *usbIdsDb = nullptr;
    QHash<QString, QString> remoteClassByBusId;

    // Maps busid -> vhci hub port number (>= 1) for currently attached devices
    QHash<QString, int> attachedPorts;
    QSystemTrayIcon *trayIcon;
    bool isExiting = false;
    QString currentProfile;
    NsdDiscoveryManager *nsdDiscoveryManager;
    WifiManager wifiManager;
    QList<WifiNetwork> wifiNetworks;
    UpdateManager *updateManager;
    bool recoveryInProgress = false;

    struct TelemetrySample {
        quint64 totalBytes = 0;
        qint64 timestampMs = 0;
    };
    struct TelemetryDisplay {
        QString throughput = "Unavailable";
        QString latency = "Unavailable";
        QString jitter = "Unavailable";
    };
    QNetworkAccessManager telemetryNetworkManager;
    QNetworkReply *telemetryReply = nullptr;
    QElapsedTimer telemetryClock;
    QHash<QString, TelemetrySample> previousTelemetrySamples;
    QHash<QString, TelemetryDisplay> telemetryByBusId;
    quint16 telemetryPort = 3241;
    QString telemetryEndpoint;
};

#endif // MAINWINDOW_H
