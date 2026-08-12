#pragma once

#include "ITransport.h"

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

    /// Aborts any pending reconnect and tries again immediately.
    void reconnectNow();

protected:
    [[nodiscard]] core::Result<void> openImpl(const TransportConfig& config) override;
    void closeImpl() override;

private:
    void startConnection();
    void onConnected();
    void onDisconnected();
    void scheduleReconnect(const QString& reason);

    QTcpSocket* socket_{nullptr};
    QTimer* reconnectTimer_{nullptr};
    LinkId currentLinkId_{kInvalidLinkId};
    bool autoReconnect_{true};
    int reconnectIntervalMs_{2000};
};

} // namespace hwsim::transport
