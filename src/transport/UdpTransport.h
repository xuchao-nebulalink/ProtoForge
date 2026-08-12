#pragma once

#include "ILink.h"
#include "ITransport.h"

#include <QHash>
#include <QHostAddress>
#include <QPointer>

class QUdpSocket;

namespace hwsim::transport {

/// UDP has no connections, so a link is synthesised per remote address.
///
/// This is what allows the layers above to stay transport-agnostic: a protocol
/// session gets its own framing buffer and its own device state per peer,
/// exactly as it would over TCP, even though everything shares one socket.
class HWSIM_TRANSPORT_API UdpLink final : public ILink {
    Q_OBJECT

public:
    UdpLink(LinkId id, QUdpSocket* socket, QHostAddress peerAddress, quint16 peerPort,
            QObject* parent = nullptr);

    [[nodiscard]] QString peerDescription() const override;

    [[nodiscard]] QHostAddress peerAddress() const { return address_; }
    [[nodiscard]] quint16 peerPort() const noexcept { return port_; }

    /// Called by the transport when a datagram from this peer arrives.
    void deliver(const QByteArray& datagram);

    void markConnected();

protected:
    qint64 writeBytes(std::span<const std::byte> data) override;
    void closeImpl() override;

private:
    QPointer<QUdpSocket> socket_;
    QHostAddress address_;
    quint16 port_;
};

class HWSIM_TRANSPORT_API UdpTransport final : public ITransport {
    Q_OBJECT

public:
    explicit UdpTransport(QObject* parent = nullptr);
    ~UdpTransport() override;

    [[nodiscard]] TransportKind kind() const noexcept override { return TransportKind::Udp; }

    [[nodiscard]] quint16 boundPort() const;

    /// In discover mode, drops the link for a peer that has gone quiet.
    void forgetPeer(const QHostAddress& address, quint16 port);

protected:
    [[nodiscard]] core::Result<void> openImpl(const TransportConfig& config) override;
    void closeImpl() override;

private:
    void onDatagramReady();
    UdpLink* linkForPeer(const QHostAddress& address, quint16 port);

    [[nodiscard]] static QString peerKey(const QHostAddress& address, quint16 port);

    QUdpSocket* socket_{nullptr};
    QHash<QString, LinkId> peerLinks_;
    bool discoverPeers_{true};
    int maxPeers_{64};
};

} // namespace hwsim::transport
