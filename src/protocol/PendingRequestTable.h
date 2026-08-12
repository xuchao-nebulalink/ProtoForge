#pragma once

#include "IMessage.h"

#include <QString>

#include <deque>
#include <functional>

namespace hwsim::protocol {

/// Matches responses to the requests that caused them, for initiator sessions.
///
/// Two matching strategies, chosen by the codec: a correlation key when the
/// protocol carries one (Modbus TCP transaction id), and oldest-first when it
/// does not (Modbus RTU, which is strictly serial on the wire). Both go through
/// the same table so the session code has one path.
class HWSIM_PROTOCOL_API PendingRequestTable {
public:
    using Callback = std::function<void(core::Result<MessagePtr>)>;

    struct Entry {
        quint64 id{0};
        QString correlationKey;
        MessagePtr request;
        Callback callback;
        qint64 deadlineMs{0};
        int timeoutMs{0};
    };

    /// Returns the id, which cancel() accepts.
    quint64 add(QString correlationKey, MessagePtr request, Callback callback, int timeoutMs,
                qint64 nowMs);

    /// Delivers `response` to the matching request. An empty key matches the
    /// oldest outstanding entry. Returns false when nothing was waiting, which
    /// tells the session the frame is unsolicited and should go to a handler.
    bool resolve(const QString& correlationKey, MessagePtr response);

    /// Fails every entry whose deadline has passed. Returns the expired entries
    /// so the caller can report them.
    std::vector<Entry> takeExpired(qint64 nowMs);

    /// Fails everything, used when the link drops.
    void failAll(const core::Error& error);

    bool cancel(quint64 id);

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] bool isEmpty() const noexcept { return entries_.empty(); }

    /// Earliest deadline, or 0 when nothing is outstanding.
    [[nodiscard]] qint64 nextDeadlineMs() const;

private:
    std::deque<Entry> entries_;
    quint64 nextId_{1};
};

} // namespace hwsim::protocol
