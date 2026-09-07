#include "mainwindow.h"
#include "driverinstaller.h"
#include "nsddiscoverymanager.h"
#include "updatemanager.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QMessageBox>
#include <QInputDialog>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QFile>
#include <QSet>
#include <QMenuBar>
#include <QStyle>
#include <QThread>
#include <QPainter>
#include <QResizeEvent>
#include <windows.h>
#include <lm.h>
#include <dxgi.h>
#include <wincrypt.h>

#include <vhci.h>
#include <remote.h>
#include "src/usbip_sdk/libusbip/src/usb_ids.h"
#include "src/transport/usb_transport.h"

#pragma comment(lib, "netapi32.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "crypt32.lib")

namespace {
QString usbSpeedLabel(USB_DEVICE_SPEED speed)
{
    switch (speed) {
    case UsbSuperSpeed:
        return QStringLiteral("Super (5 Gbps)");
    case UsbHighSpeed:
        return QStringLiteral("High (480 Mbps)");
    case UsbFullSpeed:
        return QStringLiteral("Full (12 Mbps)");
    case UsbLowSpeed:
    default:
        return QStringLiteral("Low (1.5 Mbps)");
    }
}
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent) {
    setWindowTitle(QString("USBIP Client v%1").arg(QCoreApplication::applicationVersion()));
    resize(1000, 650);

    logWindow = new LogWindow(this);
    nsdDiscoveryManager = new NsdDiscoveryManager(this);
    connect(nsdDiscoveryManager, &NsdDiscoveryManager::hostDiscovered,
            this, &MainWindow::handleHostDiscovered);

    setupUi();
    loadSettings();

    connectionMonitorTimer = new QTimer(this);
    connectionMonitorTimer->setInterval(5000);
    connect(connectionMonitorTimer, &QTimer::timeout, this, &MainWindow::checkHostConnection);

    connectionProbeTimeoutTimer = new QTimer(this);
    connectionProbeTimeoutTimer->setSingleShot(true);
    connectionProbeTimeoutTimer->setInterval(2500);
    connect(connectionProbeTimeoutTimer, &QTimer::timeout, this, [this]() {
        if (!connectionProbeInProgress)
            return;
        connectionProbeInProgress = false;
        connectionMonitorSocket->abort();
        markHostDisconnected("Connection monitor timed out.");
    });

    connectionMonitorSocket = new QTcpSocket(this);
    connect(connectionMonitorSocket, &QTcpSocket::connected,
            this, &MainWindow::handleHostConnectionEstablished);
    connect(connectionMonitorSocket, &QTcpSocket::errorOccurred,
            this, &MainWindow::handleHostConnectionError);

    QString driverError;
    if (DriverInstaller::install(&driverError)) {
        logWindow->appendLog("INFO", "Kernel driver installation succeeded.");
    } else {
        logWindow->appendLog("ERROR", QString("Kernel driver installation failed: %1").arg(driverError));
    }

    loadUsbIdDatabase();

    trayIcon = new QSystemTrayIcon(this);
    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        QIcon icon;
        if (style()) {
            icon = style()->standardIcon(QStyle::SP_ComputerIcon);
        }
        trayIcon->setIcon(icon);

        QMenu *trayMenu = new QMenu(this);
        QAction *restoreAction = trayMenu->addAction("Restore");
        connect(restoreAction, &QAction::triggered, this, &MainWindow::showNormal);
        QAction *exitAction = trayMenu->addAction("Exit");
        connect(exitAction, &QAction::triggered, this, [this]() {
            isExiting = true;
            close();
        });
        trayIcon->setContextMenu(trayMenu);
        connect(trayIcon, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::DoubleClick || reason == QSystemTrayIcon::Trigger) {
                showNormal();
                activateWindow();
                raise();
            }
        });
        trayIcon->show();
    }

    logWindow->appendLog("INFO", "USBIP Client initialized successfully.");

    updateManager = new UpdateManager(this);
    QTimer::singleShot(1500, this, [this]() {
        updateManager->checkForUpdates(this);
    });

    nsdDiscoveryManager->startDiscovery();

    if (autoConnectCheckBox->isChecked()) {
        logWindow->appendLog("INFO", "Auto-connect enabled. Initiating startup connection...");
        handleConnect();
    }
}

MainWindow::~MainWindow() {
    nsdDiscoveryManager->stopDiscovery();
    delete usbIdsDb;
}

void MainWindow::closeEvent(QCloseEvent *event) {
    saveSettings();

    if (minimizeToTrayCheckBox->isChecked() && !isExiting && QSystemTrayIcon::isSystemTrayAvailable()) {
        event->ignore();
        hide();
        trayIcon->showMessage("USBIP Client", "Application minimized to system tray.", QSystemTrayIcon::Information, 2000);
    } else {
        event->accept();
    }
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    updateArtworkBackground();
}

void MainWindow::loadSettings() {
    QSettings settings("USBIPClient", "USBIPClient");
    
    QStringList profiles = settings.value("profiles/list", QStringList() << "Default").toStringList();
    currentProfile = settings.value("profiles/active", "Default").toString();
    
    profileCombo->blockSignals(true);
    profileCombo->clear();
    profileCombo->addItems(profiles);
    int profileIndex = profileCombo->findText(currentProfile);
    if (profileIndex >= 0) {
        profileCombo->setCurrentIndex(profileIndex);
    } else {
        profileCombo->setCurrentIndex(0);
        currentProfile = profileCombo->currentText();
    }
    profileCombo->blockSignals(false);
    
    loadProfileSettings(currentProfile);
}

void MainWindow::saveSettings() {
    QSettings settings("USBIPClient", "USBIPClient");
    
    QStringList profiles;
    for (int i = 0; i < profileCombo->count(); ++i) {
        profiles.append(profileCombo->itemText(i));
    }
    settings.setValue("profiles/list", profiles);
    settings.setValue("profiles/active", currentProfile);
    
    saveProfileSettings(currentProfile);
}

void MainWindow::loadProfileSettings(const QString &profileName) {
    QSettings settings("USBIPClient", "USBIPClient");
    settings.beginGroup("profiles/" + profileName);
    
    QString savedIp = settings.value("hostIp", "192.168.1.11").toString();
    QString savedPort = settings.value("port", "3240").toString();
    QString savedTheme = settings.value("theme", "Dark").toString();
    if (themeCombo->findText(savedTheme) < 0) {
        savedTheme = "Dark";
    }
    bool savedAutoConnect = settings.value("autoConnect", false).toBool();
    bool savedMinimizeToTray = settings.value("minimizeToTray", false).toBool();
    
    settings.endGroup();

    hostIpLineEdit->setText(savedIp);
    portLineEdit->setText(savedPort);
    
    int themeIndex = themeCombo->findText(savedTheme);
    themeCombo->blockSignals(true);
    if (themeIndex >= 0) {
        themeCombo->setCurrentIndex(themeIndex);
    }
    themeCombo->blockSignals(false);
    applyTheme(savedTheme);

    autoConnectCheckBox->setChecked(savedAutoConnect);
    minimizeToTrayCheckBox->setChecked(savedMinimizeToTray);

    logWindow->appendLog("INFO", QString("Loaded profile '%1' settings. Last host IP: %2:%3").arg(profileName, savedIp, savedPort));
}

void MainWindow::saveProfileSettings(const QString &profileName) {
    QSettings settings("USBIPClient", "USBIPClient");
    settings.beginGroup("profiles/" + profileName);
    
    settings.setValue("hostIp", hostIpLineEdit->text().trimmed());
    settings.setValue("port", portLineEdit->text().trimmed());
    settings.setValue("theme", themeCombo->currentText());
    settings.setValue("autoConnect", autoConnectCheckBox->isChecked());
    settings.setValue("minimizeToTray", minimizeToTrayCheckBox->isChecked());
    
    settings.endGroup();
    logWindow->appendLog("INFO", QString("Saved settings for profile '%1'.").arg(profileName));
}

bool MainWindow::validatePort(quint16 port) {
    if (port < 3240 || port > 3260) {
        logWindow->appendLog("ERROR", QString("Port %1 out of bounds. Allowed range: 3240-3260.").arg(port));
        QMessageBox::critical(this, "Port Error", "Invalid port specified! USBIP Client requires a port between 3240 and 3260.");
        return false;
    }
    return true;
}

