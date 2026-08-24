#pragma once

#include <QObject>
#include <QHostAddress>
#include <QUdpSocket>
#include <QTimer>

class NsdDiscoveryManager : public QObject {
    Q_OBJECT
public:
    explicit NsdDiscoveryManager(QObject *parent = nullptr);
    void startDiscovery();
    void stopDiscovery();

signals:
    void hostDiscovered(const QString &hostname, const QHostAddress &address, quint16 port);

private slots:
    void readPendingDatagrams();
    void sendDiscoveryQuery();

private:
    QUdpSocket *udpSocket;
    QTimer *queryTimer;
    static constexpr quint16 MDNS_PORT = 5353;
    const QHostAddress MDNS_GROUP{QStringLiteral("224.0.0.251")};
};
