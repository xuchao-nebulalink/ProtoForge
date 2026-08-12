#include "EventBus.h"

#include <algorithm>

namespace hwsim::core {

EventBus& EventBus::global()
{
    static EventBus bus;
    return bus;
}

SubscriptionId EventBus::addEntry(TypeId type, Entry entry)
{
    std::unique_lock lock(mutex_);

    auto& entries = subscribers_[type];

    // A context-bound subscription leaves a null QPointer behind when its
    // context is destroyed. Sweeping the list for this event type on every new
    // subscription to it keeps that list from growing for the whole session as
    // panels and dialogs come and go. Event types nobody subscribes to again
    // are not swept, so subscriberCount() can still read high for those.
    std::erase_if(entries, [](const Entry& existing) {
        return existing.hasContext && existing.context.isNull();
    });

    entry.id = nextId_++;
    const SubscriptionId id = entry.id;
    entries.push_back(std::move(entry));
    return id;
}

std::vector<EventBus::Entry> EventBus::snapshot(TypeId type) const
{
    std::shared_lock lock(mutex_);
    const auto it = subscribers_.find(type);
    return it == subscribers_.end() ? std::vector<Entry>{} : it->second;
}

void EventBus::unsubscribe(SubscriptionId id)
{
    std::unique_lock lock(mutex_);
    for (auto& [type, entries] : subscribers_) {
        const auto removed = std::erase_if(
            entries, [id](const Entry& entry) { return entry.id == id; });
        if (removed > 0) {
            return;
        }
    }
}

void EventBus::clear()
{
    std::unique_lock lock(mutex_);
    subscribers_.clear();
}

std::size_t EventBus::subscriberCount(TypeId type) const
{
    std::shared_lock lock(mutex_);
    const auto it = subscribers_.find(type);
    return it == subscribers_.end() ? 0 : it->second.size();
}

} // namespace hwsim::core
