#pragma once

#include "CommandRegistry.h"
#include "IFrameCodec.h"

#include <core/ConfigSchema.h>

#include <QJsonArray>
#include <QObject>
#include <QStringList>
#include <QtPlugin>

/// Interface identifier. Bumping the trailing version invalidates every
/// previously built plugin binary, which is exactly what should happen when
/// anything below changes shape.
#define HWSIM_PROTOCOL_PLUGIN_IID "com.hwsim.ProtocolPlugin/1.0"

/// Checked at load time in addition to the IID, so a plugin built against an
/// older header is rejected with a clear message instead of crashing.
#define HWSIM_PLUGIN_ABI_VERSION 1

namespace hwsim::protocol {

/// Configuration keys the framework owns.
///
/// These are injected into the map handed to createCodec() and
/// registerCommands() before either is called. A plugin reads them but must not
/// declare them in its own configSchema(): they come from the endpoint, not
/// from the user editing protocol settings, and duplicating them in the
/// generated panel would let the two disagree.
namespace reserved {

/// "responder" for a simulated device, "initiator" for a test master.
///
/// Framing can depend on this. Modbus RTU carries no length field, so the
/// expected frame size is derived from the function code and differs between
/// requests and responses; a codec that guesses wrong silently mis-frames every
/// reply.
inline constexpr auto kRole = "role";
inline constexpr auto kRoleResponder = "responder";
inline constexpr auto kRoleInitiator = "initiator";

} // namespace reserved

struct HWSIM_PROTOCOL_API PluginMetadata {
    /// Stable identifier used in profiles and on the command line, e.g. "modbus".
    QString id;
    QString displayName;
    QString version;
    QString vendor;
    QString description;

    /// Framing variants this plugin offers, e.g. {"rtu", "tcp", "ascii"}. The
    /// selected variant is passed back in the configuration map.
    QStringList variants;
};

/// What a protocol plugin must provide.
///
/// A plugin contributes three things and nothing else: a codec that knows the
/// framing, a set of message types with their handlers, and a schema describing
/// its own settings. It never touches transports, the UI or the device model
/// implementation, which is why the same binary works for a simulated device
/// and for a test master, over any of the four transports.
class HWSIM_PROTOCOL_API IProtocolPlugin {
public:
    virtual ~IProtocolPlugin() = default;

    [[nodiscard]] virtual PluginMetadata metadata() const = 0;

    /// Must return HWSIM_PLUGIN_ABI_VERSION. Implemented by the
    /// HWSIM_DECLARE_PROTOCOL_PLUGIN macro so it cannot drift.
    [[nodiscard]] virtual int abiVersion() const = 0;

    /// Settings for one configured instance: framing variant, station address,
    /// byte order, and anything else the plugin needs. Drives the generated
    /// configuration panel.
    [[nodiscard]] virtual core::ConfigSchema configSchema() const = 0;

    [[nodiscard]] virtual core::Result<FrameCodecPtr> createCodec(const QVariantMap& config) const = 0;

    /// Fills the registry with this plugin's decoders, handlers and encoders.
    /// Called once per session, so handlers may hold per-instance state.
    [[nodiscard]] virtual core::Result<void> registerCommands(CommandRegistry& registry,
                                                              const QVariantMap& config) const = 0;

    /// Which endpoint roles make sense for this protocol. A plugin that only
    /// models a slave device returns just "responder".
    [[nodiscard]] virtual QStringList supportedRoles() const
    {
        return {QString::fromLatin1(reserved::kRoleResponder),
                QString::fromLatin1(reserved::kRoleInitiator)};
    }

    /// Optional starter set of device parameters, as the JSON the simulator's
    /// DeviceProfile understands. Returned as JSON rather than typed objects to
    /// keep this layer independent of the simulator module.
    [[nodiscard]] virtual QJsonArray defaultParameterTemplate() const { return {}; }
};

} // namespace hwsim::protocol

Q_DECLARE_INTERFACE(hwsim::protocol::IProtocolPlugin, HWSIM_PROTOCOL_PLUGIN_IID)

/// Boilerplate every plugin class needs, placed inside the class body.
///
///     class ModbusPlugin : public QObject, public hwsim::protocol::IProtocolPlugin {
///         Q_OBJECT
///         Q_PLUGIN_METADATA(IID HWSIM_PROTOCOL_PLUGIN_IID FILE "modbus.json")
///         Q_INTERFACES(hwsim::protocol::IProtocolPlugin)
///         HWSIM_DECLARE_PROTOCOL_PLUGIN
///     public:
///         ...
///     };
#define HWSIM_DECLARE_PROTOCOL_PLUGIN                                                    \
public:                                                                                  \
    [[nodiscard]] int abiVersion() const override { return HWSIM_PLUGIN_ABI_VERSION; }   \
                                                                                         \
private:

/// Emits the factory symbol the generated StaticPluginImports.cpp refers to.
/// Expands to nothing in a dynamic build, so the same source file serves both
/// linkage modes. Place it at file scope in the plugin's .cpp.
///
/// `pluginId` must match the NAME given to hwsim_add_protocol_plugin(), because
/// CMake derives the extern declaration from that name.
#if defined(HWSIM_PLUGIN_STATIC_BUILD)
#  define HWSIM_EXPORT_STATIC_PLUGIN(pluginId, ClassName)                                \
      extern "C" QObject* hwsim_static_plugin_##pluginId();                              \
      extern "C" QObject* hwsim_static_plugin_##pluginId() { return new ClassName(); }
#else
#  define HWSIM_EXPORT_STATIC_PLUGIN(pluginId, ClassName)
#endif
