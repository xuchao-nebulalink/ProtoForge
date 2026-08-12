#pragma once

#include "ILink.h"
#include "ITransport.h"

#include <QPointer>

namespace hwsim::transport {

class LoopbackTransport;

class HWSIM_TRANSPORT_API LoopbackLink final : public ILink {
    Q_OBJECT

public:
    LoopbackLink(LinkId id, LoopbackTransport* owner, QString peerName, QObject* parent = nullptr);

    [[nodiscard]] QString peerDescription() const override;

    void deliver(const QByteArray& data);
    void markConnected();

protected:
    qint64 writeBytes(std::span<const std::byte> data) override;
    void closeImpl() override;

private:
    QPointer<LoopbackTransport> owner_;
    QString peerName_;
};

/// Two endpoints wired together in memory.
///
/// This is the transport that makes the stack testable. An integration test
/// runs a simulated device on one side and a protocol master on the other, and
/// exercises framing, dispatch, the device model and fault injection end to end
/// without opening a socket, binding a port or needing a serial adapter.
class HWSIM_TRANSPORT_API LoopbackTransport final : public ITransport {
    Q_OBJECT

public:
    explicit LoopbackTransport(QObject* parent = nullptr);
    ~LoopbackTransport() override;

    [[nodiscard]] TransportKind kind() const noexcept override { return TransportKind::Loopback; }

    /// Wires two open endpoints together and raises a link on each side.
    /// Delivery always goes through the event loop, so a handler that replies
    /// while processing a frame does not recurse.
    ///
    /// Both endpoints must live on the same thread when this is called: it
    /// creates a link on each side, and a QObject must be constructed on the
    /// thread that will own it. Once paired, the two halves may be moved to
    /// separate threads; transmitToPeer() hops threads safely from then on.
    static void connectPair(LoopbackTransport* first, LoopbackTransport* second);

    void disconnectPeer(const QString& reason = QStringLiteral("peer detached"));

    [[nodiscard]] LoopbackTransport* peer() const { return peer_.data(); }

    void setLatencyMs(int milliseconds) { latencyMs_ = milliseconds; }
    [[nodiscard]] int latencyMs() const noexcept { return latencyMs_; }

protected:
    [[nodiscard]] core::Result<void> openImpl(const TransportConfig& config) override;
    void closeImpl() override;

private:
    friend class LoopbackLink;

    /// Called by our link when the local side writes.
    qint64 transmitToPeer(const QByteArray& data);

    /// Called by the peer transport when it transmits.
    void receiveFromPeer(const QByteArray& data);

    QPointer<LoopbackTransport> peer_;
    LinkId localLinkId_{kInvalidLinkId};
    int latencyMs_{0};
};

} // namespace hwsim::transport
