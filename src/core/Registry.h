#pragma once

#include "CoreGlobal.h"
#include "Result.h"

#include <QHashFunctions>
#include <QString>

#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hwsim::core {

/// Hash adaptor so Qt value types work as unordered_map keys without relying on
/// whichever std::hash specialisations a given Qt version happens to ship.
struct QtKeyHash {
    template <typename T>
    [[nodiscard]] std::size_t operator()(const T& key) const noexcept
    {
        return static_cast<std::size_t>(qHash(key));
    }
};

/// Generic creator table.
///
/// Used wherever the framework has to turn a run-time identifier into an
/// object: transports ("tcp-server"), signal sources ("sine"), fault rules
/// ("packet-loss"), codecs. Adding a kind is one registration call at start-up
/// rather than a new branch in a factory function, which is the same principle
/// the command dispatcher applies to protocol messages.
template <typename Key, typename Base, typename... Args>
class Registry {
public:
    using Creator = std::function<std::unique_ptr<Base>(Args...)>;

    struct Entry {
        Key key;
        QString displayName;
        QString description;
        Creator creator;
    };

    bool add(Key key, QString displayName, Creator creator, QString description = {})
    {
        if (!creator) {
            return false;
        }
        std::unique_lock lock(mutex_);
        if (entries_.contains(key)) {
            return false;
        }
        Entry entry{key, std::move(displayName), std::move(description), std::move(creator)};
        entries_.emplace(std::move(key), std::move(entry));
        return true;
    }

    /// Convenience overload for the common "default-construct a derived type" case.
    template <typename Derived>
    bool addType(Key key, QString displayName, QString description = {})
    {
        static_assert(std::is_base_of_v<Base, Derived>, "Derived must inherit Base");
        return add(std::move(key), std::move(displayName),
                   [](Args... args) -> std::unique_ptr<Base> {
                       return std::make_unique<Derived>(std::forward<Args>(args)...);
                   },
                   std::move(description));
    }

    bool remove(const Key& key)
    {
        std::unique_lock lock(mutex_);
        return entries_.erase(key) > 0;
    }

    [[nodiscard]] bool contains(const Key& key) const
    {
        std::shared_lock lock(mutex_);
        return entries_.contains(key);
    }

    [[nodiscard]] Result<std::unique_ptr<Base>> create(const Key& key, Args... args) const
    {
        Creator creator;
        {
            std::shared_lock lock(mutex_);
            const auto it = entries_.find(key);
            if (it == entries_.end()) {
                return makeError(ErrorCode::NotFound,
                                 QStringLiteral("no registered entry for '%1'").arg(keyToString(key)));
            }
            creator = it->second.creator;
        }

        auto instance = creator(std::forward<Args>(args)...);
        if (!instance) {
            return makeError(ErrorCode::Internal,
                             QStringLiteral("creator for '%1' returned null").arg(keyToString(key)));
        }
        return instance;
    }

    [[nodiscard]] std::vector<Key> keys() const
    {
        std::shared_lock lock(mutex_);
        std::vector<Key> result;
        result.reserve(entries_.size());
        for (const auto& [key, entry] : entries_) {
            result.push_back(key);
        }
        return result;
    }

    [[nodiscard]] std::vector<Entry> entries() const
    {
        std::shared_lock lock(mutex_);
        std::vector<Entry> result;
        result.reserve(entries_.size());
        for (const auto& [key, entry] : entries_) {
            result.push_back(entry);
        }
        return result;
    }

    [[nodiscard]] std::optional<Entry> find(const Key& key) const
    {
        std::shared_lock lock(mutex_);
        const auto it = entries_.find(key);
        return it == entries_.end() ? std::nullopt : std::optional<Entry>{it->second};
    }

    [[nodiscard]] std::size_t size() const
    {
        std::shared_lock lock(mutex_);
        return entries_.size();
    }

    void clear()
    {
        std::unique_lock lock(mutex_);
        entries_.clear();
    }

private:
    [[nodiscard]] static QString keyToString(const Key& key)
    {
        if constexpr (std::is_convertible_v<Key, QString>) {
            return QString(key);
        } else {
            return QStringLiteral("<key>");
        }
    }

    mutable std::shared_mutex mutex_;
    std::unordered_map<Key, Entry, QtKeyHash> entries_;
};

} // namespace hwsim::core
