#pragma once

#include "TransportGlobal.h"

#include <core/Result.h>

#include <QMetaType>
#include <QString>

namespace hwsim::transport {

using LinkId = quint64;
inline constexpr LinkId kInvalidLinkId = 0;

/// Concrete endpoint flavours. The framework never switches on this: it is used
/// for factory lookup, schema selection and display only.
enum class TransportKind {
    TcpServer,
    TcpClient,
    Udp,
    Serial,
    Loopback,
};

/// Which side of the conversation this endpoint plays.
///
/// The platform is symmetric: the same protocol plugin drives a simulated
/// device (Responder, waiting for requests) and a test master (Initiator,
/// issuing them). Role is a property of the endpoint, not of the protocol.
enum class TransportRole {
    Responder,
    Initiator,
};

enum class LinkState {
    Disconnected,
    Connecting,
    Connected,
    Closing,
    Faulted,
};

/// Per-link counters. Frame counts are maintained by the protocol layer, which
/// is the only layer that knows where a frame ends.
struct HWSIM_TRANSPORT_API LinkStatistics {
    quint64 bytesReceived{0};
    quint64 bytesSent{0};
    quint64 framesReceived{0};
    quint64 framesSent{0};
    quint64 errorCount{0};
    qint64 openedAtMs{0};
    qint64 lastActivityMs{0};

    void reset();
};

[[nodiscard]] HWSIM_TRANSPORT_API QString transportKindName(TransportKind kind);
[[nodiscard]] HWSIM_TRANSPORT_API QString transportKindDisplayName(TransportKind kind);
[[nodiscard]] HWSIM_TRANSPORT_API core::Result<TransportKind> transportKindFromName(const QString& name);
[[nodiscard]] HWSIM_TRANSPORT_API QString linkStateName(LinkState state);
[[nodiscard]] HWSIM_TRANSPORT_API QString transportRoleName(TransportRole role);

/// Registers the enum and struct types used in queued signal connections.
/// Called once by TransportFactory; safe to call repeatedly.
HWSIM_TRANSPORT_API void registerTransportMetaTypes();

} // namespace hwsim::transport

Q_DECLARE_METATYPE(hwsim::transport::TransportKind)
Q_DECLARE_METATYPE(hwsim::transport::TransportRole)
Q_DECLARE_METATYPE(hwsim::transport::LinkState)
Q_DECLARE_METATYPE(hwsim::transport::LinkStatistics)
Q_DECLARE_METATYPE(hwsim::core::Error)
