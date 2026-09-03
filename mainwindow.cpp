#include "mainwindow.h"
#include "driverinstaller.h"
#include "nsddiscoverymanager.h"
#include <algorithm>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QMessageBox>
#include <QInputDialog>
#include <QCoreApplication>
#include <QFile>
#include <QMenuBar>
#include <QStyle>
#include <QThread>
#include <windows.h>
#include <lm.h>
#include <dxgi.h>
#include <wincrypt.h>

#include <vhci.h>
#include <remote.h>
#include "src/usbip_sdk/libusbip/src/usb_ids.h"
#include "src/transport/usb_transport.h"
#include "src/transport/hybrid_udp_transport.h"

#pragma comment(lib, "netapi32.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "crypt32.lib")

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent) {
    setWindowTitle("USBIP Client v1.0.4");
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
    bool savedAutoConnect = settings.value("autoConnect", false).toBool();
    bool savedMinimizeToTray = settings.value("minimizeToTray", false).toBool();
    
    settings.endGroup();

    hostIpLineEdit->setText(savedIp);
    portLineEdit->setText(savedPort);
    
    int themeIndex = themeCombo->findText(savedTheme);
    if (themeIndex >= 0) {
        themeCombo->setCurrentIndex(themeIndex);
    }
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
        if (endpoint.size() != 2)
            return;

        hostIpLineEdit->setText(endpoint.at(0));
        portLineEdit->setText(endpoint.at(1));
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

    telemetryTable = new QTableWidget(this);
    telemetryTable->setColumnCount(6);
    telemetryTable->setHorizontalHeaderLabels({"Bus ID", "Device", "Speed", "Class", "Jitter", "Throughput"});
    telemetryTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);

    telemetryDock = new QDockWidget("Live Device Telemetry", this);
    telemetryDock->setWidget(telemetryTable);
    addDockWidget(Qt::BottomDockWidgetArea, telemetryDock);

    QMenu *viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addAction(telemetryDock->toggleViewAction());

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

    usbDeviceTable = new QTableWidget(0, 8, this);
    usbDeviceTable->setHorizontalHeaderLabels({"Fav", "Device Name", "VID:PID", "Speed", "Status", "Attach Action", "Reset Action", "Protocol"});
    usbDeviceTable->horizontalHeaderItem(0)->setToolTip("Attach device automatically when connected to host.");
    usbDeviceTable->horizontalHeaderItem(3)->setToolTip("Selectable USB operational speed for this device.");
    usbDeviceTable->horizontalHeaderItem(5)->setToolTip("Mount or unmount this USB device to the Windows kernel.");
    usbDeviceTable->horizontalHeaderItem(7)->setToolTip("Transport protocol used to attach this device. UDP is experimental.");
    usbDeviceTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    usbDeviceTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    usbDeviceTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    usbDeviceTable->horizontalHeader()->setSectionResizeMode(7, QHeaderView::ResizeToContents);

    usbLayout->addWidget(usbDeviceTable);
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

    QComboBox *speedCombo = new QComboBox(this);
    speedCombo->setToolTip("Select USB operational speed supported by this device.");
    switch (detectedSpeed) {
        case UsbSuperSpeed:
            speedCombo->addItem("Super (5 Gbps)", static_cast<int>(UsbSuperSpeed));
            speedCombo->addItem("High (480 Mbps)", static_cast<int>(UsbHighSpeed));
            speedCombo->addItem("Full (12 Mbps)", static_cast<int>(UsbFullSpeed));
            speedCombo->addItem("Low (1.5 Mbps)", static_cast<int>(UsbLowSpeed));
            break;
        case UsbHighSpeed:
            speedCombo->addItem("High (480 Mbps)", static_cast<int>(UsbHighSpeed));
            speedCombo->addItem("Full (12 Mbps)", static_cast<int>(UsbFullSpeed));
            speedCombo->addItem("Low (1.5 Mbps)", static_cast<int>(UsbLowSpeed));
            break;
        case UsbFullSpeed:
            speedCombo->addItem("Full (12 Mbps)", static_cast<int>(UsbFullSpeed));
            speedCombo->addItem("Low (1.5 Mbps)", static_cast<int>(UsbLowSpeed));
            break;
        case UsbLowSpeed:
        default:
            speedCombo->addItem("Low (1.5 Mbps)", static_cast<int>(UsbLowSpeed));
            break;
    }

    int defaultIdx = speedCombo->findData(static_cast<int>(detectedSpeed));
    if (defaultIdx >= 0) {
        speedCombo->setCurrentIndex(defaultIdx);
    }
    if (attached) {
        speedCombo->setEnabled(false);
    }
    usbDeviceTable->setCellWidget(row, 3, speedCombo);

    usbDeviceTable->setItem(row, 4, new QTableWidgetItem(status));

    QPushButton *attachBtn = new QPushButton(attached ? "Detach" : "Attach", this);
    attachBtn->setToolTip("Mount or unmount this USB device to the Windows kernel.");
    QPushButton *resetBtn = new QPushButton("Reset Connection", this);
    resetBtn->setToolTip("Force reset the connection state for this device.");

    connect(attachBtn, &QPushButton::clicked, [this, row]() { handleToggleDeviceAttach(row); });
    connect(resetBtn, &QPushButton::clicked, [this, row]() { handleResetDeviceConnection(row); });

    usbDeviceTable->setCellWidget(row, 5, attachBtn);
    usbDeviceTable->setCellWidget(row, 6, resetBtn);

    QComboBox *protocolCombo = new QComboBox(this);
    protocolCombo->setToolTip("Transport protocol used to attach this device. UDP is experimental.");
    protocolCombo->addItem("TCP (Standard)", static_cast<int>(usbip::transport::TransportMode::TCP));
    protocolCombo->addItem("UDP (Experimental)", static_cast<int>(usbip::transport::TransportMode::UDP));
    if (attached) {
        protocolCombo->setEnabled(false);
    }
    usbDeviceTable->setCellWidget(row, 7, protocolCombo);
}

