#pragma once

#include <protocol/IProtocolPlugin.h>

#include <QObject>

namespace hwsim::plugins::tlv {

/// Reference implementation for a private protocol.
///
/// Copy the directory, rename the class and the plugin id, then replace the
/// message set and the codec. Everything else - metadata, schema, role-aware
/// registration, the static/dynamic export macro - is boilerplate that every
/// plugin needs and that is easier to adapt than to rewrite.
class TemplatePlugin : public QObject, public hwsim::protocol::IProtocolPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID HWSIM_PROTOCOL_PLUGIN_IID FILE "template.json")
    Q_INTERFACES(hwsim::protocol::IProtocolPlugin)
    HWSIM_DECLARE_PROTOCOL_PLUGIN

public:
    TemplatePlugin() = default;

    [[nodiscard]] hwsim::protocol::PluginMetadata metadata() const override;
    [[nodiscard]] hwsim::core::ConfigSchema configSchema() const override;

    [[nodiscard]] hwsim::core::Result<hwsim::protocol::FrameCodecPtr> createCodec(
        const QVariantMap& config) const override;

    [[nodiscard]] hwsim::core::Result<void> registerCommands(
        hwsim::protocol::CommandRegistry& registry, const QVariantMap& config) const override;

    [[nodiscard]] QJsonArray defaultParameterTemplate() const override;
};

} // namespace hwsim::plugins::tlv
