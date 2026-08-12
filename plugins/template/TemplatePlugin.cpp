#include "TemplatePlugin.h"

#include "TemplateProtocol.h"

#include <QJsonArray>
#include <QJsonObject>

using hwsim::core::ConfigField;
using hwsim::core::ConfigSchema;
using hwsim::core::Result;
using hwsim::protocol::CommandRegistry;
using hwsim::protocol::FrameCodecPtr;
using hwsim::protocol::PluginMetadata;

namespace hwsim::plugins::tlv {
namespace {

constexpr quint32 kDefaultTagBase = 50000;

bool isInitiator(const QVariantMap& config)
{
    return config.value(QStringLiteral("role"), QStringLiteral("responder")).toString()
           == QStringLiteral("initiator");
}

QJsonObject tagDefinition(const QString& key, quint32 address, const QString& label, double value,
                          const QString& unit)
{
    QJsonObject json;
    json.insert(QStringLiteral("key"), key);
    json.insert(QStringLiteral("displayName"), label);
    json.insert(QStringLiteral("type"), QStringLiteral("double"));
    json.insert(QStringLiteral("access"), QStringLiteral("rw"));
    json.insert(QStringLiteral("address"), static_cast<qint64>(address));
    json.insert(QStringLiteral("default"), value);
    json.insert(QStringLiteral("group"), QStringLiteral("过程量"));
    json.insert(QStringLiteral("unit"), unit);
    return json;
}

} // namespace

PluginMetadata TemplatePlugin::metadata() const
{
    PluginMetadata metadata;
    metadata.id = QStringLiteral("tlv-template");
    metadata.displayName = QStringLiteral("私有协议模板 (TLV)");
    metadata.version = QStringLiteral("1.0.0");
    metadata.vendor = QStringLiteral("HwSimPlatform");
    metadata.description =
        QStringLiteral("带起始字节、序列号、长度字段与 CRC16-CCITT 的 TLV 协议样例，"
                       "作为新增私有协议的复制起点。");
    metadata.variants = {QStringLiteral("v1")};
    return metadata;
}

ConfigSchema TemplatePlugin::configSchema() const
{
    ConfigSchema schema(QStringLiteral("私有协议模板"));

    schema.add(ConfigField::integer(QStringLiteral("tagBase"), QStringLiteral("标签基地址"),
                                    kDefaultTagBase)
                   .range(0, 4000000000.0)
                   .describedAs(QStringLiteral("协议里的 tag 号加上该基址即为设备参数地址")));

    schema.merge(TlvCodec{}.configSchema(), QStringLiteral("组帧"));
    return schema;
}

Result<FrameCodecPtr> TemplatePlugin::createCodec(const QVariantMap& config) const
{
    auto codec = std::make_unique<TlvCodec>();
    if (const auto configured = codec->configure(config); configured.hasError()) {
        return configured.error();
    }
    return FrameCodecPtr(std::move(codec));
}

Result<void> TemplatePlugin::registerCommands(CommandRegistry& registry,
                                              const QVariantMap& config) const
{
    const auto tagBase = config.value(QStringLiteral("tagBase"), kDefaultTagBase).toUInt();

    if (isInitiator(config)) {
        registry.bindEncoder<HeartbeatRequest>();
        registry.bindEncoder<ReadTagsRequest>();
        registry.bindEncoder<WriteTagsRequest>();

        registry.bindDecoder<HeartbeatResponse>(HeartbeatResponse::opcode());
        registry.bindDecoder<ReadTagsResponse>(ReadTagsResponse::opcode());
        registry.bindDecoder<WriteTagsResponse>(WriteTagsResponse::opcode());
        registry.bindDecoder<ErrorResponse>(ErrorResponse::opcode());
        return core::success();
    }

    // One line per message: decoder, handler and encoder all get registered.
    registry.bind<HeartbeatRequest>(std::make_shared<HeartbeatHandler>(),
                                    QStringLiteral("Heartbeat"));
    registry.bind<ReadTagsRequest>(std::make_shared<ReadTagsHandler>(tagBase),
                                   QStringLiteral("ReadTags"));
    registry.bind<WriteTagsRequest>(std::make_shared<WriteTagsHandler>(tagBase),
                                    QStringLiteral("WriteTags"));

    // Messages this side only ever transmits.
    registry.bindEncoder<HeartbeatResponse>();
    registry.bindEncoder<ReadTagsResponse>();
    registry.bindEncoder<WriteTagsResponse>();
    registry.bindEncoder<ErrorResponse>();

    return core::success();
}

QJsonArray TemplatePlugin::defaultParameterTemplate() const
{
    QJsonArray parameters;
    parameters.append(tagDefinition(QStringLiteral("tag.temperature"), kDefaultTagBase + 1,
                                    QStringLiteral("温度"), 25.0, QStringLiteral("℃")));
    parameters.append(tagDefinition(QStringLiteral("tag.pressure"), kDefaultTagBase + 2,
                                    QStringLiteral("压力"), 101.3, QStringLiteral("kPa")));
    parameters.append(tagDefinition(QStringLiteral("tag.flow"), kDefaultTagBase + 3,
                                    QStringLiteral("流量"), 0.0, QStringLiteral("L/min")));
    return parameters;
}

} // namespace hwsim::plugins::tlv

HWSIM_EXPORT_STATIC_PLUGIN(tlv_template, hwsim::plugins::tlv::TemplatePlugin)