QWidget* MainWindow::createSettingsTab() {
    QWidget *tab = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(tab);

    QGroupBox *settingsGroup = new QGroupBox("Application Preferences", tab);
    QFormLayout *formLayout = new QFormLayout(settingsGroup);

    themeCombo = new QComboBox(this);
    themeCombo->addItems({"Dark", "Light", "High Contrast"});

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
        isLogicallyConnected = false;
        connectButton->setText("Connect");
        connectionStatusLabel->setText("Status: Disconnected");
        connectionStatusLabel->setStyleSheet("color: #ff3366; font-weight: bold;");
        logWindow->appendLog("INFO", "Disconnected from host by user request.");
        return;
    }

    logWindow->appendLog("INFO", QString("Checking USB/IP host %1:%2...").arg(ip).arg(port));
    if (!probeHost(ip, port)) {
        logWindow->appendLog("ERROR", QString("Cannot reach USB/IP host %1:%2. Check the host address and firewall (TCP %3).").arg(ip).arg(port).arg(port));
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
    connectionMonitorSocket->disconnectFromHost();
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
}

bool MainWindow::probeHost(const QString &ip, quint16 port)
{
    QTcpSocket socket;
    socket.connectToHost(ip, port);
    if (!socket.waitForConnected(3000)) {
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

    if (!probeHost(ip, port)) {
        logWindow->appendLog("ERROR", QString("Scan aborted: %1:%2 is unreachable (TCP timeout). Verify the host IP and firewall.").arg(ip).arg(port));
        return;
    }

    logWindow->appendLog("INFO", "Scanning exportable devices via usbip_sdk...");

    usbip::Socket sock = usbip::connect(ip.toStdString().c_str(), QString::number(port).toStdString().c_str());
    if (!sock) {
        logWindow->appendLog("ERROR", QString("Scan connect to %1:%2 failed (error %3).").arg(ip).arg(port).arg(GetLastError()));
        return;
    }

    QList<usbip::usb_device> devices;
    bool enumOk = false;
    try {
        enumOk = usbip::enum_exportable_devices(
            sock.get(),
            [&devices](int, const usbip::usb_device &dev) { devices.append(dev); },
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

    QComboBox *speedCombo = qobject_cast<QComboBox*>(usbDeviceTable->cellWidget(row, 3));
    QComboBox *protocolCombo = qobject_cast<QComboBox*>(usbDeviceTable->cellWidget(row, 7));

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
                        if (speedCombo) speedCombo->setEnabled(true);
                        if (protocolCombo) protocolCombo->setEnabled(true);
                    }
                } else {
                    logWindow->appendLog("ERROR", QString("vhci::detach() failed for bus %1 (error %2).").arg(busid).arg(err));
                }
                btn->setEnabled(true);
                return;
            }

            attachedPorts.remove(busid);
            if (auto *w = dropWatchers.take(busid)) { w->disconnectFromHost(); w->deleteLater(); }
            if (row < usbDeviceTable->rowCount()) {
                usbDeviceTable->item(row, 4)->setText("Available");
                btn->setText("Attach");
                if (speedCombo) speedCombo->setEnabled(true);
                if (protocolCombo) protocolCombo->setEnabled(true);
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

        QString speedText = speedCombo ? speedCombo->currentText() : "Default";
        logWindow->appendLog("INFO", QString("Attaching bus %1 (%2) from %3:%4...").arg(busid, speedText, ip, port));

        const auto transportMode = protocolCombo
            ? static_cast<usbip::transport::TransportMode>(protocolCombo->currentData().toInt())
            : usbip::transport::TransportMode::TCP;
        if (transportMode == usbip::transport::TransportMode::UDP) {
            logWindow->appendLog("INFO", "UDP transport selected: attempting hybrid TCP-handshake / UDP-data-plane.");
        }
        std::unique_ptr<usbip::transport::IUsbTransport> transport;
        if (transportMode == usbip::transport::TransportMode::UDP)
            transport = std::make_unique<usbip::transport::HybridUdpTransport>();
        else
            transport = std::make_unique<usbip::transport::TcpTransport>();

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
                clearDeviceTable();
                handleScanHost();
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
                            if (auto *sc = qobject_cast<QComboBox*>(usbDeviceTable->cellWidget(newRow, 3)))
                                sc->setEnabled(false);
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
            btn->setEnabled(true);
            return;
        }

        attachedPorts[busid] = hubPort;
        if (row < usbDeviceTable->rowCount()) {
            usbDeviceTable->item(row, 4)->setText("Attached");
            btn->setText("Detach");
            if (speedCombo) speedCombo->setEnabled(false);
            if (protocolCombo) protocolCombo->setEnabled(false);
        }
        logWindow->appendLog("INFO", QString("Attached bus %1 on hub port %2.").arg(busid).arg(hubPort));

        auto *watcher = new QTcpSocket(this);
        watcher->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
        const QString capturedBusid = busid;
        auto onDrop = [this, capturedBusid]() { handleNetworkDrop(capturedBusid); };
        connect(watcher, &QAbstractSocket::disconnected, this, onDrop);
        connect(watcher, &QAbstractSocket::errorOccurred, this, [onDrop](QAbstractSocket::SocketError) { onDrop(); });
        watcher->connectToHost(ip, port.toUShort());
        dropWatchers[busid] = watcher;
    } catch (const std::exception &ex) {
        logWindow->appendLog("ERROR", QString("SDK exception in attach/detach: %1").arg(ex.what()));
    }

    btn->setEnabled(true);
}

