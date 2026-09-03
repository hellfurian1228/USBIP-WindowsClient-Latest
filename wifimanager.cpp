#include "wifimanager.h"

#include <windows.h>
#include <wlanapi.h>
#include <objbase.h>

#include <QByteArray>
#include <QXmlStreamWriter>

#include <algorithm>
#include <QHash>
#include <QSet>
#include <QNetworkInterface>

#pragma comment(lib, "wlanapi.lib")
#pragma comment(lib, "ole32.lib")

namespace
{
QString ssidToString(const DOT11_SSID &ssid)
{
    return QString::fromUtf8(reinterpret_cast<const char *>(ssid.ucSSID), ssid.uSSIDLength);
}

QString profileNameFor(const QString &ssid)
{
    return ssid;
}

QByteArray ssidHex(const QString &ssid)
{
    const QByteArray bytes = ssid.toUtf8();
    return bytes.toHex().toUpper();
}

bool hasSavedProfile(HANDLE client, const GUID &interfaceGuid, const QString &ssid)
{
    WLAN_PROFILE_INFO_LIST *profiles = nullptr;
    if (WlanGetProfileList(client, &interfaceGuid, nullptr, &profiles) != ERROR_SUCCESS)
        return false;

    bool found = false;
    for (DWORD i = 0; i < profiles->dwNumberOfItems; ++i) {
        if (QString::fromWCharArray(profiles->ProfileInfo[i].strProfileName) == profileNameFor(ssid)) {
            found = true;
            break;
        }
    }
    WlanFreeMemory(profiles);
    return found;
}

QString createProfile(const WifiNetwork &network, const QString &password)
{
    QString xml;
    QXmlStreamWriter writer(&xml);
    writer.writeStartDocument();
    writer.writeStartElement("WLANProfile");
    writer.writeAttribute("xmlns", "http://www.microsoft.com/networking/WLAN/profile/v1");
    writer.writeTextElement("name", profileNameFor(network.ssid));
    writer.writeStartElement("SSIDConfig");
    writer.writeStartElement("SSID");
    writer.writeTextElement("hex", QString::fromLatin1(ssidHex(network.ssid)));
    writer.writeTextElement("name", network.ssid);
    writer.writeEndElement();
    writer.writeEndElement();
    writer.writeTextElement("connectionType", "ESS");
    writer.writeTextElement("connectionMode", "auto");
    writer.writeStartElement("MSM");
    writer.writeStartElement("security");
    writer.writeStartElement("authEncryption");
    writer.writeTextElement("authentication", network.secured ? "WPA2PSK" : "open");
    writer.writeTextElement("encryption", network.secured ? "AES" : "none");
    writer.writeTextElement("useOneX", "false");
    writer.writeEndElement();
    if (network.secured) {
        writer.writeStartElement("sharedKey");
        writer.writeTextElement("keyType", "passPhrase");
        writer.writeTextElement("protected", "false");
        writer.writeTextElement("keyMaterial", password);
        writer.writeEndElement();
    }
    writer.writeEndElement();
    writer.writeEndElement();
    writer.writeEndElement();
    writer.writeEndDocument();
    return xml;
}

QString bandForFrequency(ULONG frequency)
{
    if (frequency >= 2300000 && frequency < 2500000)
        return QStringLiteral("2.4 GHz");
    if (frequency >= 4900000 && frequency < 5900000)
        return QStringLiteral("5 GHz");
    if (frequency >= 5900000 && frequency < 7200000)
        return QStringLiteral("6 GHz");
    return QString();
}

int qtInterfaceIndexForDescription(const QString &description)
{
    for (const QNetworkInterface &networkInterface : QNetworkInterface::allInterfaces()) {
        if (networkInterface.humanReadableName() == description)
            return static_cast<int>(networkInterface.index());
    }
    return -1;
}
}