void MainWindow::setupUi() {
    QWidget *centralWidget = new QWidget(this);
    centralWidget->setObjectName("centralWidget");
    QVBoxLayout *mainLayout = new QVBoxLayout(centralWidget);

    QHBoxLayout *topBarLayout = new QHBoxLayout();
    
    QLabel *ipLabel = new QLabel("Host IP:", this);
    hostIpLineEdit = new QLineEdit("192.168.1.11", this);
    
    QLabel *portLabel = new QLabel("Port (3240-3260):", this);
    portLineEdit = new QLineEdit("3240", this);
    portLineEdit->setFixedWidth(60);

    connectButton = new QPushButton("Connect", this);
    connectButton->setToolTip("Verify the remote USB/IP host and connect.");
    scanHostButton = new QPushButton("Scan Host", this);
    scanHostButton->setToolTip("Query the host for available and exportable USB devices.");
    loggerButton = new QPushButton("Debug Console", this);
    loggerButton->setToolTip("Toggle the debug console to view application logs and errors.");
    connectionStatusLabel = new QLabel("Status: Disconnected", this);

    artistCredit = new QLabel("Warpedskull", this);
    artistCredit->setVisible(false);
    artistCredit->setObjectName("artistCredit");
    artistCredit->setToolTip("Artwork by Warpedskull");

    QGroupBox *networkGroup = new QGroupBox("Network", this);
    QHBoxLayout *networkLayout = new QHBoxLayout(networkGroup);
    networkLayout->setContentsMargins(8, 4, 8, 4);
    discoveredHostCombo = new QComboBox(this);
    discoveredHostCombo->setMinimumWidth(160);
    discoveredHostCombo->setToolTip("Android hosts discovered via mDNS. Select one to populate the Host IP field.");
    discoveredHostCombo->addItem("-- mDNS Hosts --");
    connect(discoveredHostCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index <= 0)
            return;

        const QStringList endpoint = discoveredHostCombo->itemData(index).toString().split('|');
        if (endpoint.size() < 2)
            return;

        hostIpLineEdit->setText(endpoint.at(0));
        portLineEdit->setText(endpoint.at(1));
        telemetryPort = endpoint.size() >= 3 ? endpoint.at(2).toUShort() : 3241;
        previousTelemetrySamples.clear();
        telemetryByBusId.clear();
    });

    topBarLayout->addWidget(ipLabel);
    topBarLayout->addWidget(hostIpLineEdit);
    topBarLayout->addWidget(portLabel);
    topBarLayout->addWidget(portLineEdit);
    topBarLayout->addWidget(connectButton);
    topBarLayout->addWidget(scanHostButton);
    topBarLayout->addWidget(connectionStatusLabel);
    topBarLayout->addWidget(discoveredHostCombo);
    topBarLayout->addStretch();
    topBarLayout->addWidget(loggerButton);

    QLabel *wifiLabel = new QLabel("Networks:", networkGroup);
    wifiNetworkCombo = new QComboBox(this);
    wifiNetworkCombo->setMinimumWidth(200);
    wifiNetworkCombo->setToolTip("Select a visible Wi-Fi network.");
    connect(wifiNetworkCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) {
                const bool connected = index >= 0 && index < wifiNetworks.size() &&
                    wifiNetworks.at(index).connected;
                const bool ethernet = index >= 0 && index < wifiNetworks.size() &&
                    wifiNetworks.at(index).ethernet;
                wifiConnectButton->setEnabled(index >= 0 && index < wifiNetworks.size() && !connected && !ethernet);
                wifiDisconnectButton->setEnabled(connected && !ethernet);
                if (index >= 0 && index < wifiNetworks.size()) {
                    nsdDiscoveryManager->setInterfaceIndex(wifiNetworks.at(index).networkInterfaceIndex);
                    discoveredHostCombo->setCurrentIndex(0);
                    for (int hostIndex = discoveredHostCombo->count() - 1; hostIndex > 0; --hostIndex)
                        discoveredHostCombo->removeItem(hostIndex);
                    logWindow->appendLog("INFO", QString("Network selected: %1. mDNS restricted to this network.")
                        .arg(wifiNetworks.at(index).ssid));
                }
            });
    networkLayout->addWidget(wifiLabel);
    networkLayout->addWidget(wifiNetworkCombo);
    wifiScanButton = new QPushButton("Refresh", networkGroup);
    wifiScanButton->setToolTip("Scan for available Wi-Fi networks.");
    networkLayout->addWidget(wifiScanButton);
    wifiConnectButton = new QPushButton("Connect", networkGroup);
    wifiConnectButton->setToolTip("Connect to the selected Wi-Fi network.");
    networkLayout->addWidget(wifiConnectButton);
    wifiDisconnectButton = new QPushButton("Disconnect", networkGroup);
    wifiDisconnectButton->setToolTip("Disconnect the selected Wi-Fi network.");
    wifiDisconnectButton->setEnabled(false);
    networkLayout->addWidget(wifiDisconnectButton);

    tabWidget = new QTabWidget(this);
    tabWidget->addTab(createNetworkTab(), "Network & USB/IP");
    tabWidget->addTab(createSettingsTab(), "Settings");

    mainLayout->addLayout(topBarLayout);
    mainLayout->addWidget(networkGroup);
    mainLayout->addWidget(tabWidget);

    setCentralWidget(centralWidget);
    updateArtworkBackground();

    telemetryTable = new QTableWidget(this);
    telemetryTable->setColumnCount(7);
    telemetryTable->setHorizontalHeaderLabels({"Bus ID", "Device", "Speed", "Class", "Latency", "Jitter", "Throughput"});
    QHeaderView *telemetryHeader = telemetryTable->horizontalHeader();
    telemetryHeader->setSectionResizeMode(QHeaderView::Interactive);
    telemetryHeader->setMinimumSectionSize(80);
    telemetryHeader->resizeSection(0, 110);
    telemetryHeader->resizeSection(1, 180);
    telemetryHeader->resizeSection(2, 150);
    telemetryHeader->resizeSection(3, 120);
    telemetryHeader->resizeSection(4, 100);
    telemetryHeader->resizeSection(5, 100);
    telemetryHeader->resizeSection(6, 120);

    telemetryDock = new QDockWidget("Live Device Telemetry", this);
    telemetryDock->setWidget(telemetryTable);
    addDockWidget(Qt::BottomDockWidgetArea, telemetryDock);

    QMenu *viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addAction(telemetryDock->toggleViewAction());
    menuBar()->setCornerWidget(artistCredit, Qt::TopRightCorner);

    telemetryUpdateTimer = new QTimer(this);
    connect(telemetryUpdateTimer, &QTimer::timeout, this, &MainWindow::refreshTelemetryStats);
    telemetryUpdateTimer->start(1000);

    connect(connectButton, &QPushButton::clicked, this, &MainWindow::handleConnect);
    connect(scanHostButton, &QPushButton::clicked, this, &MainWindow::handleScanHost);
    connect(loggerButton, &QPushButton::clicked, this, &MainWindow::handleToggleLogWindow);
    connect(wifiScanButton, &QPushButton::clicked, this, &MainWindow::handleWifiScan);
    connect(wifiConnectButton, &QPushButton::clicked, this, &MainWindow::handleWifiConnect);
    connect(wifiDisconnectButton, &QPushButton::clicked, this, [this]() {
        const int index = wifiNetworkCombo->currentIndex();
        if (index < 0 || index >= wifiNetworks.size())
            return;
        QString error;
        if (wifiManager.disconnect(wifiNetworks.at(index).interfaceIndex, &error))
            logWindow->appendLog("INFO", QString("Disconnected from Wi-Fi network '%1'.").arg(wifiNetworks.at(index).ssid));
        else
            logWindow->appendLog("ERROR", error);
        QTimer::singleShot(1000, this, &MainWindow::handleWifiScan);
    });
    handleWifiScan();
}

void MainWindow::updateArtworkBackground()
{
    if (artworkPixmap.isNull())
        artworkPixmap.load(QStringLiteral(":/themes/warpedskull-artwork.jpg"));
    if (artworkPixmap.isNull() || size().isEmpty())
        return;

    QPixmap background(size());
    background.fill(QColor("#0a1420"));
    const QPixmap scaled = artworkPixmap.scaled(
        background.size(), Qt::KeepAspectRatio, Qt::FastTransformation);
    QPainter painter(&background);
    painter.drawPixmap(
        (background.width() - scaled.width()) / 2,
        (background.height() - scaled.height()) / 2,
        scaled);

    QPalette windowPalette = palette();
    windowPalette.setBrush(QPalette::Window, QBrush(background));
    setPalette(windowPalette);
}

