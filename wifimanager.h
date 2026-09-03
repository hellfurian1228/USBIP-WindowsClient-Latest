#pragma once

#include <QString>
#include <QList>

struct WifiNetwork
{
    QString ssid;
    bool secured = false;
    bool connected = false;
    bool saved = false;
    int interfaceIndex = 0;
    QString band;
    int networkInterfaceIndex = -1;
    bool ethernet = false;
};

class WifiManager
{
public:
    QList<WifiNetwork> scan() const;
    bool connect(const WifiNetwork &network, const QString &password, QString *error) const;
    bool disconnect(int interfaceIndex, QString *error) const;
};