void MainWindow::handleHostDiscovered(const QString &hostname, const QHostAddress &address, quint16 port, int interfaceIndex)
{
    const QString ip = address.toString();
    const QString label = QStringLiteral("%1 (%2:%3)").arg(hostname, ip).arg(port);
    const QString hostKey = QStringLiteral("%1|%2").arg(hostname).arg(interfaceIndex);
    for (int i = 1; i < discoveredHostCombo->count(); ++i) {
        if (discoveredHostCombo->itemData(i, Qt::UserRole + 1).toString() != hostKey)
            continue;

        const QString oldEndpoint = discoveredHostCombo->itemData(i).toString();
        const QString newEndpoint = QStringLiteral("%1|%2").arg(ip).arg(port);
        if (oldEndpoint == newEndpoint)
            return;

        discoveredHostCombo->setItemText(i, label);
        discoveredHostCombo->setItemData(i, newEndpoint, Qt::UserRole);
        logWindow->appendLog("INFO", QStringLiteral("mDNS: host %1 changed endpoint from %2 to %3:%4")
            .arg(hostname, oldEndpoint.section('|', 0, 0), ip).arg(port));

        if (discoveredHostCombo->currentIndex() == i) {
            hostIpLineEdit->setText(ip);
            portLineEdit->setText(QString::number(port));
            logWindow->appendLog("INFO", QStringLiteral("Updated active host endpoint to %1:%2 from mDNS.").arg(ip).arg(port));
        }
        return;
    }
    const int index = discoveredHostCombo->count();
    discoveredHostCombo->addItem(label, QStringLiteral("%1|%2").arg(ip).arg(port));
    discoveredHostCombo->setItemData(index, hostKey, Qt::UserRole + 1);
    logWindow->appendLog("INFO", QStringLiteral("mDNS: discovered host %1 at %2:%3").arg(hostname, ip).arg(port));
}

