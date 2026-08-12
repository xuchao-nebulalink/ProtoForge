#pragma once

#include "ITransport.h"

#include <QPointer>

class QTcpSocket;
class QTimer;

namespace hwsim::transport {

/// Outgoing connection, used when the platform drives a real device or when an
/// automated test acts as the master against a simulated one.
///
/// open() returns as soon as the connection attempt starts; the link appears
/// once the socket is actually connected. That keeps the lifecycle identical to
/// the server case, where links also arrive asynchronously.
class HWSIM_TRANSPORT_API TcpClientTransport final : public ITransport {
    Q_OBJECT

public:
    explicit TcpClientTransport(QObject* parent = nullptr);
    ~TcpClientTransport() override;

    [[nodiscard]] TransportKind kind() const noexcept override { return TransportKind::TcpClient; }

    /// Cancels the reconnect timer, abandons a connection attempt still in
    /// progress, and dials again immediately. Does nothing when a link is
    /// already up: use close() first to drop it.
    ///
    /// Provided for the UI's "reconnect" action; nothing in the framework calls it.
    void reconnectNow();

protected:
    [[nodiscard]] core::Result<void> openImpl(const TransportConfig& config) override;
    void closeImpl() override;

private:
    void startConnection();
    void onConnected();
    void onDisconnected();
    void scheduleReconnect(const QString& reason);

    /// QPointer, not a raw pointer: once a connection succeeds the socket is
    /// owned by its StreamLink, and ITransport::close() destroys the links
    /// before it calls closeImpl(). A raw pointer would be dangling there.
    QPointer<QTcpSocket> socket_;

    QTimer* reconnectTimer_{nullptr};
    LinkId currentLinkId_{kInvalidLinkId};
    bool autoReconnect_{true};
    int reconnectIntervalMs_{2000};
};

} // namespace hwsim::transport
