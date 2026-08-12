#include "TcpServerTransport.h"

#include "StreamLink.h"

#include <core/Logger.h>

#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>

namespace {
constexpr auto kLogCategory = "transport.tcp.server";
}

namespace hwsim::transport {

TcpServerTransport::TcpServerTransport(QObject* parent) : ITransport(parent) {}

TcpServerTransport::~TcpServerTransport()
{
    closeImpl();
}

quint16 TcpServerTransport::boundPort() const
{
    return server_ != nullptr ? server_->serverPort() : 0;
}

core::Result<void> TcpServerTransport::openImpl(const TransportConfig& config)
{
    maxConnections_ = config.value(QStringLiteral("maxConnections"), 16).toInt();

    server_ = new QTcpServer(this);
    connect(server_, &QTcpServer::newConnection, this, &TcpServerTransport::onNewConnection);

    const QHostAddress address(config.bindAddress());
    if (!server_->listen(address, config.port())) {
        const QString reason = server_->errorString();
        server_->deleteLater();
        server_ = nullptr;
        return core::makeError(core::ErrorCode::AddressInUse, reason,
                               QStringLiteral("%1:%2").arg(config.bindAddress()).arg(config.port()));
    }

    HWSIM_LOG_INFO(kLogCategory) << "listening on " << server_->serverAddress().toString() << ':'
                                 << server_->serverPort();
    return core::success();
}

void TcpServerTransport::closeImpl()
{
    if (server_ == nullptr) {
        return;
    }
    server_->close();
    server_->deleteLater();
    server_ = nullptr;
}

void TcpServerTransport::onNewConnection()
{
    while (server_ != nullptr && server_->hasPendingConnections()) {
        QTcpSocket* socket = server_->nextPendingConnection();
        if (socket == nullptr) {
            break;
        }

        if (linkCount() >= maxConnections_) {
            HWSIM_LOG_WARNING(kLogCategory)
                << "rejecting " << socket->peerAddress().toString()
                << ": connection limit " << maxConnections_ << " reached";
            socket->abort();
            socket->deleteLater();
            continue;
        }

        attachSocket(socket);
    }
}

void TcpServerTransport::attachSocket(QTcpSocket* socket)
{
    const QString peer =
        QStringLiteral("%1:%2").arg(socket->peerAddress().toString()).arg(socket->peerPort());

    auto link = std::make_unique<StreamLink>(ILink::allocateId(), socket, peer);
    StreamLink* raw = link.get();
    const LinkId id = raw->id();

    connect(socket, &QTcpSocket::disconnected, this,
            [this, id] { removeLink(id, QStringLiteral("peer disconnected")); });
    connect(socket, &QTcpSocket::errorOccurred, this,
            [this, id, socket](QAbstractSocket::SocketError) {
                removeLink(id, socket->errorString());
            });

    addLink(std::move(link));
    raw->markConnected();
}

} // namespace hwsim::transport