void MainWindow::populateWifiNetworks()
{
    const QString selectedKey = wifiNetworkCombo->currentData().toString();
    wifiNetworks = wifiManager.scan();
    wifiNetworkCombo->blockSignals(true);
    wifiNetworkCombo->clear();
    for (const WifiNetwork &network : wifiNetworks) {
        QString label = network.ssid;
        label += QStringLiteral(" [%1]").arg(network.band);
        if (network.ethernet) label += " [Ethernet]";
        else if (network.secured) label += " [Secured]";
        if (network.saved && !network.ethernet) label += " [Saved]";
        if (network.connected) label += " [Connected]";
        wifiNetworkCombo->addItem(label);
        wifiNetworkCombo->setItemData(wifiNetworkCombo->count() - 1,
            QStringLiteral("%1|%2").arg(network.networkInterfaceIndex).arg(network.ssid));
    }
    int restoredIndex = wifiNetworkCombo->findData(selectedKey);
    if (restoredIndex >= 0)
        wifiNetworkCombo->setCurrentIndex(restoredIndex);
    wifiNetworkCombo->blockSignals(false);
    const int selectedIndex = wifiNetworkCombo->currentIndex();
    const bool wifiConnected = selectedIndex >= 0 && selectedIndex < wifiNetworks.size() &&
        wifiNetworks.at(selectedIndex).connected;
    const bool ethernet = selectedIndex >= 0 && selectedIndex < wifiNetworks.size() &&
        wifiNetworks.at(selectedIndex).ethernet;
    wifiConnectButton->setEnabled(!wifiConnected && !ethernet && !wifiNetworks.isEmpty());
    wifiDisconnectButton->setEnabled(wifiConnected && !ethernet);
    if (selectedIndex >= 0 && selectedIndex < wifiNetworks.size())
        nsdDiscoveryManager->setInterfaceIndex(wifiNetworks.at(selectedIndex).networkInterfaceIndex);
    if (wifiNetworks.isEmpty())
        wifiNetworkCombo->addItem("No Wi-Fi networks found");
}

void MainWindow::handleWifiScan()
{
    wifiScanButton->setEnabled(false);
    populateWifiNetworks();
    wifiScanButton->setEnabled(true);
    logWindow->appendLog("INFO", QString("Wi-Fi scan complete: %1 network(s) found.").arg(wifiNetworks.size()));
}

void MainWindow::handleWifiConnect()
{
    const int index = wifiNetworkCombo->currentIndex();
    if (index < 0 || index >= wifiNetworks.size()) {
        logWindow->appendLog("ERROR", "Select a Wi-Fi network before connecting.");
        return;
    }

    const WifiNetwork network = wifiNetworks.at(index);
    QString password;
    if (network.secured && !network.saved) {
        bool accepted = false;
        password = QInputDialog::getText(this, "Wi-Fi Password",
            QString("Enter the password for %1:").arg(network.ssid),
            QLineEdit::Password, QString(), &accepted);
        if (!accepted)
            return;
    }

    QString error;
    wifiConnectButton->setEnabled(false);
    if (wifiManager.connect(network, password, &error)) {
        logWindow->appendLog("INFO", QString("Connecting to Wi-Fi network '%1'.").arg(network.ssid));
        QTimer::singleShot(3000, this, &MainWindow::handleWifiScan);
    } else {
        logWindow->appendLog("ERROR", error);
    }
    wifiConnectButton->setEnabled(true);
}

QWidget* MainWindow::createNetworkTab() {
    QWidget *tab = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(tab);

    QGroupBox *usbGroupBox = new QGroupBox("Remote USB Devices (USB/IP)", tab);
    QVBoxLayout *usbLayout = new QVBoxLayout(usbGroupBox);

    usbDeviceTable = new QTableWidget(0, 7, this);
    usbDeviceTable->setHorizontalHeaderLabels({"Fav", "Device Name", "VID:PID", "Reported Speed", "Status", "Attach Action", "Reset Action"});
    usbDeviceTable->horizontalHeaderItem(0)->setToolTip("Attach device automatically when connected to host.");
    usbDeviceTable->horizontalHeaderItem(3)->setToolTip("Host-reported USB operational speed for this device.");
    usbDeviceTable->horizontalHeaderItem(5)->setToolTip("Mount or unmount this USB device to the Windows kernel.");
    usbDeviceTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    usbDeviceTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    usbDeviceTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);

    usbLayout->addWidget(usbDeviceTable);

    QLabel *transportNotice = new QLabel("USB/IP uses the stable TCP transport for device connections.", tab);
    transportNotice->setWordWrap(true);
    transportNotice->setStyleSheet("color: #8dd3ff; margin-top: 6px;");
    usbLayout->addWidget(transportNotice);

    layout->addWidget(usbGroupBox);

    return tab;
}

void MainWindow::addUsbDeviceToTable(const QString &name, const QString &busId, const QString &vidPid, const QString &status, bool attached, USB_DEVICE_SPEED detectedSpeed) {
    int row = usbDeviceTable->rowCount();
    usbDeviceTable->insertRow(row);

    QWidget *favWidget = new QWidget(this);
    QHBoxLayout *favLayout = new QHBoxLayout(favWidget);
    favLayout->setAlignment(Qt::AlignCenter);
    favLayout->setContentsMargins(0, 0, 0, 0);
    QCheckBox *favCheck = new QCheckBox(favWidget);
    favCheck->setChecked(getFavorites().contains(vidPid));
    connect(favCheck, &QCheckBox::toggled, [this, vidPid](bool checked) { setFavorite(vidPid, checked); });
    favLayout->addWidget(favCheck);
    favWidget->setToolTip("Attach device automatically when connected to host.");
    usbDeviceTable->setCellWidget(row, 0, favWidget);

    QTableWidgetItem *nameItem = new QTableWidgetItem(name);
    nameItem->setData(Qt::UserRole, busId);
    usbDeviceTable->setItem(row, 1, nameItem);

    usbDeviceTable->setItem(row, 2, new QTableWidgetItem(vidPid));

    const QString speedText = usbSpeedLabel(detectedSpeed);
    QTableWidgetItem *speedItem = new QTableWidgetItem(speedText);
    speedItem->setFlags(speedItem->flags() & ~Qt::ItemIsEditable);
    usbDeviceTable->setItem(row, 3, speedItem);

    usbDeviceTable->setItem(row, 4, new QTableWidgetItem(status));

    QPushButton *attachBtn = new QPushButton(attached ? "Detach" : "Attach", this);
    attachBtn->setToolTip("Mount or unmount this USB device to the Windows kernel.");
    QPushButton *resetBtn = new QPushButton("Reset Connection", this);
    resetBtn->setToolTip("Force reset the connection state for this device.");

    connect(attachBtn, &QPushButton::clicked, [this, row]() { handleToggleDeviceAttach(row); });
    connect(resetBtn, &QPushButton::clicked, [this, row]() { handleResetDeviceConnection(row); });

    usbDeviceTable->setCellWidget(row, 5, attachBtn);
    usbDeviceTable->setCellWidget(row, 6, resetBtn);

}

QWidget* MainWindow::createSettingsTab() {
    QWidget *tab = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(tab);

    QGroupBox *settingsGroup = new QGroupBox("Application Preferences", tab);
    QFormLayout *formLayout = new QFormLayout(settingsGroup);

    themeCombo = new QComboBox(this);
    themeCombo->addItems({"Dark", "Light", "High Contrast", "Warpedskull"});

    minimizeToTrayCheckBox = new QCheckBox("Minimize to system tray on close", this);
    autoConnectCheckBox = new QCheckBox("Auto-connect to previously paired host on startup", this);

    profileCombo = new QComboBox(this);
    QPushButton *newProfileBtn = new QPushButton("New Profile", this);

    QHBoxLayout *profileLayout = new QHBoxLayout();
    profileLayout->addWidget(profileCombo, 1);
    profileLayout->addWidget(newProfileBtn);

    formLayout->addRow("UI Theme:", themeCombo);
    formLayout->addRow("Tray Behavior:", minimizeToTrayCheckBox);
    formLayout->addRow("Auto Connection:", autoConnectCheckBox);
    formLayout->addRow("Active Profile:", profileLayout);

    layout->addWidget(settingsGroup);
    layout->addStretch();

    connect(themeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::handleThemeChange);
    connect(profileCombo, &QComboBox::currentTextChanged, this, &MainWindow::handleProfileChange);
    connect(newProfileBtn, &QPushButton::clicked, this, &MainWindow::handleNewProfile);

    return tab;
}

