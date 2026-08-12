#include "LoopbackTransport.h"

#include <core/HexUtils.h>
#include <core/Logger.h>

#include <QMetaObject>
#include <QTimer>

namespace {
constexpr auto kLogCategory = "transport.loopback";
}

namespace hwsim::transport {

// --- LoopbackLink ----------------------------------------------------------

LoopbackLink::LoopbackLink(LinkId id, LoopbackTransport* owner, QString peerName, QObject* parent)
    : ILink(id, parent), owner_(owner), peerName_(std::move(peerName))
{
}

QString LoopbackLink::peerDescription() const
{
    return peerName_;
}

void LoopbackLink::deliver(const QByteArray& data)
{
    handleIncoming(data);
}

void LoopbackLink::markConnected()
{
    setState(LinkState::Connected);
}

qint64 LoopbackLink::writeBytes(std::span<const std::byte> data)
{
    if (owner_.isNull()) {
        reportError(core::makeError(core::ErrorCode::NotConnected,
                                    QStringLiteral("loopback endpoint is gone")));
        return -1;
    }
    return owner_->transmitToPeer(core::hex::toByteArray(data));
}

void LoopbackLink::closeImpl() {}

// --- LoopbackTransport -----------------------------------------------------

LoopbackTransport::LoopbackTransport(QObject* parent) : ITransport(parent) {}

LoopbackTransport::~LoopbackTransport()
{
    disconnectPeer(QStringLiteral("endpoint destroyed"));
}

core::Result<void> LoopbackTransport::openImpl(const TransportConfig& config)
{
    latencyMs_ = config.value(QStringLiteral("latencyMs"), 0).toInt();
    return core::success();
}

void LoopbackTransport::closeImpl()
{
    disconnectPeer(QStringLiteral("endpoint closed"));
    localLinkId_ = kInvalidLinkId;
}

void LoopbackTransport::connectPair(LoopbackTransport* first, LoopbackTransport* second)
{
    if (first == nullptr || second == nullptr || first == second) {
        return;
    }

    // Pairing creates a QObject on each side and writes both peers' state, so
    // it has to happen with both endpoints on one thread. Move them afterwards
    // if the test needs them apart.
    if (first->thread() != second->thread()) {
        HWSIM_LOG_WARNING(kLogCategory)
            << "connectPair requires both endpoints on the same thread; refusing to pair "
            << first->describe() << " with " << second->describe();
        return;
    }

    first->peer_ = second;
    second->peer_ = first;

    const auto raise = [](LoopbackTransport* endpoint, const QString& peerName) {
        auto link = std::make_unique<LoopbackLink>(ILink::allocateId(), endpoint, peerName);
        LoopbackLink* raw = link.get();
        endpoint->localLinkId_ = raw->id();
        endpoint->addLink(std::move(link));
        raw->markConnected();
    };

    raise(first, second->name().isEmpty() ? QStringLiteral("loopback-b") : second->name());
    raise(second, first->name().isEmpty() ? QStringLiteral("loopback-a") : first->name());

    HWSIM_LOG_DEBUG(kLogCategory) << "paired " << first->describe() << " with " << second->describe();
}

void LoopbackTransport::disconnectPeer(const QString& reason)
{
    if (LoopbackTransport* other = peer_.data(); other != nullptr) {
        peer_.clear();
        other->peer_.clear();
        if (other->localLinkId_ != kInvalidLinkId) {
            const LinkId id = other->localLinkId_;
            other->localLinkId_ = kInvalidLinkId;
            other->removeLink(id, reason);
        }
    }

    if (localLinkId_ != kInvalidLinkId) {
        const LinkId id = localLinkId_;
        localLinkId_ = kInvalidLinkId;
        removeLink(id, reason);
    }
}

qint64 LoopbackTransport::transmitToPeer(const QByteArray& data)
{
    LoopbackTransport* other = peer_.data();
    if (other == nullptr) {
        return -1;
    }

    // Always hop through the event loop. A responder that answers while still
    // inside its own read handler would otherwise recurse into the sender.
    QPointer<LoopbackTransport> target(other);
    const auto forward = [target, data] {
        if (!target.isNull()) {
            target->receiveFromPeer(data);
        }
    };

    if (latencyMs_ > 0) {
        QTimer::singleShot(latencyMs_, other, forward);
    } else {
        QMetaObject::invokeMethod(other, forward, Qt::QueuedConnection);
    }
    return static_cast<qint64>(data.size());
}

void LoopbackTransport::receiveFromPeer(const QByteArray& data)
{
    if (localLinkId_ == kInvalidLinkId) {
        return;
    }
    if (auto* link = qobject_cast<LoopbackLink*>(findLink(localLinkId_)); link != nullptr) {
        link->deliver(data);
    }
}

} // namespace hwsim::transport