void MainWindow::handleNetworkDrop(const QString &busid)
{
    if (!attachedPorts.contains(busid))
        return;

    logWindow->appendLog("WARNING", QString("Network drop detected for bus %1 — forcing detach.").arg(busid));

    if (auto *w = dropWatchers.take(busid)) {
        w->blockSignals(true);
        w->disconnectFromHost();
        w->deleteLater();
    }

    const int hubPort = attachedPorts.take(busid);

    usbip::Handle dev = usbip::vhci::open();
    if (dev)
        usbip::vhci::detach(dev.get(), hubPort);

    for (int r = 0; r < usbDeviceTable->rowCount(); ++r) {
        QTableWidgetItem *item = usbDeviceTable->item(r, 1);
        if (!item || item->data(Qt::UserRole).toString() != busid)
            continue;

        usbDeviceTable->item(r, 4)->setText("Disconnected");
        if (auto *btn = qobject_cast<QPushButton*>(usbDeviceTable->cellWidget(r, 5))) {
            btn->setText("Attach");
            btn->setEnabled(true);
        }
        if (auto *sc = qobject_cast<QComboBox*>(usbDeviceTable->cellWidget(r, 3)))
            sc->setEnabled(true);
        if (auto *pc = qobject_cast<QComboBox*>(usbDeviceTable->cellWidget(r, 7)))
            pc->setEnabled(true);
        break;
    }
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
                if (auto *sc = qobject_cast<QComboBox*>(usbDeviceTable->cellWidget(row, 3)))
                    sc->setEnabled(true);
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
            if (auto *sc = qobject_cast<QComboBox*>(usbDeviceTable->cellWidget(row, 3)))
                sc->setEnabled(true);
        }
        return;
    }

    attachedPorts[busid] = newHubPort;
    if (row < usbDeviceTable->rowCount()) {
        usbDeviceTable->item(row, 4)->setText("Attached");
        if (auto *sc = qobject_cast<QComboBox*>(usbDeviceTable->cellWidget(row, 3)))
            sc->setEnabled(false);
    }
    logWindow->appendLog("INFO", QString("Reset complete: bus %1 re-attached on hub port %2.").arg(busid).arg(newHubPort));
}

void MainWindow::handleThemeChange(int index) {
    QString theme = themeCombo->itemText(index);
    applyTheme(theme);
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
    if (themeName == "Dark") {
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

    for (int row = 0; row < usbDeviceTable->rowCount(); ++row) {
        QPushButton *actionBtn = qobject_cast<QPushButton*>(usbDeviceTable->cellWidget(row, 5));
        if (actionBtn && actionBtn->text() == "Detach") {
            QTableWidgetItem *nameItem = usbDeviceTable->item(row, 1);
            if (!nameItem) continue;
            QString currentBusId = nameItem->data(Qt::UserRole).toString();

            bool isActuallyAttached = false;
            for (const auto &importedDev : importedDevices) {
                if (QString::fromStdString(importedDev.location.busid) == currentBusId) {
                    isActuallyAttached = true;
                    break;
                }
            }

            if (!isActuallyAttached) {
                logWindow->appendLog("INFO", QString("Background monitor detected drop for bus %1. Syncing UI.").arg(currentBusId));
                attachedPorts.remove(currentBusId);
                if (auto *statusItem = usbDeviceTable->item(row, 4)) {
                    statusItem->setText("Available");
                }
                actionBtn->setText("Attach");
                if (auto *speedCombo = qobject_cast<QComboBox*>(usbDeviceTable->cellWidget(row, 3))) {
                    speedCombo->setEnabled(true);
                }
            }
        }
    }
}

void MainWindow::refreshTelemetryStats() {
    syncDeviceStates();

    telemetryTable->setRowCount(0);

    usbip::Handle dev = usbip::vhci::open();
    if (!dev) return;

    auto devicesOpt = usbip::vhci::get_imported_devices(dev.get());
    if (!devicesOpt) return;

    for (const auto &importedDev : *devicesOpt) {
        QString busId = QString::fromStdString(importedDev.location.busid);
        int row = telemetryTable->rowCount();
        telemetryTable->insertRow(row);

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

        QString throughputStr = "0 KB/s";
        QString jitterStr    = "N/A";
        QString devClass     = "N/A";

        telemetryTable->setItem(row, 0, new QTableWidgetItem(busId));
        telemetryTable->setItem(row, 1, new QTableWidgetItem(deviceName));
        telemetryTable->setItem(row, 2, new QTableWidgetItem(speedStr));
        telemetryTable->setItem(row, 3, new QTableWidgetItem(devClass));
        telemetryTable->setItem(row, 4, new QTableWidgetItem(jitterStr));
        telemetryTable->setItem(row, 5, new QTableWidgetItem(throughputStr));
    }
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