void MainWindow::handleConnect() {
    QString ip = hostIpLineEdit->text().trimmed();
    bool ok = false;
    quint16 port = portLineEdit->text().toUShort(&ok);

    if (!ok || !validatePort(port)) {
        return;
    }

    if (isLogicallyConnected) {
        connectionMonitorTimer->stop();
        connectionProbeTimeoutTimer->stop();
        connectionProbeInProgress = false;
        connectionMonitorSocket->abort();
        telemetryByBusId.clear();
        previousTelemetrySamples.clear();
        isLogicallyConnected = false;
        connectButton->setText("Connect");
        connectionStatusLabel->setText("Status: Disconnected");
        connectionStatusLabel->setStyleSheet("color: #ff3366; font-weight: bold;");
        logWindow->appendLog("INFO", "Disconnected from host by user request.");
        return;
    }

    logWindow->appendLog("INFO", QString("Checking USB/IP host %1:%2...").arg(ip).arg(port));
    QString probeError;
    if (!probeHost(ip, port, &probeError)) {
        logWindow->appendLog(
            "ERROR",
            QString("Cannot reach USB/IP host %1:%2: %3")
                .arg(ip)
                .arg(port)
                .arg(probeError));
        return;
    }

    usbip::Socket usbipSocket = usbip::connect(
        ip.toStdString().c_str(), QString::number(port).toStdString().c_str());
    const DWORD connectError = GetLastError();
    if (!usbipSocket) {
        const QString detail = connectError == WSAECONNREFUSED
            ? "The host is reachable, but no USB/IP service is listening on this port."
            : QString("Winsock error %1.").arg(connectError);
        logWindow->appendLog(
            "ERROR",
            QString("Cannot connect to USB/IP host %1:%2. %3")
                .arg(ip)
                .arg(port)
                .arg(detail));
        return;
    }

    isLogicallyConnected = true;
    connectButton->setText("Disconnect");
    connectionStatusLabel->setText("<font color='green'>Status: Connected</font>");
    connectionStatusLabel->setStyleSheet("font-weight: bold;");
    logWindow->appendLog("INFO", QString("Connected to %1:%2.").arg(ip).arg(port));
    connectionMonitorTimer->start();
    saveSettings();
}

void MainWindow::checkHostConnection()
{
    if (!isLogicallyConnected || connectionProbeInProgress)
        return;

    bool ok = false;
    const quint16 port = portLineEdit->text().trimmed().toUShort(&ok);
    if (!ok || !validatePort(port)) {
        markHostDisconnected("Connection monitor found an invalid host endpoint.");
        return;
    }

    connectionProbeInProgress = true;
    connectionMonitorSocket->abort();
    connectionMonitorSocket->connectToHost(hostIpLineEdit->text().trimmed(), port);
    connectionProbeTimeoutTimer->start();
}

void MainWindow::handleHostConnectionEstablished()
{
    if (!connectionProbeInProgress)
        return;

    connectionProbeTimeoutTimer->stop();
    connectionProbeInProgress = false;
    connectionMonitorSocket->abort();
}

void MainWindow::handleHostConnectionError(QAbstractSocket::SocketError error)
{
    Q_UNUSED(error);
    if (!connectionProbeInProgress)
        return;

    connectionProbeTimeoutTimer->stop();
    connectionProbeInProgress = false;
    markHostDisconnected(QString("Connection monitor error: %1.").arg(connectionMonitorSocket->errorString()));
}

void MainWindow::markHostDisconnected(const QString &reason)
{
    if (!isLogicallyConnected)
        return;

    connectionMonitorTimer->stop();
    isLogicallyConnected = false;
    connectButton->setText("Connect");
    connectionStatusLabel->setText("Status: Disconnected");
    connectionStatusLabel->setStyleSheet("color: #ff3366; font-weight: bold;");
    logWindow->appendLog("WARNING", QString("Host connection lost. %1").arg(reason));
    syncDeviceStates();
}

bool MainWindow::probeHost(const QString &ip, quint16 port, QString *error)
{
    QTcpSocket socket;
    socket.connectToHost(ip, port);
    if (!socket.waitForConnected(3000)) {
        if (error) {
            *error = socket.errorString();
        }
        socket.abort();
        return false;
    }
    socket.disconnectFromHost();
    return true;
}

void MainWindow::handleScanHost() {
    if (!isLogicallyConnected) {
        logWindow->appendLog("ERROR", "Cannot scan: not logically connected to a host. Click 'Connect' first.");
        return;
    }

    QString ip = hostIpLineEdit->text().trimmed();
    bool ok = false;
    quint16 port = portLineEdit->text().toUShort(&ok);
    if (!ok || !validatePort(port)) {
        return;
    }

    QString probeError;
    if (!probeHost(ip, port, &probeError)) {
        logWindow->appendLog(
            "ERROR",
            QString("Scan aborted: %1:%2 is unreachable: %3")
                .arg(ip)
                .arg(port)
                .arg(probeError));
        return;
    }

    logWindow->appendLog("INFO", "Scanning exportable devices via usbip_sdk...");

    usbip::Socket sock = usbip::connect(ip.toStdString().c_str(), QString::number(port).toStdString().c_str());
    const DWORD connectError = GetLastError();
    if (!sock) {
        const QString detail = connectError == WSAECONNREFUSED
            ? "No USB/IP service is listening on the selected port."
            : QString("Winsock error %1.").arg(connectError);
        logWindow->appendLog(
            "ERROR",
            QString("Scan connect to %1:%2 failed. %3")
                .arg(ip)
                .arg(port)
                .arg(detail));
        return;
    }

    QList<usbip::usb_device> devices;
    remoteClassByBusId.clear();
    bool enumOk = false;
    try {
        enumOk = usbip::enum_exportable_devices(
            sock.get(),
            [this, &devices](int, const usbip::usb_device &dev) {
                devices.append(dev);
                if (usbIdsDb) {
                    auto [className, subclassName, protocolName] =
                        usbIdsDb->find_class_subclass_proto(
                            dev.bDeviceClass, dev.bDeviceSubClass, dev.bDeviceProtocol);
                    Q_UNUSED(subclassName);
                    Q_UNUSED(protocolName);
                    remoteClassByBusId.insert(
                        QString::fromStdString(dev.busid),
                        QString::fromUtf8(className.data(), static_cast<int>(className.size())));
                }
            },
            [](int, const usbip::usb_device &, int, const usbip::usb_interface &) {},
            nullptr);
    } catch (const std::exception &ex) {
        logWindow->appendLog("ERROR", QString("enum_exportable_devices threw an exception: %1").arg(ex.what()));
        return;
    }

    if (!enumOk) {
        logWindow->appendLog("ERROR", QString("enum_exportable_devices failed (error %1).").arg(GetLastError()));
        return;
    }

    usbDeviceTable->setRowCount(0);

    if (devices.isEmpty()) {
        logWindow->appendLog("INFO", "Scan complete: no exportable devices found.");
        return;
    }

    for (const usbip::usb_device &dev : devices) {
        QString busid  = QString::fromStdString(dev.busid);
        QString vidPid = QString("%1:%2")
                            .arg(dev.idVendor,  4, 16, QChar('0'))
                            .arg(dev.idProduct, 4, 16, QChar('0'))
                            .toUpper();
        QString name   = getFriendlyDeviceName(dev.idVendor, dev.idProduct);
        int hubPort    = findAttachedPort(busid);
        bool attached  = hubPort >= 1;
        QString status = attached ? "Attached" : "Available";

        addUsbDeviceToTable(name, busid, vidPid, status, attached, dev.speed);
        logWindow->appendLog("INFO", QString("Found: %1  [%2]  %3").arg(busid, vidPid, name));
    }

    logWindow->appendLog("INFO", QString("Scan complete: %1 device(s) found.").arg(devices.size()));

    if (recoveryInProgress)
        return;

    for (int r = 0; r < usbDeviceTable->rowCount(); ++r) {
        QTableWidgetItem *vidPidItem = usbDeviceTable->item(r, 2);
        if (!vidPidItem) continue;
        QString vidPid = vidPidItem->text();
        if (!getFavorites().contains(vidPid)) continue;
        QPushButton *attachBtn = qobject_cast<QPushButton*>(usbDeviceTable->cellWidget(r, 5));
        if (attachBtn && attachBtn->text() == "Attach") {
            logWindow->appendLog("INFO", QString("Auto-attaching favorite device: %1").arg(vidPid));
            handleToggleDeviceAttach(r);
        }
    }
}

