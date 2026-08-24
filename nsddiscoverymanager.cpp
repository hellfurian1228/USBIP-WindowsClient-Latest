#include "nsddiscoverymanager.h"
#include <QNetworkDatagram>
#include <QDebug>

NsdDiscoveryManager::NsdDiscoveryManager(QObject *parent) : QObject(parent) {
    udpSocket = new QUdpSocket(this);
    queryTimer = new QTimer(this);
    connect(queryTimer, &QTimer::timeout, this, &NsdDiscoveryManager::sendDiscoveryQuery);
}

void NsdDiscoveryManager::startDiscovery() {
    if (udpSocket->bind(QHostAddress::AnyIPv4, MDNS_PORT,
                        QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        udpSocket->joinMulticastGroup(MDNS_GROUP);
        connect(udpSocket, &QUdpSocket::readyRead, this, &NsdDiscoveryManager::readPendingDatagrams);
        queryTimer->start(5000);
        qDebug() << "mDNS Discovery started on port" << MDNS_PORT;
    } else {
        qWarning() << "Failed to bind mDNS socket to port" << MDNS_PORT;
    }
}

void NsdDiscoveryManager::stopDiscovery() {
    queryTimer->stop();
    udpSocket->leaveMulticastGroup(MDNS_GROUP);
    udpSocket->close();
    qDebug() << "mDNS Discovery stopped.";
}

void NsdDiscoveryManager::sendDiscoveryQuery() {
    // DNS-SD PTR query for _usbip._tcp.local
    QByteArray queryPacket;
    queryPacket.append("\x00\x00\x01\x00\x00\x01\x00\x00\x00\x00\x00\x00", 12); // header
    queryPacket.append("\x06" "_usbip" "\x04" "_tcp" "\x05" "local" "\x00", 19); // QNAME
    queryPacket.append("\x00\x0c\x00\x01", 4);                                   // PTR, IN

    udpSocket->writeDatagram(queryPacket, MDNS_GROUP, MDNS_PORT);
}

void NsdDiscoveryManager::readPendingDatagrams() {
    while (udpSocket->hasPendingDatagrams()) {
        QNetworkDatagram datagram = udpSocket->receiveDatagram();
        QByteArray data = datagram.data();

        if (data.contains("USBIP-AndroidHost")) {
            emit hostDiscovered(QStringLiteral("USBIP-AndroidHost"),
                                datagram.senderAddress(),
                                3240);
        }
    }
}
