#pragma once

#include "TransportTypes.h"

#include <core/Clock.h>
#include <core/ErrorCode.h>

#include <QByteArray>
#include <QString>

namespace hwsim::transport {

enum class Direction {
    Inbound,
    Outbound,
};

[[nodiscard]] inline QString directionName(Direction direction)
{
    return direction == Direction::Inbound ? QStringLiteral("RX") : QStringLiteral("TX");
}

/// Value-typed notifications published on the core EventBus.
///
/// Qt signals on ITransport/ILink carry pointers and are meant for same-thread
/// wiring inside the I/O thread. These carry copies and are what the UI, the
/// statistics collector and the script runner observe, so nothing outside the
/// I/O thread ever holds a pointer into it.
struct TransportOpenedEvent {
    QString transportName;
    TransportKind kind{TransportKind::TcpServer};
    QString description;
    qint64 timestampMs{core::wallClockMs()};
};

struct TransportClosedEvent {
    QString transportName;
    TransportKind kind{TransportKind::TcpServer};
    qint64 timestampMs{core::wallClockMs()};
};

struct LinkOpenedEvent {
    LinkId linkId{kInvalidLinkId};
    QString transportName;
    QString peerDescription;
    qint64 timestampMs{core::wallClockMs()};
};

struct LinkClosedEvent {
    LinkId linkId{kInvalidLinkId};
    QString transportName;
    QString reason;
    qint64 timestampMs{core::wallClockMs()};
};

struct LinkStateChangedEvent {
    LinkId linkId{kInvalidLinkId};
    QString transportName;
    LinkState state{LinkState::Disconnected};
    qint64 timestampMs{core::wallClockMs()};
};

/// Raw traffic, before any protocol decoding. The packet view renders these in
/// its hex and ascii tabs; the protocol layer publishes decoded frames separately.
struct BytesTransferredEvent {
    LinkId linkId{kInvalidLinkId};
    QString transportName;
    Direction direction{Direction::Inbound};
    QByteArray data;
    qint64 timestampMs{core::wallClockMs()};
};

struct TransportErrorEvent {
    QString transportName;
    LinkId linkId{kInvalidLinkId};
    core::Error error;
    qint64 timestampMs{core::wallClockMs()};
};

} // namespace hwsim::transport
