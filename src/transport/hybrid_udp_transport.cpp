/*
 * hybrid_udp_transport.cpp
 */

#include "hybrid_udp_transport.h"

#include <vhci.h>
#include <usbip/proto_op.h>
#include <usbip/consts.h>

#include <QDataStream>
#include <QHostAddress>
#include <QMutexLocker>
#include <QTcpSocket>

#include <cstring>

// ntohl / htonl without pulling in all of winsock2 again
#include <winsock2.h>

namespace usbip::transport
{

// ── HybridUdpWorker ──────────────────────────────────────────────────────────

HybridUdpWorker::HybridUdpWorker(QObject *parent)
    : QObject(parent)
{}

HybridUdpWorker::~HybridUdpWorker()
{
    closeSocket();
}

void HybridUdpWorker::setTarget(const QString &host, quint16 udpPort)
{
    m_host    = host;
    m_udpPort = udpPort;
}

void HybridUdpWorker::run()
{
    m_socket = new QUdpSocket(this);

    // Bind to any local port; we send/receive to the server's udpPort.
    if (!m_socket->bind(QHostAddress::AnyIPv4, 0)) {
        emit sessionError(QStringLiteral("UDP bind failed: ") + m_socket->errorString());
        return;
    }

    connect(m_socket, &QUdpSocket::readyRead,
            this,     &HybridUdpWorker::handleIncomingDatagram);
}

bool HybridUdpWorker::sendUrb(std::span<const uint8_t> data)
{
    if (!m_socket || m_stop.load(std::memory_order_relaxed))
        return false;

    const uint32_t seq = htonl(m_nextSendSeq.fetch_add(1, std::memory_order_relaxed));

    QByteArray datagram;
    datagram.resize(static_cast<qsizetype>(sizeof(udp_urb_header) + data.size()));
    std::memcpy(datagram.data(), &seq, sizeof(seq));
    std::memcpy(datagram.data() + sizeof(udp_urb_header), data.data(), data.size());

    const qint64 sent = m_socket->writeDatagram(
        datagram, QHostAddress(m_host), m_udpPort);

    return sent == static_cast<qint64>(datagram.size());
}

bool HybridUdpWorker::receiveUrb(std::span<uint8_t> buffer)
{
    QMutexLocker lock(&m_recvMutex);

    if (!m_recvCond.wait(&m_recvMutex, UDP_RECV_TIMEOUT_MS)) {
        emit sessionError(QStringLiteral("UDP receive timed out"));
        return false;
    }

    if (m_recvError)
        return false;

    const qsizetype copyLen =
        std::min(static_cast<qsizetype>(buffer.size()), m_recvBuffer.size());
    std::memcpy(buffer.data(), m_recvBuffer.constData(), static_cast<size_t>(copyLen));

    m_recvReady = false;
    m_recvBuffer.clear();
    return true;
}

void HybridUdpWorker::handleIncomingDatagram()
{
    while (m_socket->hasPendingDatagrams()) {
        QByteArray datagram(static_cast<qsizetype>(m_socket->pendingDatagramSize()), Qt::Uninitialized);
        m_socket->readDatagram(datagram.data(), datagram.size());

        if (datagram.size() < static_cast<qsizetype>(sizeof(udp_urb_header))) {
            emit sessionError(QStringLiteral("UDP datagram too short"));
            requestStop();
            return;
        }

        uint32_t recvSeqNet = 0;
        std::memcpy(&recvSeqNet, datagram.constData(), sizeof(recvSeqNet));
        const uint32_t recvSeq = ntohl(recvSeqNet);

        if (recvSeq != m_nextRecvSeq) {
            emit sessionError(
                QStringLiteral("UDP sequence mismatch: expected %1, got %2")
                    .arg(m_nextRecvSeq).arg(recvSeq));
            requestStop();
            return;
        }
        ++m_nextRecvSeq;

        QMutexLocker lock(&m_recvMutex);
        m_recvBuffer = datagram.mid(static_cast<qsizetype>(sizeof(udp_urb_header)));
        m_recvReady  = true;
        m_recvError  = false;
        m_recvCond.wakeOne();
    }
}

void HybridUdpWorker::requestStop()
{
    m_stop.store(true, std::memory_order_relaxed);

    QMutexLocker lock(&m_recvMutex);
    m_recvError = true;
    m_recvCond.wakeAll();

    closeSocket();
}

void HybridUdpWorker::closeSocket()
{
    if (m_socket) {
        m_socket->close();
        // m_socket is owned by this QObject; it will be deleted with it.
        m_socket = nullptr;
    }
}

// ── HybridUdpTransport ───────────────────────────────────────────────────────

HybridUdpTransport::HybridUdpTransport() = default;

HybridUdpTransport::~HybridUdpTransport()
{
    stopWorker();
}

int HybridUdpTransport::connect(HANDLE dev, const usbip::device_location &location)
{
    const QString host = QString::fromStdString(location.hostname);

    // Determine TCP port (fall back to standard 3240 if service string is empty).
    bool ok = false;
    const quint16 tcpPort = QString::fromStdString(location.service).toUShort(&ok);
    const quint16 resolvedTcpPort = (ok && tcpPort > 0) ? tcpPort
                                                         : static_cast<quint16>(USBIP_TCP_PORT);

    const quint16 udpPort = negotiateUdp(host, resolvedTcpPort, location.busid);

    // Always attach via the kernel vhci driver regardless of UDP outcome.
    const int hubPort = usbip::vhci::attach(dev, location);
    if (hubPort < 1)
        return hubPort;

    if (udpPort > 0) {
        startWorker(host, udpPort);
        m_udpActive = true;
    }

    return hubPort;
}

bool HybridUdpTransport::sendUrb(std::span<const uint8_t> data)
{
    if (m_udpActive && m_worker)
        return m_worker->sendUrb(data);
    return true; // kernel driver owns URB I/O when UDP is not active
}

bool HybridUdpTransport::receiveUrb(std::span<uint8_t> buffer)
{
    if (m_udpActive && m_worker)
        return m_worker->receiveUrb(buffer);
    return true;
}

void HybridUdpTransport::disconnect(HANDLE dev, int hubPort)
{
    stopWorker();
    usbip::vhci::detach(dev, hubPort);
}

// ── Private helpers ──────────────────────────────────────────────────────────

quint16 HybridUdpTransport::negotiateUdp(const QString &host,
                                          quint16 tcpPort,
                                          const std::string &busid)
{
    QTcpSocket tcp;
    tcp.connectToHost(host, tcpPort);
    if (!tcp.waitForConnected(5000))
        return 0;

    // Build op_common (network byte order).
    usbip::op_common hdr{};
    hdr.version = htons(USBIP_VERSION);
    hdr.code    = htons(OP_REQ_IMPORT);
    hdr.status  = 0;

    // Build op_import_request.
    usbip::op_import_request req{};
    const size_t copyLen = std::min(busid.size(), sizeof(req.busid) - size_t{1});
    std::memcpy(req.busid, busid.c_str(), copyLen);

    // Build UDP extension.
    udp_request_extension ext{};
    ext.flags       = htonl(FLAG_REQUEST_UDP);
    ext.client_port = 0;
    ext._reserved   = 0;

    QByteArray packet;
    packet.append(reinterpret_cast<const char *>(&hdr), sizeof(hdr));
    packet.append(reinterpret_cast<const char *>(&req), sizeof(req));
    packet.append(reinterpret_cast<const char *>(&ext), sizeof(ext));

    tcp.write(packet);
    if (!tcp.waitForBytesWritten(3000))
        return 0;

    // Read op_common reply.
    if (!tcp.waitForReadyRead(5000))
        return 0;

    usbip::op_common replyHdr{};
    if (tcp.read(reinterpret_cast<char *>(&replyHdr), sizeof(replyHdr))
            != static_cast<qint64>(sizeof(replyHdr)))
        return 0;

    usbip::byteswap(replyHdr);

    if (replyHdr.code != OP_REP_IMPORT || replyHdr.status != ST_OK)
        return 0;

    // Read op_import_reply (device descriptor).
    usbip::op_import_reply devReply{};
    if (tcp.read(reinterpret_cast<char *>(&devReply), sizeof(devReply))
            != static_cast<qint64>(sizeof(devReply)))
        return 0;

    // Attempt to read the optional udp_accept_reply extension.
    // A legacy server will have closed the connection or sent nothing extra.
    if (!tcp.waitForReadyRead(1000))
        return 0; // server did not offer UDP — fall back silently

    udp_accept_reply udpReply{};
    if (tcp.read(reinterpret_cast<char *>(&udpReply), sizeof(udpReply))
            != static_cast<qint64>(sizeof(udpReply)))
        return 0;

    if (ntohl(udpReply.magic) != UDP_ACCEPT_MAGIC)
        return 0;

    return ntohs(udpReply.udp_port);
}

void HybridUdpTransport::startWorker(const QString &host, quint16 udpPort)
{
    m_thread = new QThread();
    m_worker = new HybridUdpWorker();
    m_worker->setTarget(host, udpPort);
    m_worker->moveToThread(m_thread);

    QObject::connect(m_thread, &QThread::started,
                     m_worker, &HybridUdpWorker::run);

    // Worker signals session errors; log them via Qt's debug channel since
    // HybridUdpTransport has no direct reference to LogWindow.
    QObject::connect(m_worker, &HybridUdpWorker::sessionError,
                     m_worker, [](const QString &msg) {
                         qWarning("[HybridUdpTransport] %s", qPrintable(msg));
                     });

    QObject::connect(m_thread, &QThread::finished,
                     m_worker, &QObject::deleteLater);
    QObject::connect(m_thread, &QThread::finished,
                     m_thread, &QObject::deleteLater);

    m_thread->start();
}

void HybridUdpTransport::stopWorker()
{
    if (!m_thread)
        return;

    if (m_worker)
        m_worker->requestStop();

    m_thread->quit();
    m_thread->wait(3000);

    // deleteLater connections handle cleanup; clear our pointers.
    m_worker    = nullptr;
    m_thread    = nullptr;
    m_udpActive = false;
}

} // namespace usbip::transport
