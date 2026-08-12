#pragma once

#include "CoreGlobal.h"
#include "TypeId.h"

#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QThread>

#include <functional>
#include <memory>
#include <shared_mutex>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hwsim::core {

using SubscriptionId = quint64;

/// Typed publish/subscribe hub.
///
/// The alternative would be a web of signal/slot connections between layers,
/// which forces every publisher to know its consumers. Here the event type is
/// the contract: transport publishes LinkStateChanged, and the log dock, the
/// device tree and the statistics collector each subscribe independently.
///
/// Subscribing with a QObject context makes delivery thread-affine: the handler
/// runs on the context's thread through a queued invocation, so UI subscribers
/// can be written without any locking even though events originate on transport
/// worker threads.
class HWSIM_CORE_API EventBus {
public:
    EventBus() = default;
    ~EventBus() = default;
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    /// Application-wide instance. Tests normally construct their own bus.
    [[nodiscard]] static EventBus& global();

    /// Handler runs on whichever thread publishes the event.
    template <typename E>
    SubscriptionId subscribe(std::function<void(const E&)> handler)
    {
        return subscribeImpl<E>(nullptr, std::move(handler));
    }

    /// Handler runs on `context`'s thread and stops being called once the
    /// context is destroyed.
    ///
    /// The guard is a QPointer, which is not thread safe, so this is only sound
    /// when the context is destroyed on the same thread that publishes, or when
    /// cross-thread delivery goes through the queued path (Qt discards queued
    /// events for a receiver that dies first). A subscriber that is destroyed
    /// on one thread while another publishes should capture a shared_ptr to its
    /// state instead of relying on the context guard.
    template <typename E>
    SubscriptionId subscribe(QObject* context, std::function<void(const E&)> handler)
    {
        return subscribeImpl<E>(context, std::move(handler));
    }

    template <typename E>
    void publish(const E& event) const;

    /// Removes a subscription.
    ///
    /// IMPORTANT: publish() copies the subscriber list and then releases the
    /// lock before invoking handlers, so that a handler is free to subscribe,
    /// unsubscribe or publish without deadlocking. The consequence is that a
    /// handler may still be executing when unsubscribe() returns.
    ///
    /// A handler must therefore not capture a raw pointer to an object that can
    /// be destroyed. Either subscribe with a QObject context, or capture a
    /// shared_ptr to the state the handler touches.
    void unsubscribe(SubscriptionId id);

    void clear();

    [[nodiscard]] std::size_t subscriberCount(TypeId type) const;

    template <typename E>
    [[nodiscard]] std::size_t subscriberCount() const
    {
        return subscriberCount(TypeId::of<E>());
    }

private:
    struct Entry {
        SubscriptionId id{0};
        QPointer<QObject> context;
        bool hasContext{false};
        std::shared_ptr<void> handler;
    };

    template <typename E>
    SubscriptionId subscribeImpl(QObject* context, std::function<void(const E&)> handler)
    {
        Entry entry;
        entry.hasContext = context != nullptr;
        entry.context = context;
        entry.handler = std::make_shared<std::function<void(const E&)>>(std::move(handler));
        return addEntry(TypeId::of<E>(), std::move(entry));
    }

    SubscriptionId addEntry(TypeId type, Entry entry);
    [[nodiscard]] std::vector<Entry> snapshot(TypeId type) const;

    mutable std::shared_mutex mutex_;
    std::unordered_map<TypeId, std::vector<Entry>> subscribers_;
    SubscriptionId nextId_{1};
};

template <typename E>
void EventBus::publish(const E& event) const
{
    static_assert(std::is_copy_constructible_v<E>,
                  "events must be copyable so they can be queued to another thread");

    using Handler = std::function<void(const E&)>;

    const std::vector<Entry> entries = snapshot(TypeId::of<E>());
    for (const Entry& entry : entries) {
        const auto* handler = static_cast<const Handler*>(entry.handler.get());
        if (handler == nullptr || !*handler) {
            continue;
        }

        if (!entry.hasContext) {
            (*handler)(event);
            continue;
        }

        QObject* context = entry.context.data();
        if (context == nullptr) {
            continue;
        }
        if (context->thread() == QThread::currentThread()) {
            (*handler)(event);
            continue;
        }

        // The shared_ptr copy keeps the handler alive until the queued call runs.
        auto keepAlive = entry.handler;
        QMetaObject::invokeMethod(
            context,
            [keepAlive, copy = event]() {
                (*static_cast<const Handler*>(keepAlive.get()))(copy);
            },
            Qt::QueuedConnection);
    }
}

/// RAII wrapper so subscribers do not have to remember an id.
class HWSIM_CORE_API ScopedSubscription {
public:
    ScopedSubscription() = default;
    ScopedSubscription(EventBus* bus, SubscriptionId id) : bus_(bus), id_(id) {}
    ~ScopedSubscription() { reset(); }

    ScopedSubscription(const ScopedSubscription&) = delete;
    ScopedSubscription& operator=(const ScopedSubscription&) = delete;

    ScopedSubscription(ScopedSubscription&& other) noexcept
        : bus_(std::exchange(other.bus_, nullptr)), id_(std::exchange(other.id_, 0))
    {
    }

    ScopedSubscription& operator=(ScopedSubscription&& other) noexcept
    {
        if (this != &other) {
            reset();
            bus_ = std::exchange(other.bus_, nullptr);
            id_ = std::exchange(other.id_, 0);
        }
        return *this;
    }

    void reset()
    {
        if (bus_ != nullptr && id_ != 0) {
            bus_->unsubscribe(id_);
        }
        bus_ = nullptr;
        id_ = 0;
    }

    [[nodiscard]] SubscriptionId id() const noexcept { return id_; }
    [[nodiscard]] bool isActive() const noexcept { return bus_ != nullptr && id_ != 0; }

private:
    EventBus* bus_{nullptr};
    SubscriptionId id_{0};
};

} // namespace hwsim::core
