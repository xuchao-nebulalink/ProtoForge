#include "UdpTransport.h"

#include <core/Logger.h>

#include <QNetworkDatagram>
#include <QUdpSocket>

namespace {
constexpr auto kLogCategory = "transport.udp";
}

namespace hwsim::transport {

// --- UdpLink ---------------------------------------------------------------

UdpLink::UdpLink(LinkId id, QUdpSocket* socket, QHostAddress peerAddress, quint16 peerPort,
                 QObject* parent)
    : ILink(id, parent), socket_(socket), address_(std::move(peerAddress)), port_(peerPort)
{
}

QString UdpLink::peerDescription() const
{
    return QStringLiteral("%1:%2").arg(address_.toString()).arg(port_);
}

void UdpLink::deliver(const QByteArray& datagram)
{
    handleIncoming(datagram);
}

void UdpLink::markConnected()
{
    setState(LinkState::Connected);
}

qint64 UdpLink::writeBytes(std::span<const std::byte> data)
{
    if (socket_.isNull()) {
        reportError(core::makeError(core::ErrorCode::NotConnected,
                                    QStringLiteral("socket is gone"), peerDescription()));
        return -1;
    }

    const qint64 written = socket_->writeDatagram(reinterpret_cast<const char*>(data.data()),
                                                  static_cast<qint64>(data.size()), address_, port_);
    if (written < 0) {
        reportError(core::makeError(core::ErrorCode::IoError, socket_->errorString(),
                                    peerDescription()));
    }
    return written;
}

void UdpLink::closeImpl()
{
    // Nothing to tear down: the socket belongs to the transport and is shared
    // by every peer link.
}

// --- UdpTransport ----------------------------------------------------------

UdpTransport::UdpTransport(QObject* parent) : ITransport(parent) {}

UdpTransport::~UdpTransport()
{
    closeImpl();
}

quint16 UdpTransport::boundPort() const
{
    return socket_ != nullptr ? socket_->localPort() : 0;
}

core::Result<void> UdpTransport::openImpl(const TransportConfig& config)
{
    discoverPeers_ = config.value(QStringLiteral("peerMode"), QStringLiteral("discover")).toString()
                     != QStringLiteral("fixed");
    // Clamped rather than trusted: the schema declares a range, but a config
    // built in code bypasses normalise(), and a zero here would make the
    // discover-mode ceiling reject every peer.
    maxPeers_ = qMax(1, config.value(QStringLiteral("maxConnections"), 64).toInt());

    // Validate the fixed peer before binding. ITransport::close() is a no-op on
    // a transport that never reported itself open, so returning an error after
    // a successful bind would strand the socket holding the port until the
    // transport is destroyed.
    QHostAddress remote;
    quint16 remotePort = 0;
    if (!discoverPeers_) {
        remote = QHostAddress(config.value(QStringLiteral("remoteHost")).toString());
        remotePort = static_cast<quint16>(config.value(QStringLiteral("remotePort"), 502).toUInt());
        if (remote.isNull()) {
            return core::makeError(core::ErrorCode::ConfigInvalid,
                                   QStringLiteral("fixed peer mode requires a valid remoteHost"));
        }
    }

    socket_ = new QUdpSocket(this);
    connect(socket_, &QUdpSocket::readyRead, this, &UdpTransport::onDatagramReady);

    const QHostAddress bindAddress(config.bindAddress());
    const QUdpSocket::BindMode mode = QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint;
    if (!socket_->bind(bindAddress, config.localPort(), mode)) {
        const QString reason = socket_->errorString();
        socket_->deleteLater();
        socket_ = nullptr;
        return core::makeError(core::ErrorCode::AddressInUse, reason,
                               QStringLiteral("%1:%2").arg(config.bindAddress())
                                   .arg(config.localPort()));
    }

    HWSIM_LOG_INFO(kLogCategory) << "bound to " << socket_->localAddress().toString() << ':'
                                 << socket_->localPort();

    // A fixed peer gets its link up front so the protocol layer can start
    // sending without waiting for the peer to speak first.
    if (!discoverPeers_) {
        linkForPeer(remote, remotePort);
    }

    return core::success();
}

void UdpTransport::closeImpl()
{
    peerLinks_.clear();
    if (socket_ == nullptr) {
        return;
    }
    socket_->close();
    socket_->deleteLater();
    socket_ = nullptr;
}

void UdpTransport::forgetPeer(const QHostAddress& address, quint16 port)
{
    const auto it = peerLinks_.constFind(peerKey(address, port));
    if (it == peerLinks_.constEnd()) {
        return;
    }
    const LinkId id = it.value();
    peerLinks_.erase(it);
    removeLink(id, QStringLiteral("peer forgotten"));
}

void UdpTransport::onDatagramReady()
{
    while (socket_ != nullptr && socket_->hasPendingDatagrams()) {
        const QNetworkDatagram datagram = socket_->receiveDatagram();
        if (!datagram.isValid()) {
            continue;
        }

        UdpLink* link = linkForPeer(datagram.senderAddress(), static_cast<quint16>(datagram.senderPort()));
        if (link == nullptr) {
            HWSIM_LOG_DEBUG(kLogCategory)
                << "dropping datagram from unexpected peer "
                << datagram.senderAddress().toString() << ':' << datagram.senderPort();
            continue;
        }
        link->deliver(datagram.data());
    }
}

UdpLink* UdpTransport::linkForPeer(const QHostAddress& address, quint16 port)
{
    const QString key = peerKey(address, port);

    if (const auto it = peerLinks_.constFind(key); it != peerLinks_.constEnd()) {
        return qobject_cast<UdpLink*>(findLink(it.value()));
    }
    if (!discoverPeers_ && !peerLinks_.isEmpty()) {
        return nullptr;
    }

    // Without a ceiling, a port scan or spoofed source addresses would create
    // an unbounded number of links, since UDP has no connection teardown to
    // reclaim them. forgetPeer() exists for deliberate cleanup.
    if (discoverPeers_ && linkCount() >= maxPeers_) {
        HWSIM_LOG_WARNING(kLogCategory)
            << "peer limit " << maxPeers_ << " reached, ignoring " << key;
        return nullptr;
    }

    auto link = std::make_unique<UdpLink>(ILink::allocateId(), socket_, address, port);
    UdpLink* raw = link.get();
    peerLinks_.insert(key, raw->id());

    addLink(std::move(link));
    raw->markConnected();
    return raw;
}

QString UdpTransport::peerKey(const QHostAddress& address, quint16 port)
{
    return QStringLiteral("%1|%2").arg(address.toString()).arg(port);
}

} // namespace hwsim::transport
