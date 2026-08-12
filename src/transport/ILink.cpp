#include "ILink.h"

#include <core/Clock.h>
#include <core/HexUtils.h>

#include <atomic>

namespace hwsim::transport {
namespace {

std::atomic<LinkId> g_nextLinkId{1};

} // namespace

ILink::ILink(LinkId id, QObject* parent) : QObject(parent), id_(id)
{
    statistics_.openedAtMs = core::monotonicMs();
}

ILink::~ILink() = default;

LinkId ILink::allocateId() noexcept
{
    return g_nextLinkId.fetch_add(1, std::memory_order_relaxed);
}

QString ILink::localDescription() const
{
    return {};
}

qint64 ILink::send(std::span<const std::byte> data)
{
    if (!isOpen()) {
        reportError(core::makeError(core::ErrorCode::NotConnected,
                                    QStringLiteral("link %1 is %2")
                                        .arg(id_)
                                        .arg(linkStateName(state_))));
        return -1;
    }

    const qint64 written = writeBytes(data);
    if (written < 0) {
        ++statistics_.errorCount;
        return written;
    }

    statistics_.bytesSent += static_cast<quint64>(written);
    statistics_.lastActivityMs = core::monotonicMs();
    emit bytesSent(id_, core::hex::toByteArray(data.first(static_cast<std::size_t>(written))));
    return written;
}

qint64 ILink::send(const QByteArray& data)
{
    return send(core::hex::asBytes(data));
}

void ILink::close()
{
    if (state_ == LinkState::Disconnected || state_ == LinkState::Closing) {
        return;
    }
    setState(LinkState::Closing);
    closeImpl();
    setState(LinkState::Disconnected);
}

void ILink::resetStatistics()
{
    const qint64 openedAt = statistics_.openedAtMs;
    statistics_.reset();
    statistics_.openedAtMs = openedAt;
}

void ILink::noteFrameReceived() noexcept
{
    ++statistics_.framesReceived;
}

void ILink::noteFrameSent() noexcept
{
    ++statistics_.framesSent;
}

void ILink::setState(LinkState state)
{
    if (state_ == state) {
        return;
    }
    state_ = state;
    emit stateChanged(id_, state_);
}

void ILink::handleIncoming(const QByteArray& data)
{
    if (data.isEmpty()) {
        return;
    }
    statistics_.bytesReceived += static_cast<quint64>(data.size());
    statistics_.lastActivityMs = core::monotonicMs();
    emit bytesReceived(id_, data);
}

void ILink::reportError(core::Error error)
{
    ++statistics_.errorCount;
    emit errorOccurred(id_, error);
}

} // namespace hwsim::transport
