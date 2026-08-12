#pragma once

#include "TransportTypes.h"

#include <core/Result.h>

#include <QByteArray>
#include <QObject>

#include <cstddef>
#include <span>

namespace hwsim::transport {

/// One conversation with one peer.
///
/// A transport is an endpoint; a link is a session on that endpoint. The split
/// is what lets a single abstraction cover all four transports:
///
///   - TCP server : one endpoint, one link per accepted connection
///   - TCP client : one endpoint, one link
///   - UDP        : one endpoint, one link synthesised per remote address
///   - serial     : one endpoint, one link
///
/// Everything above this layer is written against ILink and therefore does not
/// care which of those it is talking to.
///
/// Threading: a link belongs to the thread of its transport. The Qt signals
/// below are meant for same-thread wiring (the protocol session attaches with
/// direct connections). Cross-thread observers such as the UI subscribe to the
/// value-typed events in TransportEvents.h instead.
class HWSIM_TRANSPORT_API ILink : public QObject {
    Q_OBJECT

public:
    ~ILink() override;

    [[nodiscard]] LinkId id() const noexcept { return id_; }
    [[nodiscard]] LinkState state() const noexcept { return state_; }
    [[nodiscard]] bool isOpen() const noexcept { return state_ == LinkState::Connected; }

    /// Something a human can identify the peer by, e.g. "192.168.1.20:49312".
    [[nodiscard]] virtual QString peerDescription() const = 0;
    [[nodiscard]] virtual QString localDescription() const;

    /// Returns the number of bytes accepted, or -1 on failure.
    qint64 send(std::span<const std::byte> data);
    qint64 send(const QByteArray& data);

    void close();

    [[nodiscard]] const LinkStatistics& statistics() const noexcept { return statistics_; }
    void resetStatistics();

    /// Frame counters are owned by the protocol layer, which is the only place
    /// that knows where one message ends and the next begins.
    void noteFrameReceived() noexcept;
    void noteFrameSent() noexcept;

    [[nodiscard]] static LinkId allocateId() noexcept;

signals:
    void bytesReceived(hwsim::transport::LinkId id, const QByteArray& data);
    void bytesSent(hwsim::transport::LinkId id, const QByteArray& data);
    void stateChanged(hwsim::transport::LinkId id, hwsim::transport::LinkState state);
    void errorOccurred(hwsim::transport::LinkId id, const hwsim::core::Error& error);

protected:
    explicit ILink(LinkId id, QObject* parent = nullptr);

    virtual qint64 writeBytes(std::span<const std::byte> data) = 0;
    virtual void closeImpl() = 0;

    void setState(LinkState state);
    void handleIncoming(const QByteArray& data);
    void reportError(core::Error error);

private:
    LinkId id_;
    LinkState state_{LinkState::Disconnected};
    LinkStatistics statistics_;
};

} // namespace hwsim::transport
