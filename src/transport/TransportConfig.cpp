#include "TransportConfig.h"

#include <QHash>
#include <QJsonObject>

using hwsim::core::ConfigField;
using hwsim::core::ConfigSchema;
using hwsim::core::Result;

namespace hwsim::transport {
namespace {

/// Keys are stored as plain char literals so they can live in constexpr
/// storage; this turns one into the QString the schema and QVariantMap want.
QString k(const char* key)
{
    return QString::fromLatin1(key);
}

ConfigSchema tcpServerSchema()
{
    ConfigSchema schema(QStringLiteral("TCP 服务端"));
    schema.add(ConfigField::host(k(keys::kBindAddress), QStringLiteral("监听地址"),
                                 QStringLiteral("0.0.0.0"))
                   .describedAs(QStringLiteral("0.0.0.0 表示监听所有网卡")));
    schema.add(ConfigField::port(k(keys::kPort), QStringLiteral("监听端口"), 502));
    schema.add(ConfigField::integer(k(keys::kMaxConnections), QStringLiteral("最大连接数"), 16)
                   .range(1, 1024)
                   .asAdvanced());
    return schema;
}

ConfigSchema tcpClientSchema()
{
    ConfigSchema schema(QStringLiteral("TCP 客户端"));
    schema.add(ConfigField::host(k(keys::kHost), QStringLiteral("目标地址")));
    schema.add(ConfigField::port(k(keys::kPort), QStringLiteral("目标端口"), 502));
    schema.add(ConfigField::duration(k(keys::kConnectTimeoutMs), QStringLiteral("连接超时"), 3000)
                   .range(100, 120000));
    schema.add(ConfigField::boolean(k(keys::kAutoReconnect), QStringLiteral("断线自动重连"), true));
    schema.add(ConfigField::duration(k(keys::kReconnectIntervalMs), QStringLiteral("重连间隔"), 2000)
                   .range(100, 600000)
                   .shownWhen(QStringLiteral("autoReconnect==true")));
    return schema;
}

ConfigSchema udpSchema()
{
    ConfigSchema schema(QStringLiteral("UDP"));
    schema.add(ConfigField::host(k(keys::kBindAddress), QStringLiteral("绑定地址"),
                                 QStringLiteral("0.0.0.0")));
    schema.add(ConfigField::port(k(keys::kLocalPort), QStringLiteral("本地端口"), 502));
    schema.add(ConfigField::enumeration(k(keys::kPeerMode), QStringLiteral("对端模式"),
                                        {QStringLiteral("discover"), QStringLiteral("fixed")},
                                        QStringLiteral("discover"))
                   .withLabels({QStringLiteral("按来源自动建链"), QStringLiteral("固定对端")})
                   .describedAs(QStringLiteral(
                       "discover 模式下每个发来数据的源地址都会生成一条虚拟链路")));
    schema.add(ConfigField::host(k(keys::kRemoteHost), QStringLiteral("对端地址"))
                   .shownWhen(QStringLiteral("peerMode==fixed")));
    schema.add(ConfigField::port(k(keys::kRemotePort), QStringLiteral("对端端口"), 502)
                   .shownWhen(QStringLiteral("peerMode==fixed")));
    schema.add(ConfigField::integer(k(keys::kMaxConnections), QStringLiteral("最大对端数"), 64)
                   .range(1, 4096)
                   .describedAs(QStringLiteral(
                       "discover 模式下的虚拟链路上限，防止端口扫描把链路表撑爆"))
                   .shownWhen(QStringLiteral("peerMode==discover"))
                   .asAdvanced());
    return schema;
}

ConfigSchema serialSchema()
{
    ConfigSchema schema(QStringLiteral("串口"));
    schema.add(ConfigField::text(k(keys::kPortName), QStringLiteral("端口"), QStringLiteral("COM1"))
                   .withPlaceholder(QStringLiteral("COM1 / /dev/ttyUSB0")));
    schema.add(ConfigField::enumeration(k(keys::kBaudRate), QStringLiteral("波特率"),
                                        {QStringLiteral("1200"), QStringLiteral("2400"),
                                         QStringLiteral("4800"), QStringLiteral("9600"),
                                         QStringLiteral("19200"), QStringLiteral("38400"),
                                         QStringLiteral("57600"), QStringLiteral("115200"),
                                         QStringLiteral("230400"), QStringLiteral("460800"),
                                         QStringLiteral("921600")},
                                        QStringLiteral("9600")));
    schema.add(ConfigField::enumeration(k(keys::kDataBits), QStringLiteral("数据位"),
                                        {QStringLiteral("5"), QStringLiteral("6"),
                                         QStringLiteral("7"), QStringLiteral("8")},
                                        QStringLiteral("8")));
    schema.add(ConfigField::enumeration(k(keys::kParity), QStringLiteral("校验位"),
                                        {QStringLiteral("none"), QStringLiteral("even"),
                                         QStringLiteral("odd"), QStringLiteral("mark"),
                                         QStringLiteral("space")},
                                        QStringLiteral("none"))
                   .withLabels({QStringLiteral("无"), QStringLiteral("偶校验"),
                                QStringLiteral("奇校验"), QStringLiteral("标记"),
                                QStringLiteral("空格")}));
    schema.add(ConfigField::enumeration(k(keys::kStopBits), QStringLiteral("停止位"),
                                        {QStringLiteral("1"), QStringLiteral("1.5"),
                                         QStringLiteral("2")},
                                        QStringLiteral("1")));
    schema.add(ConfigField::enumeration(k(keys::kFlowControl), QStringLiteral("流控"),
                                        {QStringLiteral("none"), QStringLiteral("hardware"),
                                         QStringLiteral("software")},
                                        QStringLiteral("none"))
                   .withLabels({QStringLiteral("无"), QStringLiteral("硬件"), QStringLiteral("软件")})
                   .asAdvanced());
    schema.add(ConfigField::duration(k(keys::kInterFrameGapMs), QStringLiteral("帧间静默"), 4)
                   .range(0, 1000)
                   .describedAs(QStringLiteral(
                       "RTU 分帧依据的 3.5 字符静默时间，0 表示交由协议插件自行分帧"))
                   .asAdvanced());
    return schema;
}

ConfigSchema loopbackSchema()
{
    ConfigSchema schema(QStringLiteral("内存回环"));
    schema.add(ConfigField::duration(k(keys::kLatencyMs), QStringLiteral("模拟时延"), 0)
                   .range(0, 60000)
                   .describedAs(QStringLiteral("每个方向附加的传输延迟，用于复现慢链路")));
    return schema;
}

} // namespace

TransportConfig::TransportConfig(TransportKind kind, QVariantMap values)
    : kind_(kind), values_(std::move(values))
{
    applyDefaults();
}

QVariant TransportConfig::value(const QString& key, const QVariant& fallback) const
{
    return values_.value(key, fallback);
}

void TransportConfig::setValue(const QString& key, QVariant newValue)
{
    values_.insert(key, std::move(newValue));
}

bool TransportConfig::contains(const QString& key) const
{
    return values_.contains(key);
}

QString TransportConfig::host() const
{
    return values_.value(k(keys::kHost), QStringLiteral("127.0.0.1")).toString();
}

quint16 TransportConfig::port() const
{
    return static_cast<quint16>(values_.value(k(keys::kPort), 502).toUInt());
}

QString TransportConfig::bindAddress() const
{
    return values_.value(k(keys::kBindAddress), QStringLiteral("0.0.0.0")).toString();
}

quint16 TransportConfig::localPort() const
{
    return static_cast<quint16>(values_.value(k(keys::kLocalPort), 0).toUInt());
}

QString TransportConfig::portName() const
{
    return values_.value(k(keys::kPortName)).toString();
}

qint32 TransportConfig::baudRate() const
{
    return values_.value(k(keys::kBaudRate), 9600).toInt();
}

bool TransportConfig::autoReconnect() const
{
    return values_.value(k(keys::kAutoReconnect), true).toBool();
}

int TransportConfig::reconnectIntervalMs() const
{
    return values_.value(k(keys::kReconnectIntervalMs), 2000).toInt();
}

void TransportConfig::applyDefaults()
{
    values_ = schemaFor(kind_).normalise(values_);
}

Result<void> TransportConfig::validate() const
{
    return schemaFor(kind_).validate(values_);
}

QJsonObject TransportConfig::toJson() const
{
    QJsonObject json;
    json.insert(QStringLiteral("kind"), transportKindName(kind_));
    json.insert(QStringLiteral("role"), transportRoleName(role_));
    json.insert(QStringLiteral("values"), QJsonObject::fromVariantMap(values_));
    return json;
}

Result<TransportConfig> TransportConfig::fromJson(const QJsonObject& json)
{
    const auto kind = transportKindFromName(json.value(QStringLiteral("kind")).toString());
    if (kind.hasError()) {
        return kind.error();
    }

    TransportConfig config(kind.value(),
                           json.value(QStringLiteral("values")).toObject().toVariantMap());
    config.setRole(json.value(QStringLiteral("role")).toString() == QStringLiteral("Initiator")
                       ? TransportRole::Initiator
                       : TransportRole::Responder);

    if (const auto valid = config.validate(); valid.hasError()) {
        return valid.error();
    }
    return config;
}

QString TransportConfig::describe() const
{
    using Formatter = QString (*)(const TransportConfig&);
    static const QHash<TransportKind, Formatter> kFormatters{
        {TransportKind::TcpServer,
         [](const TransportConfig& c) {
             return QStringLiteral("%1:%2").arg(c.bindAddress()).arg(c.port());
         }},
        {TransportKind::TcpClient,
         [](const TransportConfig& c) {
             return QStringLiteral("%1:%2").arg(c.host()).arg(c.port());
         }},
        {TransportKind::Udp,
         [](const TransportConfig& c) {
             return QStringLiteral("%1:%2").arg(c.bindAddress()).arg(c.localPort());
         }},
        {TransportKind::Serial,
         [](const TransportConfig& c) {
             return QStringLiteral("%1 @ %2").arg(c.portName()).arg(c.baudRate());
         }},
        {TransportKind::Loopback,
         [](const TransportConfig&) { return QStringLiteral("in-memory"); }},
    };

    const QString prefix = transportKindDisplayName(kind_);
    const Formatter formatter = kFormatters.value(kind_, nullptr);
    return formatter == nullptr ? prefix : QStringLiteral("%1 %2").arg(prefix, formatter(*this));
}

ConfigSchema TransportConfig::schemaFor(TransportKind kind)
{
    using Builder = ConfigSchema (*)();
    static const QHash<TransportKind, Builder> kBuilders{
        {TransportKind::TcpServer, &tcpServerSchema},
        {TransportKind::TcpClient, &tcpClientSchema},
        {TransportKind::Udp, &udpSchema},
        {TransportKind::Serial, &serialSchema},
        {TransportKind::Loopback, &loopbackSchema},
    };

    const Builder builder = kBuilders.value(kind, nullptr);
    return builder == nullptr ? ConfigSchema{} : builder();
}

} // namespace hwsim::transport
