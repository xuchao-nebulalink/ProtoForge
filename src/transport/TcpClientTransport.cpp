#include "TcpClientTransport.h"

#include "StreamLink.h"

#include <core/Logger.h>

#include <QTcpSocket>
#include <QTimer>

namespace {
constexpr auto kLogCategory = "transport.tcp.client";
}

namespace hwsim::transport {

TcpClientTransport::TcpClientTransport(QObject* parent) : ITransport(parent) {}

TcpClientTransport::~TcpClientTransport()
{
    closeImpl();
}

core::Result<void> TcpClientTransport::openImpl(const TransportConfig& config)
{
    autoReconnect_ = config.autoReconnect();
    reconnectIntervalMs_ = config.reconnectIntervalMs();

    reconnectTimer_ = new QTimer(this);
    reconnectTimer_->setSingleShot(true);
    connect(reconnectTimer_, &QTimer::timeout, this, &TcpClientTransport::startConnection);

    startConnection();
    return core::success();
}

void TcpClientTransport::closeImpl()
{
    if (reconnectTimer_ != nullptr) {
        reconnectTimer_->stop();
        reconnectTimer_->deleteLater();
        reconnectTimer_ = nullptr;
    }
    // QPointer already nulls itself if the link destroyed the socket, so this
    // only runs while the socket really is still alive.
    if (!socket_.isNull()) {
        // Detach before letting go. A socket that outlives this close, because
        // its StreamLink owns it, would otherwise still be wired to onConnected
        // and could raise a link on a stale connection after a later open().
        disconnect(socket_, nullptr, this, nullptr);

        // The socket is owned by its StreamLink once the link exists; only
        // clean it up here if the connection never completed.
        if (currentLinkId_ == kInvalidLinkId) {
            socket_->abort();
            socket_->deleteLater();
        }
    }
    socket_ = nullptr;
    currentLinkId_ = kInvalidLinkId;
}

void TcpClientTransport::reconnectNow()
{
    if (reconnectTimer_ != nullptr) {
        reconnectTimer_->stop();
    }

    // Abandon an attempt that is still in ConnectingState, otherwise
    // startConnection() sees a live socket_ and returns without doing anything,
    // which makes this function a no-op exactly when it is most wanted.
    if (!socket_.isNull() && currentLinkId_ == kInvalidLinkId) {
        disconnect(socket_, nullptr, this, nullptr);
        socket_->abort();
        socket_->deleteLater();
        socket_ = nullptr;
    }

    startConnection();
}

void TcpClientTransport::startConnection()
{
    if (currentLinkId_ != kInvalidLinkId || !socket_.isNull()) {
        return;
    }

    socket_ = new QTcpSocket(this);
    connect(socket_, &QTcpSocket::connected, this, &TcpClientTransport::onConnected);

    // The lambda captures its own socket rather than reading the member: a
    // previous socket can still be alive inside a retired StreamLink and emit
    // errorOccurred, and acting on that would tear down the *current* attempt
    // and leave the transport reconnecting forever.
    connect(socket_, &QTcpSocket::errorOccurred, this,
            [this, socket = socket_](QAbstractSocket::SocketError) {
                if (socket.isNull() || socket != socket_
                    || currentLinkId_ != kInvalidLinkId) {
                    return;
                }
                const QString reason = socket->errorString();
                reportError(core::makeError(core::ErrorCode::ConnectionRefused, reason,
                                            config().describe()));
                scheduleReconnect(reason);
            });

    HWSIM_LOG_DEBUG(kLogCategory) << "connecting to " << config().host() << ':' << config().port();
    socket_->connectToHost(config().host(), config().port());
}

void TcpClientTransport::onConnected()
{
    if (socket_.isNull()) {
        return;
    }

    const QString peer =
        QStringLiteral("%1:%2").arg(socket_->peerAddress().toString()).arg(socket_->peerPort());

    auto link = std::make_unique<StreamLink>(ILink::allocateId(), socket_.data(), peer);
    StreamLink* raw = link.get();
    currentLinkId_ = raw->id();

    connect(socket_, &QTcpSocket::disconnected, this, &TcpClientTransport::onDisconnected);

    addLink(std::move(link));
    raw->markConnected();

    HWSIM_LOG_INFO(kLogCategory) << "connected to " << peer;
}

void TcpClientTransport::onDisconnected()
{
    const LinkId id = currentLinkId_;
    currentLinkId_ = kInvalidLinkId;
    socket_ = nullptr;

    if (id != kInvalidLinkId) {
        removeLink(id, QStringLiteral("peer disconnected"));
    }
    scheduleReconnect(QStringLiteral("peer disconnected"));
}

void TcpClientTransport::scheduleReconnect(const QString& reason)
{
    if (!autoReconnect_ || !isOpen() || reconnectTimer_ == nullptr) {
        return;
    }
    if (reconnectTimer_->isActive()) {
        return;
    }

    // A failed socket cannot be reused; drop it so startConnection makes a new one.
    if (!socket_.isNull() && currentLinkId_ == kInvalidLinkId) {
        disconnect(socket_, nullptr, this, nullptr);
        socket_->deleteLater();
        socket_ = nullptr;
    }

    HWSIM_LOG_DEBUG(kLogCategory) << "retrying in " << reconnectIntervalMs_ << " ms (" << reason << ')';
    reconnectTimer_->start(reconnectIntervalMs_);
}

} // namespace hwsim::transport
