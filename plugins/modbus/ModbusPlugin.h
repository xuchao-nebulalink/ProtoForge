#pragma once

#include <protocol/IProtocolPlugin.h>

#include <QObject>

namespace hwsim::plugins::modbus {

/// Modbus RTU and Modbus TCP.
///
/// One plugin, two framings, one set of message types and handlers. That split
/// is the whole reason IFrameCodec is separate from the message types: RTU and
/// TCP disagree about every byte of framing and agree about every function
/// code, so only the codec changes between them.
///
/// The same binary also serves both roles. registerCommands() looks at the
/// configured role and installs request decoders plus handlers for a simulated
/// slave, or response decoders plus request encoders for a test master.
class ModbusPlugin : public QObject, public hwsim::protocol::IProtocolPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID HWSIM_PROTOCOL_PLUGIN_IID FILE "modbus.json")
    Q_INTERFACES(hwsim::protocol::IProtocolPlugin)
    HWSIM_DECLARE_PROTOCOL_PLUGIN

public:
    ModbusPlugin() = default;

    [[nodiscard]] hwsim::protocol::PluginMetadata metadata() const override;
    [[nodiscard]] hwsim::core::ConfigSchema configSchema() const override;

    [[nodiscard]] hwsim::core::Result<hwsim::protocol::FrameCodecPtr> createCodec(
        const QVariantMap& config) const override;

    [[nodiscard]] hwsim::core::Result<void> registerCommands(
        hwsim::protocol::CommandRegistry& registry, const QVariantMap& config) const override;

    [[nodiscard]] QJsonArray defaultParameterTemplate() const override;
};

} // namespace hwsim::plugins::modbus
