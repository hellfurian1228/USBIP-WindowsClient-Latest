/*
 * hybrid_udp_transport.h
 *
 * HybridUdpTransport: performs the standard OP_REQ_IMPORT handshake over TCP,
 * appends FLAG_REQUEST_UDP to signal UDP capability, then switches the data
 * plane to a QUdpSocket if the host acknowledges with a UDP port.
 *
 * URB frames are wrapped in a 4-byte sequence-number header. A sequence mismatch
 * closes the session rather than hanging the UI. All socket I/O runs on a
 * dedicated QThread so the main GUI thread is never blocked.
 */

#pragma once

#include "usb_transport.h"

#include <QObject>
#include <QThread>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QMutex>
#include <QWaitCondition>

#include <atomic>
#include <cstdint>
#include <span>
#include <string>

namespace usbip::transport
{

// ── Wire format ──────────────────────────────────────────────────────────────

// Extended OP_REQ_IMPORT header appended after the standard op_import_request.
// The server ignores unknown trailing bytes, so this is backward-compatible.
#pragma pack(push, 1)
struct udp_request_extension
{
    uint32_t flags;         // FLAG_REQUEST_UDP when set
    uint16_t client_port;   // hint: preferred local UDP port (0 = any)
    uint16_t _reserved;
};

// Server reply when it accepts UDP: sent immediately after op_import_reply.
struct udp_accept_reply
{
    uint32_t magic;         // UDP_ACCEPT_MAGIC when valid
    uint16_t udp_port;      // server-side UDP port to send/receive on
    uint16_t _reserved;
};

// Prepended to every URB datagram.
struct udp_urb_header
{
    uint32_t sequence_id;   // monotonically increasing, network byte order
};
#pragma pack(pop)

constexpr uint32_t FLAG_REQUEST_UDP = 0x00000001u;
constexpr uint32_t UDP_ACCEPT_MAGIC = 0x55445041u; // "UDPA"
constexpr int      USBIP_TCP_PORT   = 3240;
constexpr int      UDP_RECV_TIMEOUT_MS = 5000;

// ── Worker (lives on its own QThread) ────────────────────────────────────────

class HybridUdpWorker : public QObject
{
    Q_OBJECT

public:
    explicit HybridUdpWorker(QObject *parent = nullptr);
    ~HybridUdpWorker() override;

    // Called from the transport thread before the worker thread starts.
    void setTarget(const QString &host, quint16 udpPort);

    // Thread-safe send: enqueues a datagram with the next sequence id.
    bool sendUrb(std::span<const uint8_t> data);

    // Thread-safe receive: blocks until a datagram arrives or timeout.
    bool receiveUrb(std::span<uint8_t> buffer);

    void requestStop();

signals:
    void sessionError(const QString &reason);

public slots:
    void run();

private:
    void handleIncomingDatagram();
    void closeSocket();

    QString  m_host;
    quint16  m_udpPort = 0;

    QUdpSocket          *m_socket = nullptr;
    std::atomic<bool>    m_stop{false};
    std::atomic<uint32_t> m_nextSendSeq{1};
    uint32_t             m_nextRecvSeq = 1;

    // Pending received payload (one datagram at a time).
    QMutex         m_recvMutex;
    QWaitCondition m_recvCond;
    QByteArray     m_recvBuffer;
    bool           m_recvReady = false;
    bool           m_recvError = false;
};

// ── Public transport class ───────────────────────────────────────────────────

class HybridUdpTransport : public IUsbTransport
{
public:
    HybridUdpTransport();
    ~HybridUdpTransport() override;

    // Performs TCP handshake + optional UDP upgrade, then calls vhci::attach().
    int  connect(HANDLE dev, const usbip::device_location &location) override;

    bool sendUrb(std::span<const uint8_t> data) override;
    bool receiveUrb(std::span<uint8_t> buffer) override;

    void disconnect(HANDLE dev, int hubPort) override;

private:
    // Returns the UDP port offered by the server, or 0 if the server declined.
    quint16 negotiateUdp(const QString &host, quint16 tcpPort,
                         const std::string &busid);

    void startWorker(const QString &host, quint16 udpPort);
    void stopWorker();

    HybridUdpWorker *m_worker = nullptr;
    QThread         *m_thread = nullptr;
    bool             m_udpActive = false;
};

} // namespace usbip::transport