void MainWindow::handleToggleLogWindow() {
    if (logWindow->isVisible()) {
        logWindow->hide();
    } else {
        logWindow->show();
        logWindow->raise();
    }
}

void MainWindow::handleToggleDeviceAttach(int row) {
    if (row < 0 || row >= usbDeviceTable->rowCount()) return;

    QString busid = usbDeviceTable->item(row, 1)->data(Qt::UserRole).toString();
    QPushButton *btn = qobject_cast<QPushButton*>(usbDeviceTable->cellWidget(row, 5));
    if (!btn) return;

    btn->setEnabled(false);

    try {
        if (btn->text() == "Detach") {
            int hubPort = findAttachedPort(busid);
            if (hubPort < 1) {
                logWindow->appendLog("WARNING", QString("Device %1 is not recorded as attached; skipping detach.").arg(busid));
                btn->setEnabled(true);
                return;
            }

            usbip::Handle dev = usbip::vhci::open();
            if (!dev) {
                logWindow->appendLog("ERROR", QString("vhci::open() failed (error %1).").arg(GetLastError()));
                btn->setEnabled(true);
                return;
            }

            if (!usbip::vhci::detach(dev.get(), hubPort)) {
                DWORD err = GetLastError();
                if (err == ERROR_DEVICE_NOT_CONNECTED) {
                    logWindow->appendLog("WARNING", QString("Device on bus %1 was already disconnected on the host/OS level (error 1167). Force-syncing local state.").arg(busid));
                    attachedPorts.remove(busid);
                    if (row < usbDeviceTable->rowCount()) {
                        usbDeviceTable->item(row, 4)->setText("Available");
                        QPushButton *attachBtn = qobject_cast<QPushButton*>(usbDeviceTable->cellWidget(row, 5));
                        if (attachBtn) attachBtn->setText("Attach");
                    }
                } else {
                    logWindow->appendLog("ERROR", QString("vhci::detach() failed for bus %1 (error %2).").arg(busid).arg(err));
                }
                btn->setEnabled(true);
                return;
            }

            attachedPorts.remove(busid);
            if (row < usbDeviceTable->rowCount()) {
                usbDeviceTable->item(row, 4)->setText("Available");
                btn->setText("Attach");
            }
            logWindow->appendLog("INFO", QString("Detached bus %1 (port %2).").arg(busid).arg(hubPort));
            btn->setEnabled(true);
            return;
        }

        // Attach path
        QString ip   = hostIpLineEdit->text().trimmed();
        QString port = portLineEdit->text().trimmed();
        bool portOk = false;
        const quint16 portNumber = port.toUShort(&portOk);
        if (!portOk || !validatePort(portNumber) || !probeHost(ip, portNumber)) {
            logWindow->appendLog("ERROR", QString("Attach aborted: USB/IP host %1:%2 is unreachable. Verify the host IP and firewall.").arg(ip, port));
            btn->setEnabled(true);
            return;
        }

        usbip::Handle dev = usbip::vhci::open();
        if (!dev) {
            logWindow->appendLog("ERROR", QString("vhci::open() failed (error %1).").arg(GetLastError()));
            btn->setEnabled(true);
            return;
        }

        usbip::device_location location;
        location.hostname = ip.toStdString();
        location.service  = port.toStdString();
        location.busid    = busid.toStdString();

        logWindow->appendLog("INFO", QString("Attaching bus %1 from %2:%3 using the host-reported speed...").arg(busid, ip, port));

        std::unique_ptr<usbip::transport::IUsbTransport> transport = std::make_unique<usbip::transport::TcpTransport>();

        QString vidPid;
        if (row < usbDeviceTable->rowCount()) {
            QTableWidgetItem *vpItem = usbDeviceTable->item(row, 2);
            if (vpItem) vidPid = vpItem->text();
        }

        // Clear any stale VHCI port holding this busid to prevent Error 995
        if (auto devicesOpt = usbip::vhci::get_imported_devices(dev.get())) {
            for (const auto &imported : *devicesOpt) {
                if (QString::fromStdString(imported.location.busid) == busid) {
                    logWindow->appendLog("INFO", QString("Clearing stale port %1 for bus %2 before re-attach.").arg(imported.port).arg(busid));
                    usbip::vhci::detach(dev.get(), imported.port);
                    QThread::msleep(50); // Allow kernel VHCI driver time to finish async port release
                }
            }
        }

        int hubPort = transport->connect(dev.get(), location);
        if (hubPort < 1) {
            DWORD attachErr = GetLastError();
            logWindow->appendLog("WARNING", QString("vhci::attach() failed for bus %1 (error %2). Attempting dynamic recovery...").arg(busid).arg(attachErr));

            if (!vidPid.isEmpty()) {
                recoveryInProgress = true;
                clearDeviceTable();
                handleScanHost();
                recoveryInProgress = false;
                QString newBusId = getFreshBusId(vidPid);
                if (!newBusId.isEmpty()) {
                    logWindow->appendLog("INFO", QString("[Recovery] Device found on new Bus ID: %1. Re-attaching...").arg(newBusId));
                    usbip::device_location recoveryLocation;
                    recoveryLocation.hostname = ip.toStdString();
                    recoveryLocation.service  = port.toStdString();
                    recoveryLocation.busid    = newBusId.toStdString();
                    hubPort = transport->connect(dev.get(), recoveryLocation);
                    if (hubPort >= 1) {
                        attachedPorts[newBusId] = hubPort;
                        logWindow->appendLog("INFO", QString("[Recovery] Successfully attached device on new Bus ID %1 (hub port %2).").arg(newBusId).arg(hubPort));
                        int newRow = -1;
                        for (int r = 0; r < usbDeviceTable->rowCount(); ++r) {
                            QTableWidgetItem *ni = usbDeviceTable->item(r, 1);
                            if (ni && ni->data(Qt::UserRole).toString() == newBusId) { newRow = r; break; }
                        }
                        if (newRow >= 0) {
                            usbDeviceTable->item(newRow, 4)->setText("Attached");
                            if (auto *rb = qobject_cast<QPushButton*>(usbDeviceTable->cellWidget(newRow, 5)))
                                rb->setText("Detach");
                            if (auto *pc = qobject_cast<QComboBox*>(usbDeviceTable->cellWidget(newRow, 7)))
                                pc->setEnabled(false);
                        }
                    } else {
                        logWindow->appendLog("ERROR", QString("[Recovery] Re-attach failed (error %1).").arg(GetLastError()));
                    }
                } else {
                    logWindow->appendLog("ERROR", "[Recovery] Device not found in fresh scan. Hardware may be physically disconnected.");
                }
            } else {
                logWindow->appendLog("ERROR", QString("vhci::attach() failed for bus %1 (error %2).").arg(busid).arg(attachErr));
            }
            const QString displayedBusId = getFreshBusId(vidPid).isEmpty() ? busid : getFreshBusId(vidPid);
            for (int currentRow = 0; currentRow < usbDeviceTable->rowCount(); ++currentRow) {
                QTableWidgetItem *currentNameItem = usbDeviceTable->item(currentRow, 1);
                if (!currentNameItem || currentNameItem->data(Qt::UserRole).toString() != displayedBusId)
                    continue;
                if (auto *currentButton = qobject_cast<QPushButton*>(usbDeviceTable->cellWidget(currentRow, 5)))
                    currentButton->setEnabled(true);
                break;
            }
            return;
        }

        attachedPorts[busid] = hubPort;
        if (row < usbDeviceTable->rowCount()) {
            usbDeviceTable->item(row, 4)->setText("Attached");
            btn->setText("Detach");
        }
        logWindow->appendLog("INFO", QString("Attached bus %1 on hub port %2.").arg(busid).arg(hubPort));
    } catch (const std::exception &ex) {
        logWindow->appendLog("ERROR", QString("SDK exception in attach/detach: %1").arg(ex.what()));
    }

    btn->setEnabled(true);
}

