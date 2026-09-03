#pragma once

#include <QObject>
#include <QHostAddress>
#include <QUdpSocket>
#include <QTimer>
#include <QNetworkInterface>

class NsdDiscoveryManager : public QObject {
    Q_OBJECT
public:
    explicit NsdDiscoveryManager(QObject *parent = nullptr);
    void startDiscovery();
    void stopDiscovery();
    void setInterfaceIndex(int interfaceIndex);

signals:
    void hostDiscovered(const QString &hostname, const QHostAddress &address, quint16 port, int interfaceIndex);

private slots:
    void readPendingDatagrams();
    void sendDiscoveryQuery();

private:
    QUdpSocket *udpSocket;
    QTimer *queryTimer;
    QList<QNetworkInterface> multicastInterfaces;
    int preferredInterfaceIndex = -1;
    bool discoveryActive = false;
    static constexpr quint16 MDNS_PORT = 5353;
    const QHostAddress MDNS_GROUP{QStringLiteral("224.0.0.251")};
};