QList<WifiNetwork> WifiManager::scan() const
{
    QList<WifiNetwork> networks;
    HANDLE client = nullptr;
    DWORD version = 0;
    if (WlanOpenHandle(2, nullptr, &version, &client) != ERROR_SUCCESS)
        return networks;

    WLAN_INTERFACE_INFO_LIST *interfaces = nullptr;
    if (WlanEnumInterfaces(client, nullptr, &interfaces) != ERROR_SUCCESS) {
        WlanCloseHandle(client, nullptr);
        return networks;
    }

    for (DWORD interfaceIndex = 0; interfaceIndex < interfaces->dwNumberOfItems; ++interfaceIndex) {
        const WLAN_INTERFACE_INFO &interfaceInfo = interfaces->InterfaceInfo[interfaceIndex];
        QHash<QString, QSet<QString>> bandsBySsid;
        WLAN_BSS_LIST *bssList = nullptr;
        if (WlanGetNetworkBssList(client, &interfaceInfo.InterfaceGuid, nullptr,
                                  dot11_BSS_type_any, FALSE, nullptr, &bssList) == ERROR_SUCCESS) {
            for (DWORD bssIndex = 0; bssIndex < bssList->dwNumberOfItems; ++bssIndex) {
                const WLAN_BSS_ENTRY &bss = bssList->wlanBssEntries[bssIndex];
                const QString band = bandForFrequency(bss.ulChCenterFrequency);
                if (!band.isEmpty())
                    bandsBySsid[ssidToString(bss.dot11Ssid)].insert(band);
            }
            WlanFreeMemory(bssList);
        }

        QString connectedSsid;
        WLAN_CONNECTION_ATTRIBUTES *connection = nullptr;
        DWORD connectionSize = 0;
        WLAN_OPCODE_VALUE_TYPE opcodeType{};
        if (WlanQueryInterface(client, &interfaceInfo.InterfaceGuid,
                               wlan_intf_opcode_current_connection, nullptr,
                               &connectionSize, reinterpret_cast<PVOID *>(&connection),
                               &opcodeType) == ERROR_SUCCESS) {
            connectedSsid = ssidToString(connection->wlanAssociationAttributes.dot11Ssid);
            WlanFreeMemory(connection);
        }

        WLAN_PROFILE_INFO_LIST *profiles = nullptr;
        if (WlanGetProfileList(client, &interfaceInfo.InterfaceGuid, nullptr, &profiles) == ERROR_SUCCESS) {
            for (DWORD profileIndex = 0; profileIndex < profiles->dwNumberOfItems; ++profileIndex) {
                WifiNetwork network;
                network.ssid = QString::fromWCharArray(profiles->ProfileInfo[profileIndex].strProfileName);
                if (network.ssid.isEmpty())
                    continue;
                network.saved = true;
                network.interfaceIndex = static_cast<int>(interfaceIndex);
                const QString interfaceDescription = QString::fromWCharArray(interfaceInfo.strInterfaceDescription);
                network.networkInterfaceIndex = qtInterfaceIndexForDescription(interfaceDescription);
                network.connected = network.ssid == connectedSsid;
                network.band = bandsBySsid.value(network.ssid).values().join(QStringLiteral(" / "));
                if (network.band.isEmpty())
                    network.band = QStringLiteral("Band unavailable");
                networks.append(network);
            }
            WlanFreeMemory(profiles);
        }
    }

    WlanFreeMemory(interfaces);
    WlanCloseHandle(client, nullptr);

    for (const QNetworkInterface &networkInterface : QNetworkInterface::allInterfaces()) {
        const auto flags = networkInterface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp) ||
            !flags.testFlag(QNetworkInterface::IsRunning) ||
            flags.testFlag(QNetworkInterface::IsLoopBack) ||
            networkInterface.type() != QNetworkInterface::Ethernet)
            continue;

        WifiNetwork network;
        network.ssid = networkInterface.humanReadableName();
        if (network.ssid.isEmpty())
            network.ssid = networkInterface.name();
        network.ethernet = true;
        network.connected = true;
        network.saved = true;
        network.band = QStringLiteral("Ethernet");
        network.interfaceIndex = static_cast<int>(networkInterface.index());
        networks.append(network);
    }
    return networks;
}

