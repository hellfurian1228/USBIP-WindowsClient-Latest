#include "nsddiscoverymanager.h"
#include <QNetworkDatagram>
#include <QDebug>

NsdDiscoveryManager::NsdDiscoveryManager(QObject *parent) : QObject(parent) {
    udpSocket = new QUdpSocket(this);
    queryTimer = new QTimer(this);
    connect(queryTimer, &QTimer::timeout, this, &NsdDiscoveryManager::sendDiscoveryQuery);
    connect(udpSocket, &QUdpSocket::readyRead, this, &NsdDiscoveryManager::readPendingDatagrams);
}

void NsdDiscoveryManager::startDiscovery() {
    if (discoveryActive)
        stopDiscovery();

    if (udpSocket->bind(QHostAddress::AnyIPv4, MDNS_PORT,
                        QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        multicastInterfaces.clear();
        for (const QNetworkInterface &networkInterface : QNetworkInterface::allInterfaces()) {
            const auto flags = networkInterface.flags();
            if (!flags.testFlag(QNetworkInterface::IsUp) ||
                !flags.testFlag(QNetworkInterface::IsRunning) ||
                !flags.testFlag(QNetworkInterface::CanMulticast) ||
                flags.testFlag(QNetworkInterface::IsLoopBack)) {
                continue;
            }

            if (preferredInterfaceIndex >= 0 && networkInterface.index() != preferredInterfaceIndex)
                continue;

            if (udpSocket->joinMulticastGroup(MDNS_GROUP, networkInterface))
                multicastInterfaces.append(networkInterface);
        }

        discoveryActive = true;
        sendDiscoveryQuery();
        queryTimer->start(5000);
        qDebug() << "mDNS Discovery started on port" << MDNS_PORT
                 << "across" << multicastInterfaces.size() << "interfaces";
    } else {
        qWarning() << "Failed to bind mDNS socket to port" << MDNS_PORT;
    }
}

void NsdDiscoveryManager::stopDiscovery() {
    queryTimer->stop();
    for (const QNetworkInterface &networkInterface : multicastInterfaces)
        udpSocket->leaveMulticastGroup(MDNS_GROUP, networkInterface);
    multicastInterfaces.clear();
    udpSocket->close();
    discoveryActive = false;
    qDebug() << "mDNS Discovery stopped.";
}

void NsdDiscoveryManager::setInterfaceIndex(int interfaceIndex)
{
    if (preferredInterfaceIndex == interfaceIndex)
        return;

    preferredInterfaceIndex = interfaceIndex;
    if (discoveryActive)
        startDiscovery();
}

void NsdDiscoveryManager::sendDiscoveryQuery() {
    // DNS-SD PTR query for _usbip._tcp.local
    QByteArray queryPacket;
    queryPacket.append("\x00\x00\x01\x00\x00\x01\x00\x00\x00\x00\x00\x00", 12); // header
    queryPacket.append("\x06" "_usbip" "\x04" "_tcp" "\x05" "local" "\x00", 19); // QNAME
    queryPacket.append("\x00\x0c\x00\x01", 4);                                   // PTR, IN

    for (const QNetworkInterface &networkInterface : multicastInterfaces) {
        udpSocket->setMulticastInterface(networkInterface);
        udpSocket->writeDatagram(queryPacket, MDNS_GROUP, MDNS_PORT);
    }
}

void NsdDiscoveryManager::readPendingDatagrams() {
    while (udpSocket->hasPendingDatagrams()) {
        QNetworkDatagram datagram = udpSocket->receiveDatagram();
        QByteArray data = datagram.data();

        if (data.contains("USBIP-AndroidHost")) {
            emit hostDiscovered(QStringLiteral("USBIP-AndroidHost"),
                                datagram.senderAddress(),
                                3240,
                                static_cast<int>(datagram.interfaceIndex()));
        }
    }
}
