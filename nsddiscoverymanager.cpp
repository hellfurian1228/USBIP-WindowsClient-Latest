#include "nsddiscoverymanager.h"
#include <QNetworkDatagram>
#include <QDebug>

namespace {
constexpr quint16 defaultTelemetryPort = 3241;

bool skipDnsName(const QByteArray &packet, int *offset) {
    while (*offset < packet.size()) {
        const quint8 length = static_cast<quint8>(packet.at(*offset));
        if (length == 0) {
            ++*offset;
            return true;
        }
        if ((length & 0xc0) == 0xc0) {
            if (*offset + 1 >= packet.size())
                return false;
            *offset += 2;
            return true;
        }
        if (length > 63 || *offset + 1 + length > packet.size())
            return false;
        *offset += 1 + length;
    }
    return false;
}

quint16 telemetryPortFromDnsSd(const QByteArray &packet) {
    if (packet.size() < 12)
        return defaultTelemetryPort;

    const auto readU16 = [&packet](int offset) {
        return static_cast<quint16>((static_cast<quint8>(packet.at(offset)) << 8) |
                                     static_cast<quint8>(packet.at(offset + 1)));
    };
    const auto readU32 = [&packet](int offset) {
        return (static_cast<quint32>(static_cast<quint8>(packet.at(offset))) << 24) |
               (static_cast<quint32>(static_cast<quint8>(packet.at(offset + 1))) << 16) |
               (static_cast<quint32>(static_cast<quint8>(packet.at(offset + 2))) << 8) |
               static_cast<quint8>(packet.at(offset + 3));
    };

    int offset = 12;
    const quint16 questionCount = readU16(4);
    const quint16 answerCount = readU16(6);
    const quint16 authorityCount = readU16(8);
    const quint16 additionalCount = readU16(10);
    for (quint16 i = 0; i < questionCount; ++i) {
        if (!skipDnsName(packet, &offset) || offset + 4 > packet.size())
            return defaultTelemetryPort;
        offset += 4;
    }

    const quint32 recordCount = static_cast<quint32>(answerCount) + authorityCount + additionalCount;
    for (quint32 i = 0; i < recordCount; ++i) {
        if (!skipDnsName(packet, &offset) || offset + 10 > packet.size())
            return defaultTelemetryPort;
        const quint16 type = readU16(offset);
        offset += 8;
        const quint16 dataLength = readU16(offset);
        offset += 2;
        if (offset + dataLength > packet.size())
            return defaultTelemetryPort;

        if (type == 16) {
            const int dataStart = offset;
            const int end = dataStart + dataLength;
            while (offset < end) {
                const quint8 textLength = static_cast<quint8>(packet.at(offset++));
                if (offset + textLength > end)
                    break;
                const QByteArray text = packet.mid(offset, textLength);
                offset += textLength;
                const QByteArray prefix("telemetry_port=");
                if (text.left(prefix.size()).toLower() == prefix) {
                    bool ok = false;
                    const uint port = text.mid(prefix.size()).toUInt(&ok);
                    if (ok && port >= 1 && port <= 65535)
                        return static_cast<quint16>(port);
                }
            }
            offset = dataStart;
        }
        offset += dataLength;
    }
    return defaultTelemetryPort;
}
}

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
            const quint16 telemetryPort = telemetryPortFromDnsSd(data);
            emit hostDiscovered(QStringLiteral("USBIP-AndroidHost"),
                                datagram.senderAddress(),
                                3240,
                                telemetryPort,
                                static_cast<int>(datagram.interfaceIndex()));
        }
    }
}