bool WifiManager::disconnect(int interfaceIndex, QString *error) const
{
    HANDLE client = nullptr;
    DWORD version = 0;
    if (WlanOpenHandle(2, nullptr, &version, &client) != ERROR_SUCCESS) {
        if (error) *error = QStringLiteral("Unable to open Windows WLAN service.");
        return false;
    }

    WLAN_INTERFACE_INFO_LIST *interfaces = nullptr;
    if (WlanEnumInterfaces(client, nullptr, &interfaces) != ERROR_SUCCESS) {
        if (error) *error = QStringLiteral("Unable to enumerate Wi-Fi adapters.");
        WlanCloseHandle(client, nullptr);
        return false;
    }

    DWORD result = ERROR_NOT_FOUND;
    for (DWORD i = 0; i < interfaces->dwNumberOfItems; ++i) {
        const WLAN_INTERFACE_INFO &interfaceInfo = interfaces->InterfaceInfo[i];
        if (static_cast<int>(i) == interfaceIndex) {
            result = WlanDisconnect(client, &interfaceInfo.InterfaceGuid, nullptr);
            break;
        }
    }

    WlanFreeMemory(interfaces);
    WlanCloseHandle(client, nullptr);
    if (result != ERROR_SUCCESS && error)
        *error = QStringLiteral("Wi-Fi disconnect failed (error %1).").arg(result);
    return result == ERROR_SUCCESS;
}

bool WifiManager::connect(const WifiNetwork &network, const QString &password, QString *error) const
{
    HANDLE client = nullptr;
    DWORD version = 0;
    if (WlanOpenHandle(2, nullptr, &version, &client) != ERROR_SUCCESS) {
        if (error) *error = QStringLiteral("Unable to open Windows WLAN service.");
        return false;
    }

    WLAN_INTERFACE_INFO_LIST *interfaces = nullptr;
    if (WlanEnumInterfaces(client, nullptr, &interfaces) != ERROR_SUCCESS || interfaces->dwNumberOfItems == 0) {
        if (error) *error = QStringLiteral("No Wi-Fi adapter is available.");
        if (interfaces) WlanFreeMemory(interfaces);
        WlanCloseHandle(client, nullptr);
        return false;
    }

    const int selectedInterface = std::clamp(network.interfaceIndex, 0,
                                             static_cast<int>(interfaces->dwNumberOfItems) - 1);
    const GUID interfaceGuid = interfaces->InterfaceInfo[selectedInterface].InterfaceGuid;
    const bool saved = hasSavedProfile(client, interfaceGuid, network.ssid);
    if (network.secured && !saved && password.isEmpty()) {
        if (error) *error = QStringLiteral("A password is required for this network.");
        WlanFreeMemory(interfaces);
        WlanCloseHandle(client, nullptr);
        return false;
    }

    DWORD reason = 0;
    if (!saved || !password.isEmpty()) {
        const QString profile = createProfile(network, password);
        const DWORD result = WlanSetProfile(client, &interfaceGuid, 0, profile.toStdWString().c_str(), nullptr, TRUE, nullptr, &reason);
        if (result != ERROR_SUCCESS) {
            if (error) *error = QStringLiteral("Could not save the Wi-Fi profile (error %1).").arg(result);
            WlanFreeMemory(interfaces);
            WlanCloseHandle(client, nullptr);
            return false;
        }
    }

    WLAN_CONNECTION_PARAMETERS parameters{};
    parameters.wlanConnectionMode = wlan_connection_mode_profile;
    const std::wstring profileName = profileNameFor(network.ssid).toStdWString();
    parameters.strProfile = profileName.c_str();
    parameters.dot11BssType = dot11_BSS_type_infrastructure;
    const DWORD result = WlanConnect(client, &interfaceGuid, &parameters, nullptr);
    if (result != ERROR_SUCCESS && error)
        *error = QStringLiteral("Wi-Fi connection failed (error %1).").arg(result);

    WlanFreeMemory(interfaces);
    WlanCloseHandle(client, nullptr);
    return result == ERROR_SUCCESS;
}
