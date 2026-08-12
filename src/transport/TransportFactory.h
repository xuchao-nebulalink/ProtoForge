#pragma once

#include "ITransport.h"

#include <core/Registry.h>

#include <QList>

#include <functional>
#include <memory>

namespace hwsim::transport {

/// Turns a configuration into a transport instance.
///
/// Registration based rather than a hand-written factory function, so a new
/// endpoint type (a CAN adapter, a websocket bridge) is added by registering it
/// here and adding a schema in TransportConfig, without editing any dispatch
/// logic.
class HWSIM_TRANSPORT_API TransportFactory {
public:
    using Creator = std::function<std::unique_ptr<ITransport>()>;

    [[nodiscard]] static TransportFactory& instance();

    bool registerKind(TransportKind kind, QString displayName, Creator creator);

    [[nodiscard]] core::Result<std::unique_ptr<ITransport>> create(TransportKind kind) const;
    [[nodiscard]] core::Result<std::unique_ptr<ITransport>> create(const QString& kindName) const;

    /// Creates and opens in one step, so callers do not have to remember the order.
    [[nodiscard]] core::Result<std::unique_ptr<ITransport>> createAndOpen(TransportConfig config) const;

    [[nodiscard]] QList<TransportKind> availableKinds() const;

private:
    TransportFactory();

    core::Registry<QString, ITransport> registry_;
};

} // namespace hwsim::transport
