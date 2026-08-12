#include "PendingRequestTable.h"

#include <algorithm>

namespace hwsim::protocol {

quint64 PendingRequestTable::add(QString correlationKey, MessagePtr request, Callback callback,
                                 int timeoutMs, qint64 nowMs)
{
    Entry entry;
    entry.id = nextId_++;
    entry.correlationKey = std::move(correlationKey);
    entry.request = std::move(request);
    entry.callback = std::move(callback);
    entry.timeoutMs = timeoutMs;
    entry.deadlineMs = nowMs + timeoutMs;

    const quint64 id = entry.id;
    entries_.push_back(std::move(entry));
    return id;
}

bool PendingRequestTable::resolve(const QString& correlationKey, MessagePtr response)
{
    if (entries_.empty()) {
        return false;
    }

    auto match = entries_.end();
    if (correlationKey.isEmpty()) {
        match = entries_.begin();
    } else {
        match = std::find_if(entries_.begin(), entries_.end(), [&correlationKey](const Entry& entry) {
            return entry.correlationKey == correlationKey;
        });
        // A codec that produces keys may still receive a reply without one;
        // fall back to oldest-first rather than dropping the response.
        if (match == entries_.end()) {
            match = entries_.begin();
        }
    }

    Entry entry = std::move(*match);
    entries_.erase(match);

    if (entry.callback) {
        entry.callback(core::Result<MessagePtr>(std::move(response)));
    }
    return true;
}

std::vector<PendingRequestTable::Entry> PendingRequestTable::takeExpired(qint64 nowMs)
{
    std::vector<Entry> expired;

    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->deadlineMs <= nowMs) {
            expired.push_back(std::move(*it));
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }

    for (Entry& entry : expired) {
        if (entry.callback) {
            entry.callback(core::makeError(
                core::ErrorCode::Timeout,
                QStringLiteral("no response within %1 ms").arg(entry.timeoutMs),
                entry.request ? entry.request->describe() : QString{}));
        }
    }
    return expired;
}

void PendingRequestTable::failAll(const core::Error& error)
{
    std::deque<Entry> pending;
    pending.swap(entries_);

    for (Entry& entry : pending) {
        if (entry.callback) {
            entry.callback(core::Result<MessagePtr>(error));
        }
    }
}

bool PendingRequestTable::cancel(quint64 id)
{
    const auto match = std::find_if(entries_.begin(), entries_.end(),
                                    [id](const Entry& entry) { return entry.id == id; });
    if (match == entries_.end()) {
        return false;
    }
    entries_.erase(match);
    return true;
}

qint64 PendingRequestTable::nextDeadlineMs() const
{
    qint64 earliest = 0;
    for (const Entry& entry : entries_) {
        if (earliest == 0 || entry.deadlineMs < earliest) {
            earliest = entry.deadlineMs;
        }
    }
    return earliest;
}

} // namespace hwsim::protocol
