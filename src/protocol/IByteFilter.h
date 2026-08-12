#pragma once

#include "ProtocolTypes.h"

#include <QByteArray>

#include <memory>
#include <vector>

namespace hwsim::protocol {

struct HWSIM_PROTOCOL_API ByteFilterContext {
    transport::LinkId linkId{transport::kInvalidLinkId};
    QString sessionName;
    transport::Direction direction{transport::Direction::Outbound};

    /// Set when the bytes correspond to a message the session has decoded.
    /// Null for raw inbound traffic, which has not been framed yet.
    const Frame* frame{nullptr};
};

/// What a filter decided to do with a buffer.
struct HWSIM_PROTOCOL_API ByteFilterDecision {
    bool deliver{true};
    int delayMs{0};
    QString note;

    [[nodiscard]] static ByteFilterDecision pass() { return {}; }
    [[nodiscard]] static ByteFilterDecision drop(QString reason)
    {
        return ByteFilterDecision{false, 0, std::move(reason)};
    }
    [[nodiscard]] static ByteFilterDecision delay(int milliseconds, QString reason)
    {
        return ByteFilterDecision{true, milliseconds, std::move(reason)};
    }
};

/// Byte-level interception point, sitting between the codec and the link.
///
/// This is where fault injection lives. Corrupting a checksum, flipping a bit,
/// truncating, dropping or delaying a frame are all things that must happen to
/// the finished bytes, after framing, and they must be invisible to protocol
/// code: a plugin should never contain an "if fault injection is on" branch.
///
/// Message-level concerns (tracing, statistics, state gating) belong in
/// IMiddleware instead.
class HWSIM_PROTOCOL_API IByteFilter {
public:
    virtual ~IByteFilter() = default;

    [[nodiscard]] virtual QString name() const = 0;
    [[nodiscard]] virtual bool isEnabled() const { return true; }

    /// May modify `bytes` in place. Returning a decision with deliver == false
    /// discards the buffer entirely.
    [[nodiscard]] virtual ByteFilterDecision apply(QByteArray& bytes,
                                                   const ByteFilterContext& context) = 0;
};

using ByteFilterPtr = std::shared_ptr<IByteFilter>;

/// Applies filters in order and merges their decisions: any drop wins, delays
/// add up, and notes are collected for the packet view annotation.
class HWSIM_PROTOCOL_API ByteFilterChain {
public:
    void add(ByteFilterPtr filter);
    void remove(const QString& name);
    void clear();

    [[nodiscard]] bool isEmpty() const noexcept { return filters_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return filters_.size(); }
    [[nodiscard]] QStringList names() const;

    [[nodiscard]] ByteFilterDecision apply(QByteArray& bytes, const ByteFilterContext& context) const;

private:
    std::vector<ByteFilterPtr> filters_;
};

} // namespace hwsim::protocol