void MainWindow::handleHostDiscovered(const QString &hostname, const QHostAddress &address,
                                      quint16 port, quint16 discoveredTelemetryPort, int interfaceIndex)
{
    const QString ip = address.toString();
    const QString label = QStringLiteral("%1 (%2:%3)").arg(hostname, ip).arg(port);
    const QString hostKey = QStringLiteral("%1|%2").arg(hostname).arg(interfaceIndex);
    for (int i = 1; i < discoveredHostCombo->count(); ++i) {
        if (discoveredHostCombo->itemData(i, Qt::UserRole + 1).toString() != hostKey)
            continue;

        const QString oldEndpoint = discoveredHostCombo->itemData(i).toString();
        const QString newEndpoint = QStringLiteral("%1|%2|%3").arg(ip).arg(port).arg(discoveredTelemetryPort);
        if (oldEndpoint == newEndpoint)
            return;

        discoveredHostCombo->setItemText(i, label);
        discoveredHostCombo->setItemData(i, newEndpoint, Qt::UserRole);
        logWindow->appendLog("INFO", QStringLiteral("mDNS: host %1 changed endpoint from %2 to %3:%4")
            .arg(hostname, oldEndpoint.section('|', 0, 0), ip).arg(port));

        if (discoveredHostCombo->currentIndex() == i) {
            hostIpLineEdit->setText(ip);
            portLineEdit->setText(QString::number(port));
            telemetryPort = discoveredTelemetryPort;
            previousTelemetrySamples.clear();
            telemetryByBusId.clear();
            logWindow->appendLog("INFO", QStringLiteral("Updated active host endpoint to %1:%2 from mDNS.").arg(ip).arg(port));
        }
        return;
    }
    const int index = discoveredHostCombo->count();
    discoveredHostCombo->addItem(label, QStringLiteral("%1|%2|%3").arg(ip).arg(port).arg(discoveredTelemetryPort));
    discoveredHostCombo->setItemData(index, hostKey, Qt::UserRole + 1);
    logWindow->appendLog("INFO", QStringLiteral("mDNS: discovered host %1 at %2:%3 (telemetry %4)")
        .arg(hostname, ip).arg(port).arg(discoveredTelemetryPort));
}

void MainWindow::handleResetDeviceConnection(int row) {
    if (row < 0 || row >= usbDeviceTable->rowCount()) return;

    QString busid = usbDeviceTable->item(row, 1)->data(Qt::UserRole).toString();

    int hubPort = findAttachedPort(busid);
    if (hubPort < 1) {
        logWindow->appendLog("WARNING", QString("Device on bus %1 is not attached; cannot reset.").arg(busid));
        return;
    }

    usbip::Handle dev = usbip::vhci::open();
    if (!dev) {
        logWindow->appendLog("ERROR", QString("vhci::open() failed (error %1).").arg(GetLastError()));
        return;
    }

    if (!usbip::vhci::detach(dev.get(), hubPort)) {
        DWORD err = GetLastError();
        if (err == ERROR_DEVICE_NOT_CONNECTED) {
            logWindow->appendLog("WARNING", QString("Device on bus %1 was already disconnected on the host/OS level (error 1167). Force-syncing local state.").arg(busid));
            attachedPorts.remove(busid);
            if (row < usbDeviceTable->rowCount()) {
                usbDeviceTable->item(row, 4)->setText("Available");
                QPushButton *attachBtn = qobject_cast<QPushButton*>(usbDeviceTable->cellWidget(row, 5));
                if (attachBtn) attachBtn->setText("Attach");
            }
        } else {
            logWindow->appendLog("ERROR", QString("vhci::detach() failed for bus %1 (error %2).").arg(busid).arg(err));
            return;
        }
    } else {
        attachedPorts.remove(busid);
    }
    logWindow->appendLog("INFO", QString("Resetting device on bus %1 (detaching and re-attaching)...").arg(busid));

    QString ip   = hostIpLineEdit->text().trimmed();
    QString port = portLineEdit->text().trimmed();

    usbip::device_location location;
    location.hostname = ip.toStdString();
    location.service  = port.toStdString();
    location.busid    = busid.toStdString();

    int newHubPort = usbip::vhci::attach(dev.get(), location);
    if (newHubPort < 1) {
        logWindow->appendLog("ERROR", QString("Re-attach failed for bus %1 (error %2).").arg(busid).arg(GetLastError()));
        if (row < usbDeviceTable->rowCount()) {
            usbDeviceTable->item(row, 4)->setText("Available");
        }
        return;
    }

    attachedPorts[busid] = newHubPort;
    if (row < usbDeviceTable->rowCount()) {
        usbDeviceTable->item(row, 4)->setText("Attached");
        if (auto *attachBtn = qobject_cast<QPushButton*>(usbDeviceTable->cellWidget(row, 5)))
            attachBtn->setText("Detach");
    }
    logWindow->appendLog("INFO", QString("Reset complete: bus %1 re-attached on hub port %2.").arg(busid).arg(newHubPort));
}

void MainWindow::handleThemeChange(int index) {
    if (index < 0 || index >= themeCombo->count()) {
        return;
    }

    QString theme = themeCombo->itemText(index);
    applyTheme(theme);
    saveProfileSettings(currentProfile);
    logWindow->appendLog("INFO", QString("UI Theme changed to %1").arg(theme));
}

