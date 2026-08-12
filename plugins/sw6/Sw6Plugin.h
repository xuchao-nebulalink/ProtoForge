#pragma once

#include <protocol/IProtocolPlugin.h>

#include <QObject>

namespace hwsim::plugins::sw6 {

/// SW6 six-legged (Stewart) platform, protocol V2.
///
/// Both roles are supported: as a responder the plugin simulates the
/// controller, as an initiator it drives a real one and consumes its realtime
/// stream.
class Sw6Plugin : public QObject, public hwsim::protocol::IProtocolPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID HWSIM_PROTOCOL_PLUGIN_IID FILE "sw6.json")
    Q_INTERFACES(hwsim::protocol::IProtocolPlugin)
    HWSIM_DECLARE_PROTOCOL_PLUGIN

public:
    Sw6Plugin() = default;

    [[nodiscard]] hwsim::protocol::PluginMetadata metadata() const override;
    [[nodiscard]] hwsim::core::ConfigSchema configSchema() const override;

    [[nodiscard]] hwsim::core::Result<hwsim::protocol::FrameCodecPtr> createCodec(
        const QVariantMap& config) const override;

    [[nodiscard]] hwsim::core::Result<void> registerCommands(
        hwsim::protocol::CommandRegistry& registry, const QVariantMap& config) const override;

    [[nodiscard]] QJsonArray defaultParameterTemplate() const override;
};

} // namespace hwsim::plugins::sw6
