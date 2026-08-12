#include "Sw6Plugin.h"

#include "Sw6Codec.h"
#include "Sw6DeviceState.h"
#include "Sw6Handlers.h"

#include <QJsonArray>
#include <QJsonObject>

using hwsim::core::ConfigField;
using hwsim::core::ConfigSchema;
using hwsim::core::Result;
using hwsim::protocol::CommandRegistry;
using hwsim::protocol::FrameCodecPtr;
using hwsim::protocol::PluginMetadata;

namespace hwsim::plugins::sw6 {
namespace {

bool isInitiator(const QVariantMap& config)
{
    return config.value(QString::fromLatin1(hwsim::protocol::reserved::kRole),
                        QString::fromLatin1(hwsim::protocol::reserved::kRoleResponder))
               .toString()
           == QString::fromLatin1(hwsim::protocol::reserved::kRoleInitiator);
}

QJsonObject parameter(const QString& key, const QString& label, const QString& group,
                      const QString& unit, double value)
{
    QJsonObject json;
    json.insert(QStringLiteral("key"), key);
    json.insert(QStringLiteral("displayName"), label);
    json.insert(QStringLiteral("type"), QStringLiteral("double"));
    json.insert(QStringLiteral("access"), QStringLiteral("rw"));
    json.insert(QStringLiteral("default"), value);
    json.insert(QStringLiteral("group"), group);
    json.insert(QStringLiteral("unit"), unit);
    return json;
}

} // namespace

PluginMetadata Sw6Plugin::metadata() const
{
    PluginMetadata metadata;
    metadata.id = QStringLiteral("sw6");
    metadata.displayName = QStringLiteral("六足平台 SW6");
    metadata.version = QStringLiteral("2.0.0");
    metadata.vendor = QStringLiteral("HwSimPlatform");
    metadata.description =
        QStringLiteral("六自由度并联（Stewart）平台协议 V2：$ 开头的 ASCII 命令帧（checksum-8）"
                       "与 0x81 二进制实时流（CRC-16/UMTS）共线并存。");
    metadata.variants = {QStringLiteral("v2")};
    return metadata;
}

ConfigSchema Sw6Plugin::configSchema() const
{
    ConfigSchema schema(QStringLiteral("SW6 六足平台"));

    schema.add(ConfigField::text(QStringLiteral("model"), QStringLiteral("设备型号"),
                                 QStringLiteral("SW6_HEXAPOD_V2"))
                   .describedAs(QStringLiteral("$IDN 回复的第一段")));
    schema.add(ConfigField::text(QStringLiteral("dateCode"), QStringLiteral("生产年月"),
                                 QStringLiteral("2509"))
                   .describedAs(QStringLiteral("$IDN 回复的 YYMM 段")));
    schema.add(ConfigField::text(QStringLiteral("serialNumber"), QStringLiteral("序列号"),
                                 QStringLiteral("001")));
    schema.add(ConfigField::text(QStringLiteral("firmwareVersion"), QStringLiteral("固件版本"),
                                 QStringLiteral("2.0.0"))
                   .describedAs(QStringLiteral("$VER 回复内容")));
    schema.add(ConfigField::integer(QStringLiteral("streamMask"), QStringLiteral("实时流掩码"), 0)
                   .range(0, 255)
                   .describedAs(QStringLiteral("0x81 帧的可选数据块，等价于上电后的 $RSE 设置："
                                               "bit0 理论位姿、bit1 实际杆长、bit2 理论杆长、"
                                               "bit3 杆长速度、bit4 AD 模拟量")));
    schema.add(ConfigField::integer(QStringLiteral("streamIntervalMs"),
                                    QStringLiteral("实时流推送周期"), 20)
                   .range(0, 60000)
                   .withUnit(QStringLiteral("ms"))
                   .describedAs(QStringLiteral("模拟设备按此周期主动推送 0x81 帧，0 = 不推送；"
                                               "测试主站角色下此项无效")));

    schema.merge(Sw6Codec{}.configSchema(), QStringLiteral("组帧"));
    return schema;
}

Result<FrameCodecPtr> Sw6Plugin::createCodec(const QVariantMap& config) const
{
    auto codec = std::make_unique<Sw6Codec>();
    if (const auto configured = codec->configure(config); configured.hasError()) {
        return configured.error();
    }
    return FrameCodecPtr(std::move(codec));
}

Result<void> Sw6Plugin::registerCommands(CommandRegistry& registry, const QVariantMap& config) const
{
    // One state object per session, which is what registerCommands() is called
    // for. In initiator mode it holds the mirror the realtime stream updates.
    auto state = std::make_shared<Sw6DeviceState>();
    state->setIdentity(
        config.value(QStringLiteral("model"), QStringLiteral("SW6_HEXAPOD_V2")).toString(),
        config.value(QStringLiteral("dateCode"), QStringLiteral("2509")).toString(),
        config.value(QStringLiteral("serialNumber"), QStringLiteral("001")).toString(),
        config.value(QStringLiteral("firmwareVersion"), QStringLiteral("2.0.0")).toString());
    state->setStreamMask(static_cast<quint8>(
        config.value(QStringLiteral("streamMask"), 0).toUInt() & 0xFFu));
    state->save();

    const int streamIntervalMs = config.value(QStringLiteral("streamIntervalMs"), 20).toInt();
    return registerSw6Commands(registry, state, isInitiator(config), streamIntervalMs);
}

QJsonArray Sw6Plugin::defaultParameterTemplate() const
{
    QJsonArray parameters;

    const QStringList labels{QStringLiteral("X 平移"), QStringLiteral("Y 平移"),
                             QStringLiteral("Z 平移"), QStringLiteral("U 旋转 (Rx)"),
                             QStringLiteral("V 旋转 (Ry)"), QStringLiteral("W 旋转 (Rz)")};
    for (int index = 0; index < kAxisCount; ++index) {
        const bool rotation = index >= 3;
        parameters.append(parameter(QStringLiteral("pose.%1").arg(axisName(index).toLower()),
                                    labels.at(index), QStringLiteral("台面位姿"),
                                    rotation ? QStringLiteral("rad") : QStringLiteral("mm"), 0.0));
    }

    for (int leg = 1; leg <= kLegCount; ++leg) {
        parameters.append(parameter(QStringLiteral("leg.%1.length").arg(leg),
                                    QStringLiteral("%1 号腿杆长").arg(leg),
                                    QStringLiteral("作动腿"), QStringLiteral("mm"), 100.0));
    }

    parameters.append(parameter(QStringLiteral("status.word"), QStringLiteral("状态字"),
                                QStringLiteral("状态"), QString{}, 0.0));
    return parameters;
}

} // namespace hwsim::plugins::sw6

HWSIM_EXPORT_STATIC_PLUGIN(sw6, hwsim::plugins::sw6::Sw6Plugin)
