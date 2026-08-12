#include "TransportTypes.h"

#include <QHash>

using hwsim::core::ErrorCode;
using hwsim::core::makeError;
using hwsim::core::Result;

namespace hwsim::transport {
namespace {

const QHash<TransportKind, QString>& kindNames()
{
    static const QHash<TransportKind, QString> names{
        {TransportKind::TcpServer, QStringLiteral("tcp-server")},
        {TransportKind::TcpClient, QStringLiteral("tcp-client")},
        {TransportKind::Udp, QStringLiteral("udp")},
        {TransportKind::Serial, QStringLiteral("serial")},
        {TransportKind::Loopback, QStringLiteral("loopback")},
    };
    return names;
}

} // namespace

void LinkStatistics::reset()
{
    *this = LinkStatistics{};
}

QString transportKindName(TransportKind kind)
{
    return kindNames().value(kind, QStringLiteral("unknown"));
}

QString transportKindDisplayName(TransportKind kind)
{
    static const QHash<TransportKind, QString> names{
        {TransportKind::TcpServer, QStringLiteral("TCP 服务端")},
        {TransportKind::TcpClient, QStringLiteral("TCP 客户端")},
        {TransportKind::Udp, QStringLiteral("UDP")},
        {TransportKind::Serial, QStringLiteral("串口")},
        {TransportKind::Loopback, QStringLiteral("内存回环")},
    };
    return names.value(kind, QStringLiteral("未知"));
}

Result<TransportKind> transportKindFromName(const QString& name)
{
    const QString normalised = name.trimmed().toLower();
    for (auto it = kindNames().constBegin(); it != kindNames().constEnd(); ++it) {
        if (it.value() == normalised) {
            return it.key();
        }
    }
    return makeError(ErrorCode::NotFound,
                     QStringLiteral("unknown transport kind '%1'").arg(name));
}

QString linkStateName(LinkState state)
{
    static const QHash<LinkState, QString> names{
        {LinkState::Disconnected, QStringLiteral("Disconnected")},
        {LinkState::Connecting, QStringLiteral("Connecting")},
        {LinkState::Connected, QStringLiteral("Connected")},
        {LinkState::Closing, QStringLiteral("Closing")},
        {LinkState::Faulted, QStringLiteral("Faulted")},
    };
    return names.value(state, QStringLiteral("Unknown"));
}

QString transportRoleName(TransportRole role)
{
    return role == TransportRole::Initiator ? QStringLiteral("Initiator")
                                            : QStringLiteral("Responder");
}

void registerTransportMetaTypes()
{
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;

    // No string aliases: that overload is deprecated in Qt 6 (moc records the
    // parameter types itself through QMetaType::fromType), and it warns.
    // LinkId is a quint64 and is already a built-in metatype.
    qRegisterMetaType<TransportKind>();
    qRegisterMetaType<TransportRole>();
    qRegisterMetaType<LinkState>();
    qRegisterMetaType<LinkStatistics>();
    qRegisterMetaType<core::Error>();
}

} // namespace hwsim::transport
