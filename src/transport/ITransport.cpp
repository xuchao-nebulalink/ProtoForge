#include "ITransport.h"

#include <core/Logger.h>

#include <QMetaObject>

#include <algorithm>

namespace {
constexpr auto kLogCategory = "transport";
}

namespace hwsim::transport {

ITransport::ITransport(QObject* parent) : QObject(parent)
{
    registerTransportMetaTypes();
}

ITransport::~ITransport()
{
    tearingDown_ = true;
    links_.clear();
    retired_.clear();
}

QString ITransport::describe() const
{
    return name_.isEmpty() ? config_.describe()
                           : QStringLiteral("%1 (%2)").arg(name_, config_.describe());
}

core::Result<void> ITransport::open(TransportConfig config)
{
    if (open_) {
        return core::makeError(core::ErrorCode::AlreadyExists,
                               QStringLiteral("transport is already open"), describe());
    }

    config.setKind(kind());
    if (const auto valid = config.validate(); valid.hasError()) {
        return valid.error();
    }
    config_ = std::move(config);

    if (const auto result = openImpl(config_); result.hasError()) {
        HWSIM_LOG_ERROR(kLogCategory) << "open failed: " << result.error().toString();
        return result;
    }

    open_ = true;
    HWSIM_LOG_INFO(kLogCategory) << "opened " << describe();
    emit opened();
    return core::success();
}

void ITransport::close()
{
    if (!open_) {
        return;
    }
    open_ = false;

    removeAllLinks(QStringLiteral("transport closed"));
    closeImpl();

    HWSIM_LOG_INFO(kLogCategory) << "closed " << describe();
    emit closed();
}

QList<ILink*> ITransport::links() const
{
    QList<ILink*> result;
    result.reserve(static_cast<qsizetype>(links_.size()));
    for (const auto& link : links_) {
        result.append(link.get());
    }
    return result;
}

ILink* ITransport::findLink(LinkId id) const
{
    const auto it = std::find_if(links_.begin(), links_.end(),
                                 [id](const std::unique_ptr<ILink>& link) {
                                     return link->id() == id;
                                 });
    return it == links_.end() ? nullptr : it->get();
}

qsizetype ITransport::linkCount() const
{
    return static_cast<qsizetype>(links_.size());
}

ILink* ITransport::primaryLink() const
{
    return links_.empty() ? nullptr : links_.front().get();
}

qsizetype ITransport::broadcast(const QByteArray& data)
{
    qsizetype delivered = 0;
    for (const auto& link : links_) {
        if (link->isOpen() && link->send(data) >= 0) {
            ++delivered;
        }
    }
    return delivered;
}

ILink* ITransport::addLink(std::unique_ptr<ILink> link)
{
    if (!link) {
        return nullptr;
    }

    // A safe point to reclaim: we are not inside any retired link's signal
    // emission here, and this covers transports running without an event loop,
    // where the queued collectGarbage would never fire and socket handles would
    // accumulate for the lifetime of the transport.
    collectGarbage();

    ILink* raw = link.get();
    links_.push_back(std::move(link));

    HWSIM_LOG_DEBUG(kLogCategory) << "link " << raw->id() << " opened from "
                                  << raw->peerDescription();
    emit linkOpened(raw);
    return raw;
}

void ITransport::removeLink(LinkId id, const QString& reason)
{
    if (tearingDown_) {
        // Reached from a socket's parting signal while links_ is being cleared.
        // The link is already gone; searching for it would walk elements that
        // are mid-destruction.
        return;
    }

    const auto it = std::find_if(links_.begin(), links_.end(),
                                 [id](const std::unique_ptr<ILink>& link) {
                                     return link->id() == id;
                                 });
    if (it == links_.end()) {
        return;
    }

    // Deferred destruction: this is frequently reached from a slot connected to
    // one of the link's own signals, where deleting it immediately would tear
    // down the object still on the call stack.
    retired_.push_back(std::move(*it));
    links_.erase(it);
    retired_.back()->close();

    HWSIM_LOG_DEBUG(kLogCategory) << "link " << id << " closed: " << reason;
    emit linkClosed(id, reason);

    if (!garbageCollectionScheduled_) {
        garbageCollectionScheduled_ = true;
        QMetaObject::invokeMethod(this, [this] { collectGarbage(); }, Qt::QueuedConnection);
    }
}

void ITransport::removeAllLinks(const QString& reason)
{
    std::vector<LinkId> ids;
    ids.reserve(links_.size());
    for (const auto& link : links_) {
        ids.push_back(link->id());
    }
    for (const LinkId id : ids) {
        removeLink(id, reason);
    }

    // Only ever reached from close(), never from a link's own signal handler,
    // so the retired links can be released immediately rather than waiting for
    // an event loop that may not exist. The destructor clears both vectors
    // directly and does not come through here.
    collectGarbage();
}

void ITransport::reportError(core::Error error)
{
    HWSIM_LOG_WARNING(kLogCategory) << describe() << ": " << error.toString();
    emit errorOccurred(error);
}

void ITransport::collectGarbage()
{
    garbageCollectionScheduled_ = false;

    // Clearing by swap: destroying a link can re-enter removeLink through a
    // socket teardown signal, and that would push onto the vector being erased.
    std::vector<std::unique_ptr<ILink>> dying;
    dying.swap(retired_);
    dying.clear();
}

} // namespace hwsim::transport
