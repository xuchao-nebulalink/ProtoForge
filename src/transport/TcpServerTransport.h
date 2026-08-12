#pragma once

#include "ITransport.h"

class QTcpServer;
class QTcpSocket;

namespace hwsim::transport {

/// Listening endpoint. Every accepted connection becomes its own link, so a
/// simulated device can serve several masters at once and the protocol layer
/// gets an independent session (and independent framing buffer) for each.
class HWSIM_TRANSPORT_API TcpServerTransport final : public ITransport {
    Q_OBJECT

public:
    explicit TcpServerTransport(QObject* parent = nullptr);
    ~TcpServerTransport() override;

    [[nodiscard]] TransportKind kind() const noexcept override { return TransportKind::TcpServer; }

    /// The port actually bound. Differs from the configured value when the
    /// configuration asked for port 0, which tests use to avoid collisions.
    [[nodiscard]] quint16 boundPort() const;

protected:
    [[nodiscard]] core::Result<void> openImpl(const TransportConfig& config) override;
    void closeImpl() override;

private:
    void onNewConnection();
    void attachSocket(QTcpSocket* socket);

    QTcpServer* server_{nullptr};
    int maxConnections_{16};
};

} // namespace hwsim::transport
