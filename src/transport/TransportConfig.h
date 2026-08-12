#pragma once

#include "TransportTypes.h"

#include <core/ConfigSchema.h>
#include <core/Result.h>

#include <QJsonObject>
#include <QVariantMap>

namespace hwsim::transport {

/// Configuration keys. Kept as named constants because they travel through
/// QVariantMap, JSON profiles and the generated UI, so a typo would otherwise
/// only surface at run time.
namespace keys {
inline constexpr auto kHost = "host";
inline constexpr auto kPort = "port";
inline constexpr auto kBindAddress = "bindAddress";
inline constexpr auto kLocalPort = "localPort";
inline constexpr auto kMaxConnections = "maxConnections";
inline constexpr auto kAutoReconnect = "autoReconnect";
inline constexpr auto kReconnectIntervalMs = "reconnectIntervalMs";
inline constexpr auto kConnectTimeoutMs = "connectTimeoutMs";

inline constexpr auto kPeerMode = "peerMode";       // "fixed" | "discover"
inline constexpr auto kRemoteHost = "remoteHost";
inline constexpr auto kRemotePort = "remotePort";

inline constexpr auto kPortName = "portName";
inline constexpr auto kBaudRate = "baudRate";
inline constexpr auto kDataBits = "dataBits";
inline constexpr auto kParity = "parity";
inline constexpr auto kStopBits = "stopBits";
inline constexpr auto kFlowControl = "flowControl";
inline constexpr auto kInterFrameGapMs = "interFrameGapMs";

inline constexpr auto kLatencyMs = "latencyMs";
} // namespace keys

/// Transport settings as an untyped map plus a schema that describes it.
///
/// Untyped storage is deliberate: it lets a profile round-trip through JSON,
/// lets the UI build an editor without knowing the transport type, and lets a
/// new transport add settings without changing this class. Typed accessors
/// cover the common keys so call sites stay readable.
class HWSIM_TRANSPORT_API TransportConfig {
public:
    TransportConfig() = default;
    explicit TransportConfig(TransportKind kind, QVariantMap values = {});

    [[nodiscard]] TransportKind kind() const noexcept { return kind_; }
    void setKind(TransportKind kind) { kind_ = kind; }

    [[nodiscard]] TransportRole role() const noexcept { return role_; }
    void setRole(TransportRole role) { role_ = role; }

    [[nodiscard]] QVariant value(const QString& key, const QVariant& fallback = {}) const;
    void setValue(const QString& key, QVariant value);
    [[nodiscard]] bool contains(const QString& key) const;

    [[nodiscard]] const QVariantMap& values() const noexcept { return values_; }
    void setValues(QVariantMap values) { values_ = std::move(values); }

    [[nodiscard]] QString host() const;
    [[nodiscard]] quint16 port() const;
    [[nodiscard]] QString bindAddress() const;
    [[nodiscard]] quint16 localPort() const;
    [[nodiscard]] QString portName() const;
    [[nodiscard]] qint32 baudRate() const;
    [[nodiscard]] bool autoReconnect() const;
    [[nodiscard]] int reconnectIntervalMs() const;

    /// Applies schema defaults and clamps out-of-range numbers.
    void applyDefaults();
    [[nodiscard]] core::Result<void> validate() const;

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static core::Result<TransportConfig> fromJson(const QJsonObject& json);

    /// Human readable one-liner for the device tree, e.g. "TCP 服务端 0.0.0.0:502".
    [[nodiscard]] QString describe() const;

    [[nodiscard]] static core::ConfigSchema schemaFor(TransportKind kind);

private:
    TransportKind kind_{TransportKind::TcpServer};
    TransportRole role_{TransportRole::Responder};
    QVariantMap values_;
};

} // namespace hwsim::transport
