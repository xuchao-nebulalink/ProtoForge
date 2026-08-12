#include "TransportFactory.h"

#include "LoopbackTransport.h"
#include "SerialTransport.h"
#include "TcpClientTransport.h"
#include "TcpServerTransport.h"
#include "UdpTransport.h"

namespace hwsim::transport {

TransportFactory::TransportFactory()
{
    registerTransportMetaTypes();

    registerKind(TransportKind::TcpServer, transportKindDisplayName(TransportKind::TcpServer),
                 [] { return std::make_unique<TcpServerTransport>(); });
    registerKind(TransportKind::TcpClient, transportKindDisplayName(TransportKind::TcpClient),
                 [] { return std::make_unique<TcpClientTransport>(); });
    registerKind(TransportKind::Udp, transportKindDisplayName(TransportKind::Udp),
                 [] { return std::make_unique<UdpTransport>(); });
    registerKind(TransportKind::Serial, transportKindDisplayName(TransportKind::Serial),
                 [] { return std::make_unique<SerialTransport>(); });
    registerKind(TransportKind::Loopback, transportKindDisplayName(TransportKind::Loopback),
                 [] { return std::make_unique<LoopbackTransport>(); });
}

TransportFactory& TransportFactory::instance()
{
    static TransportFactory factory;
    return factory;
}

bool TransportFactory::registerKind(TransportKind kind, QString displayName, Creator creator)
{
    return registry_.add(transportKindName(kind), std::move(displayName),
                         [creator = std::move(creator)]() -> std::unique_ptr<ITransport> {
                             return creator();
                         });
}

core::Result<std::unique_ptr<ITransport>> TransportFactory::create(TransportKind kind) const
{
    return registry_.create(transportKindName(kind));
}

core::Result<std::unique_ptr<ITransport>> TransportFactory::create(const QString& kindName) const
{
    return registry_.create(kindName);
}

core::Result<std::unique_ptr<ITransport>> TransportFactory::createAndOpen(TransportConfig config) const
{
    auto created = create(config.kind());
    if (created.hasError()) {
        return created.error();
    }

    auto transport = std::move(created).value();
    if (const auto opened = transport->open(std::move(config)); opened.hasError()) {
        return opened.error();
    }
    return transport;
}

QList<TransportKind> TransportFactory::availableKinds() const
{
    QList<TransportKind> kinds;
    for (const QString& name : registry_.keys()) {
        if (const auto kind = transportKindFromName(name); kind.hasValue()) {
            kinds.append(kind.value());
        }
    }
    return kinds;
}

} // namespace hwsim::transport