void MainWindow::handleNewProfile() {
    bool ok = false;
    QString name = QInputDialog::getText(this, "New Profile", "Enter profile name:", QLineEdit::Normal, "", &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    
    name = name.trimmed();
    if (profileCombo->findText(name) >= 0) {
        QMessageBox::warning(this, "Profile Exists", "A profile with this name already exists.");
        return;
    }
    
    profileCombo->addItem(name);
    profileCombo->setCurrentText(name);
}

void MainWindow::handleProfileChange(const QString &profileName) {
    if (profileName.isEmpty() || profileName == currentProfile) return;
    
    saveProfileSettings(currentProfile);
    currentProfile = profileName;
    loadProfileSettings(currentProfile);
    
    QSettings settings("USBIPClient", "USBIPClient");
    settings.setValue("profiles/active", currentProfile);
    
    logWindow->appendLog("INFO", QString("Switched to profile: %1").arg(profileName));
}

void MainWindow::applyTheme(const QString &themeName) {
    artistCredit->setVisible(themeName == "Warpedskull");

    if (themeName == "Warpedskull") {
        setStyleSheet(
            "QMainWindow { background-color: transparent; color: #e8edf0; }"
            "QDialog { background-color: #0a1420; color: #e8edf0; }"
            "QMenuBar { background-color: rgba(7, 16, 27, 80); color: #e8edf0; }"
            "QWidget#centralWidget { background-color: rgba(7, 16, 27, 55); }"
            "QDockWidget { background-color: rgba(7, 16, 27, 55); color: #e8edf0; }"
            "QDockWidget::title { background-color: rgba(16, 40, 58, 125); color: #f0b27b; padding: 5px; }"
            "QDockWidget::widget { background-color: rgba(7, 16, 27, 55); }"
            "QTabWidget::pane { border: 1px solid #385269; background-color: rgba(9, 22, 35, 90); }"
            "QTabBar::tab { background-color: rgba(10, 24, 38, 105); color: #d4e2e7; padding: 8px 16px; border: 1px solid #385269; }"
            "QTabBar::tab:selected { background-color: #17364b; color: #f0b27b; border-bottom: 2px solid #df7b45; }"
            "QGroupBox { border: 1px solid #385269; margin-top: 10px; font-weight: bold; color: #f0b27b; background-color: rgba(8, 20, 32, 75); }"
            "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 5px; }"
            "QPushButton { background-color: rgba(23, 54, 75, 175); color: #edf4f5; border: 1px solid #52758a; padding: 6px 12px; border-radius: 4px; }"
            "QPushButton:hover { background-color: #28546b; border-color: #f0b27b; }"
            "QPushButton:pressed { background-color: #0f2638; }"
            "QLineEdit, QComboBox, QSpinBox { background-color: rgba(5, 15, 25, 175); border: 1px solid #385269; color: #f5f7f7; padding: 4px; border-radius: 4px; }"
            "QComboBox QAbstractItemView { background-color: #0b1b2a; color: #f5f7f7; border: 1px solid #52758a; selection-background-color: #df7b45; selection-color: #08131d; }"
            "QTableWidget { background-color: rgba(5, 15, 25, 105); gridline-color: #294356; color: #edf4f5; }"
            "QHeaderView::section { background-color: rgba(16, 40, 58, 145); color: #f0b27b; border: 1px solid #385269; padding: 4px; }"
            "QLabel, QCheckBox { color: #e8edf0; }"
            "QLabel#artistCredit { color: #f0b27b; font-style: italic; font-weight: bold; padding: 0 8px; }"
        );
        updateArtworkBackground();
    } else if (themeName == "Dark") {
        setStyleSheet(
            "QMainWindow, QDialog { background-color: #12141d; color: #e0e6ed; }"
            "QTabWidget::pane { border: 1px solid #23273a; background: #1a1d2e; }"
            "QTabBar::tab { background: #12141d; color: #8a99ad; padding: 8px 16px; border: 1px solid #23273a; }"
            "QTabBar::tab:selected { background: #1a1d2e; color: #00f2fe; border-bottom: 2px solid #00f2fe; }"
            "QGroupBox { border: 1px solid #23273a; margin-top: 10px; font-weight: bold; color: #00f2fe; }"
            "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 5px; }"
            "QPushButton { background-color: #23273a; color: #ffffff; border: 1px solid #343b54; padding: 6px 12px; border-radius: 4px; }"
            "QPushButton:hover { background-color: #343b54; border-color: #00f2fe; }"
            "QLineEdit, QComboBox, QSpinBox { background-color: #0d0e15; border: 1px solid #23273a; color: #ffffff; padding: 4px; border-radius: 4px; }"
            "QComboBox QAbstractItemView { background-color: #0d0e15; color: #ffffff; border: 1px solid #23273a; selection-background-color: #00f2fe; selection-color: #12141d; }"
            "QTableWidget { background-color: #0d0e15; gridline-color: #23273a; color: #ffffff; }"
            "QHeaderView::section { background-color: #12141d; color: #00f2fe; border: 1px solid #23273a; padding: 4px; }"
            "QLabel, QCheckBox { color: #e0e6ed; }"
        );
    } else if (themeName == "Light") {
        setStyleSheet(
            "QMainWindow, QDialog { background-color: #f1f5f9; color: #0f172a; }"
            "QTabWidget::pane { border: 1px solid #cbd5e1; background: #ffffff; }"
            "QTabBar::tab { background: #e2e8f0; color: #64748b; padding: 8px 16px; border: 1px solid #cbd5e1; }"
            "QTabBar::tab:selected { background: #ffffff; color: #2563eb; border-bottom: 2px solid #2563eb; }"
            "QGroupBox { border: 1px solid #cbd5e1; margin-top: 10px; font-weight: bold; color: #2563eb; }"
            "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 5px; }"
            "QPushButton { background-color: #e2e8f0; color: #0f172a; border: 1px solid #cbd5e1; padding: 6px 12px; border-radius: 4px; }"
            "QPushButton:hover { background-color: #cbd5e1; border-color: #2563eb; }"
            "QLineEdit, QComboBox, QSpinBox { background-color: #ffffff; border: 1px solid #cbd5e1; color: #0f172a; padding: 4px; border-radius: 4px; }"
            "QComboBox QAbstractItemView { background-color: #ffffff; color: #0f172a; border: 1px solid #cbd5e1; selection-background-color: #2563eb; selection-color: #ffffff; }"
            "QTableWidget { background-color: #ffffff; gridline-color: #cbd5e1; color: #0f172a; }"
            "QHeaderView::section { background-color: #f8fafc; color: #2563eb; border: 1px solid #cbd5e1; padding: 4px; }"
            "QLabel, QCheckBox { color: #0f172a; }"
        );
    } else if (themeName == "High Contrast") {
        setStyleSheet(
            "QMainWindow, QDialog { background-color: #000000; color: #ffffff; }"
            "QTabWidget::pane { border: 2px solid #ffffff; background: #000000; }"
            "QTabBar::tab { background: #000000; color: #ffffff; padding: 8px 16px; border: 2px solid #ffffff; }"
            "QTabBar::tab:selected { background: #000000; color: #ffff00; border-bottom: 2px solid #ffff00; }"
            "QGroupBox { border: 2px solid #ffffff; margin-top: 10px; font-weight: bold; color: #ffff00; }"
            "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 5px; }"
            "QPushButton { background-color: #000000; color: #ffffff; border: 2px solid #ffffff; padding: 6px 12px; border-radius: 4px; }"
            "QPushButton:hover { background-color: #ffffff; color: #000000; border-color: #ffff00; }"
            "QLineEdit, QComboBox, QSpinBox { background-color: #000000; border: 2px solid #ffffff; color: #ffffff; padding: 4px; border-radius: 4px; }"
            "QComboBox QAbstractItemView { background-color: #000000; color: #ffffff; border: 2px solid #ffffff; selection-background-color: #ffff00; selection-color: #000000; }"
            "QTableWidget { background-color: #000000; gridline-color: #ffffff; color: #ffffff; }"
            "QHeaderView::section { background-color: #000000; color: #ffff00; border: 2px solid #ffffff; padding: 4px; }"
            "QLabel, QCheckBox { color: #ffffff; }"
        );
    }
}

int MainWindow::findAttachedPort(const QString &busid) const {
    return attachedPorts.value(busid, -1);
}

QStringList MainWindow::getFavorites() const {
    QSettings settings("USBIPClient", "USBIPClient");
    return settings.value("favorites").toStringList();
}

void MainWindow::setFavorite(const QString &vidPid, bool favorite) {
    QSettings settings("USBIPClient", "USBIPClient");
    QStringList favs = settings.value("favorites").toStringList();
    if (favorite && !favs.contains(vidPid)) { favs.append(vidPid); }
    else if (!favorite) { favs.removeAll(vidPid); }
    settings.setValue("favorites", favs);
    logWindow->appendLog("INFO", QString("Device %1 %2 favorites.").arg(vidPid, favorite ? "added to" : "removed from"));
}

QString MainWindow::getFriendlyDeviceName(quint16 vendorId, quint16 productId) {
    if (usbIdsDb) {
        auto [vendorStr, productStr] = usbIdsDb->find_product(vendorId, productId);
        if (!vendorStr.empty() || !productStr.empty()) {
            QString mfg = vendorStr.empty() ? "Unknown Vendor" : QString::fromUtf8(vendorStr.data(), vendorStr.size());
            QString prod = productStr.empty() ? "Unknown Product" : QString::fromUtf8(productStr.data(), productStr.size());
            QString vidPid = QString("%1:%2").arg(vendorId, 4, 16, QChar('0')).arg(productId, 4, 16, QChar('0')).toUpper();
            return QString("%1 - %2 (%3)").arg(mfg, prod, vidPid);
        }
    }
    return "Unknown Device";
}

void MainWindow::loadUsbIdDatabase() {
    QString dbPath = QCoreApplication::applicationDirPath() + "/usb.ids";
    QFile file(dbPath);
    if (!file.open(QIODevice::ReadOnly)) {
        logWindow->appendLog("WARNING", "Failed to open usb.ids database at " + dbPath);
        return;
    }

    if (usbIdsDb) {
        delete usbIdsDb;
        usbIdsDb = nullptr;
    }

    usbIdsData = file.readAll();
    usbIdsDb = new usbip::UsbIds(std::string_view(usbIdsData.constData(), usbIdsData.size()));
    logWindow->appendLog("INFO", "Loaded usb.ids database via SDK.");
}

void MainWindow::syncDeviceStates() {
    usbip::Handle dev = usbip::vhci::open();
    if (!dev) return;

    auto devicesOpt = usbip::vhci::get_imported_devices(dev.get());
    if (!devicesOpt) return;

    const auto &importedDevices = *devicesOpt;
    QHash<QString, int> actualPorts;
    for (const auto &importedDev : importedDevices) {
        actualPorts.insert(QString::fromStdString(importedDev.location.busid), importedDev.port);
    }

    attachedPorts = actualPorts;

    for (int row = 0; row < usbDeviceTable->rowCount(); ++row) {
        QPushButton *actionBtn = qobject_cast<QPushButton*>(usbDeviceTable->cellWidget(row, 5));
        QTableWidgetItem *nameItem = usbDeviceTable->item(row, 1);
        if (!actionBtn || !nameItem)
            continue;
        const QString currentBusId = nameItem->data(Qt::UserRole).toString();
        const bool isActuallyAttached = actualPorts.contains(currentBusId);
        const bool wasAttached = actionBtn->text() == "Detach";
        if (wasAttached && !isActuallyAttached)
            logWindow->appendLog("INFO", QString("Background monitor detected drop for bus %1. Syncing UI.").arg(currentBusId));

        if (auto *statusItem = usbDeviceTable->item(row, 4))
            statusItem->setText(isActuallyAttached ? "Attached" : "Available");
        actionBtn->setText(isActuallyAttached ? "Detach" : "Attach");
    }
}

void MainWindow::refreshTelemetryStats() {
    syncDeviceStates();

    const QString endpoint = QStringLiteral("%1:%2")
        .arg(hostIpLineEdit->text().trimmed()).arg(telemetryPort);
    if (endpoint != telemetryEndpoint) {
        telemetryEndpoint = endpoint;
        previousTelemetrySamples.clear();
        telemetryByBusId.clear();
        telemetryTable->setRowCount(0); // Only clear everything if the host endpoint changes
    }

    usbip::Handle dev = usbip::vhci::open();
    if (!dev) {
        telemetryTable->setRowCount(0);
        return;
    }

    auto devicesOpt = usbip::vhci::get_imported_devices(dev.get());
    if (!devicesOpt) {
        telemetryTable->setRowCount(0);
        return;
    }

    QSet<QString> currentBusIds;

    for (const auto &importedDev : *devicesOpt) {
        QString busId = QString::fromStdString(importedDev.location.busid);
        currentBusIds.insert(busId);

        QString deviceName = getFriendlyDeviceName(
            static_cast<quint16>(importedDev.vendor),
            static_cast<quint16>(importedDev.product));

        QString speedStr = "Unknown";
        switch (importedDev.speed) {
            case UsbLowSpeed:  speedStr = "Low Speed (1.5 Mbps)";    break;
            case UsbFullSpeed: speedStr = "Full Speed (12 Mbps)";    break;
            case UsbHighSpeed: speedStr = "High Speed (480 Mbps)";   break;
            case UsbSuperSpeed: speedStr = "SuperSpeed (5 Gbps)";    break;
            default: break;
        }

        const TelemetryDisplay display = telemetryByBusId.value(busId);
        const QString throughputStr = display.throughput;
        const QString jitterStr = display.jitter;
        const QString devClass = remoteClassByBusId.value(busId, "Unknown");

        // 1. Look for an existing row for this device
        int foundRow = -1;
        for (int r = 0; r < telemetryTable->rowCount(); ++r) {
            if (telemetryTable->item(r, 0) && telemetryTable->item(r, 0)->text() == busId) {
                foundRow = r;
                break;
            }
        }

        // 2. Update existing or insert new
        if (foundRow >= 0) {
            telemetryTable->item(foundRow, 2)->setText(speedStr);
            telemetryTable->item(foundRow, 3)->setText(devClass);
            telemetryTable->item(foundRow, 4)->setText(display.latency);
            telemetryTable->item(foundRow, 5)->setText(jitterStr);
            telemetryTable->item(foundRow, 6)->setText(throughputStr);
        } else {
            int newRow = telemetryTable->rowCount();
            telemetryTable->insertRow(newRow);
            telemetryTable->setItem(newRow, 0, new QTableWidgetItem(busId));
            telemetryTable->setItem(newRow, 1, new QTableWidgetItem(deviceName));
            telemetryTable->setItem(newRow, 2, new QTableWidgetItem(speedStr));
            telemetryTable->setItem(newRow, 3, new QTableWidgetItem(devClass));
            telemetryTable->setItem(newRow, 4, new QTableWidgetItem(display.latency));
            telemetryTable->setItem(newRow, 5, new QTableWidgetItem(jitterStr));
            telemetryTable->setItem(newRow, 6, new QTableWidgetItem(throughputStr));
        }
    }

    // 3. Cleanup stale rows (devices that were detached)
    for (int r = telemetryTable->rowCount() - 1; r >= 0; --r) {
        if (telemetryTable->item(r, 0) && !currentBusIds.contains(telemetryTable->item(r, 0)->text())) {
            telemetryTable->removeRow(r);
        }
    }

    if (!isLogicallyConnected || telemetryReply)
        return;

    QNetworkRequest request{QUrl(QStringLiteral("http://%1/telemetry").arg(endpoint))};
    request.setHeader(QNetworkRequest::UserAgentHeader, "USBIPClient");
    telemetryReply = telemetryNetworkManager.get(request);
    connect(telemetryReply, &QNetworkReply::finished,
            this, &MainWindow::handleTelemetryReply);
}

void MainWindow::handleTelemetryReply() {
    QNetworkReply *reply = telemetryReply;
    telemetryReply = nullptr;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        telemetryByBusId.clear();
        previousTelemetrySamples.clear();
        return;
    }

    const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
    if (!document.isObject()) {
        telemetryByBusId.clear();
        previousTelemetrySamples.clear();
        return;
    }

    const QJsonArray devices = document.object().value("devices").toArray();
    if (!document.object().contains("devices")) {
        telemetryByBusId.clear();
        previousTelemetrySamples.clear();
        return;
    }

    if (!telemetryClock.isValid())
        telemetryClock.start();
    const qint64 nowMs = telemetryClock.elapsed();
    QHash<QString, TelemetryDisplay> nextDisplay;
    QHash<QString, TelemetrySample> nextSamples;
    for (const QJsonValue &deviceValue : devices) {
        const QJsonObject device = deviceValue.toObject();
        const QString busId = device.value("busid").toString();
        if (busId.isEmpty())
            continue;

        bool toClientOk = false;
        bool fromClientOk = false;
        const quint64 toClient = device.value("bytes_to_client").toVariant().toULongLong(&toClientOk);
        const quint64 fromClient = device.value("bytes_from_client").toVariant().toULongLong(&fromClientOk);
        const quint64 totalBytes = (toClientOk ? toClient : 0) + (fromClientOk ? fromClient : 0);
        TelemetryDisplay display;
        const TelemetrySample previous = previousTelemetrySamples.value(busId);
        if ((toClientOk || fromClientOk) && previous.timestampMs > 0 && totalBytes >= previous.totalBytes) {
            const qint64 elapsedMs = nowMs - previous.timestampMs;
            if (elapsedMs > 0) {
                const double kilobytesPerSecond =
                    (static_cast<double>(totalBytes - previous.totalBytes) * 1000.0) /
                    (static_cast<double>(elapsedMs) * 1024.0);
                display.throughput = QString("%1 KB/s").arg(kilobytesPerSecond, 0, 'f', 1);
            }
        }

        const QJsonValue latency = device.value("latency_us_average");
        if (latency.isDouble())
            display.latency = QString("%1 us").arg(latency.toDouble(), 0, 'f', 1);
        const QJsonValue jitter = device.value("jitter_us");
        if (jitter.isDouble())
            display.jitter = QString("%1 us").arg(jitter.toDouble(), 0, 'f', 1);

        nextDisplay.insert(busId, display);
        nextSamples.insert(busId, {totalBytes, nowMs});
    }

    telemetryByBusId = nextDisplay;
    previousTelemetrySamples = nextSamples;
}

void MainWindow::clearDeviceTable() {
    for (int i = usbDeviceTable->rowCount() - 1; i >= 0; --i)
        usbDeviceTable->removeRow(i);
}

QString MainWindow::getFreshBusId(const QString &targetVidPid) {
    if (targetVidPid.isEmpty()) return {};
    for (int row = 0; row < usbDeviceTable->rowCount(); ++row) {
        QTableWidgetItem *vpItem = usbDeviceTable->item(row, 2);
        if (vpItem && vpItem->text().compare(targetVidPid, Qt::CaseInsensitive) == 0) {
            QTableWidgetItem *nameItem = usbDeviceTable->item(row, 1);
            if (nameItem) return nameItem->data(Qt::UserRole).toString();
        }
    }
    return {};
}