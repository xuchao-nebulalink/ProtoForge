#include "ParameterStore.h"

#include <core/Clock.h>

#include <algorithm>

using hwsim::core::ErrorCode;
using hwsim::core::makeError;
using hwsim::core::Result;

namespace hwsim::simulator {

ParameterStore::ParameterStore(QString deviceName) : deviceName_(std::move(deviceName)) {}

void ParameterStore::setDeviceName(QString name)
{
    std::unique_lock lock(mutex_);
    deviceName_ = std::move(name);
}

QString ParameterStore::deviceName() const
{
    std::shared_lock lock(mutex_);
    return deviceName_;
}

// --- Definition ------------------------------------------------------------

Result<void> ParameterStore::define(ParameterDefinition definition)
{
    if (definition.key.isEmpty()) {
        return makeError(ErrorCode::ConfigInvalid, QStringLiteral("parameter key must not be empty"));
    }

    std::unique_lock lock(mutex_);

    if (parameters_.contains(definition.key)) {
        return makeError(ErrorCode::AlreadyExists,
                         QStringLiteral("parameter '%1' is already defined").arg(definition.key));
    }
    if (definition.hasAddress && byAddress_.contains(definition.address)) {
        return makeError(ErrorCode::AlreadyExists,
                         QStringLiteral("address %1 is already used by '%2'")
                             .arg(definition.address)
                             .arg(byAddress_.at(definition.address)->key()));
    }

    const QString key = definition.key;
    const bool hasAddress = definition.hasAddress;
    const quint32 address = definition.address;

    auto parameter = std::make_unique<Parameter>(std::move(definition));
    Parameter* raw = parameter.get();
    parameters_.emplace(key, std::move(parameter));
    if (hasAddress) {
        byAddress_.emplace(address, raw);
    }
    return core::success();
}

Result<void> ParameterStore::defineAll(const QVector<ParameterDefinition>& definitions)
{
    for (const ParameterDefinition& definition : definitions) {
        if (const auto defined = define(definition); defined.hasError()) {
            return defined;
        }
    }
    return core::success();
}

bool ParameterStore::remove(const QString& key)
{
    std::unique_lock lock(mutex_);
    const auto it = parameters_.find(key);
    if (it == parameters_.end()) {
        return false;
    }
    if (it->second->definition().hasAddress) {
        byAddress_.erase(it->second->definition().address);
    }
    parameters_.erase(it);
    return true;
}

void ParameterStore::clear()
{
    std::unique_lock lock(mutex_);
    byAddress_.clear();
    parameters_.clear();
}

bool ParameterStore::contains(const QString& key) const
{
    std::shared_lock lock(mutex_);
    return parameters_.contains(key);
}

bool ParameterStore::containsAddress(quint32 address) const
{
    std::shared_lock lock(mutex_);
    return byAddress_.contains(address);
}

qsizetype ParameterStore::size() const
{
    std::shared_lock lock(mutex_);
    return static_cast<qsizetype>(parameters_.size());
}

// --- Access ----------------------------------------------------------------

Result<QVariant> ParameterStore::read(const QString& key) const
{
    std::shared_lock lock(mutex_);
    const auto it = parameters_.find(key);
    if (it == parameters_.end()) {
        return makeError(ErrorCode::NotFound,
                         QStringLiteral("no parameter named '%1'").arg(key), deviceName_);
    }
    return it->second->value();
}

Result<QVariant> ParameterStore::readAddress(quint32 address) const
{
    std::shared_lock lock(mutex_);
    const auto it = byAddress_.find(address);
    if (it == byAddress_.end()) {
        return makeError(ErrorCode::OutOfRange,
                         QStringLiteral("no parameter at address %1").arg(address), deviceName_);
    }
    return it->second->value();
}

Result<QVector<QVariant>> ParameterStore::readAddressRange(quint32 startAddress, quint32 count) const
{
    std::shared_lock lock(mutex_);

    QVector<QVariant> values;
    values.reserve(static_cast<qsizetype>(count));

    for (quint32 offset = 0; offset < count; ++offset) {
        const auto it = byAddress_.find(startAddress + offset);
        if (it == byAddress_.end()) {
            return makeError(ErrorCode::OutOfRange,
                             QStringLiteral("no parameter at address %1").arg(startAddress + offset),
                             deviceName_);
        }
        values.append(it->second->value());
    }
    return values;
}

Result<void> ParameterStore::applyWrite(Parameter& parameter, const QVariant& value,
                                        WriteOrigin origin, ParameterChange& change)
{
    const QVariant previous = parameter.value();

    if (const auto written = parameter.setValue(value, origin); written.hasError()) {
        return written;
    }

    change.deviceName = deviceName_;
    change.key = parameter.key();
    change.address = parameter.definition().address;
    change.hasAddress = parameter.definition().hasAddress;
    change.previousValue = previous;
    change.newValue = parameter.value();
    change.origin = origin;
    change.timestampMs = core::wallClockMs();
    return core::success();
}

Result<void> ParameterStore::write(const QString& key, const QVariant& value, WriteOrigin origin)
{
    ParameterChange change;
    bool changed = false;

    {
        std::unique_lock lock(mutex_);
        const auto it = parameters_.find(key);
        if (it == parameters_.end()) {
            return makeError(ErrorCode::NotFound,
                             QStringLiteral("no parameter named '%1'").arg(key), deviceName_);
        }
        if (const auto applied = applyWrite(*it->second, value, origin, change); applied.hasError()) {
            return applied;
        }
        changed = change.previousValue != change.newValue;
    }

    // Notifications go out with the lock released so a handler is free to read
    // the store, or write another parameter, without deadlocking.
    if (changed) {
        notify(change);
    }
    return core::success();
}

Result<void> ParameterStore::writeAddress(quint32 address, const QVariant& value, WriteOrigin origin)
{
    ParameterChange change;
    bool changed = false;

    {
        std::unique_lock lock(mutex_);
        const auto it = byAddress_.find(address);
        if (it == byAddress_.end()) {
            return makeError(ErrorCode::OutOfRange,
                             QStringLiteral("no parameter at address %1").arg(address), deviceName_);
        }
        if (const auto applied = applyWrite(*it->second, value, origin, change); applied.hasError()) {
            return applied;
        }
        changed = change.previousValue != change.newValue;
    }

    if (changed) {
        notify(change);
    }
    return core::success();
}

Result<void> ParameterStore::writeAddressRange(quint32 startAddress, const QVector<QVariant>& values,
                                               WriteOrigin origin)
{
    std::vector<ParameterChange> changes;
    changes.reserve(static_cast<std::size_t>(values.size()));

    Result<void> outcome = core::success();

    {
        std::unique_lock lock(mutex_);

        // Validate the whole range first: a partially applied multi-register
        // write would leave the device in a state the master never asked for.
        for (qsizetype index = 0; index < values.size(); ++index) {
            const quint32 address = startAddress + static_cast<quint32>(index);
            const auto it = byAddress_.find(address);
            if (it == byAddress_.end()) {
                return makeError(ErrorCode::OutOfRange,
                                 QStringLiteral("no parameter at address %1").arg(address),
                                 deviceName_);
            }
            if (origin == WriteOrigin::Protocol
                && it->second->definition().access == AccessMode::ReadOnly) {
                return makeError(ErrorCode::ParameterReadOnly,
                                 QStringLiteral("parameter '%1' is read-only")
                                     .arg(it->second->key()),
                                 deviceName_);
            }
            if (const auto coerced = it->second->coerce(values.at(index)); coerced.hasError()) {
                return coerced.error();
            }
        }

        for (qsizetype index = 0; index < values.size(); ++index) {
            Parameter* parameter = byAddress_.at(startAddress + static_cast<quint32>(index));
            ParameterChange change;
            if (const auto applied = applyWrite(*parameter, values.at(index), origin, change);
                applied.hasError()) {
                // Pre-validation should make this unreachable, but returning
                // here without notifying would leave already-written registers
                // invisible to the UI, the event bus and the signal engine.
                outcome = applied;
                break;
            }
            if (change.previousValue != change.newValue) {
                changes.push_back(std::move(change));
            }
        }
    }

    for (const ParameterChange& change : changes) {
        notify(change);
    }
    return outcome;
}

// --- Introspection ---------------------------------------------------------

std::optional<ParameterDefinition> ParameterStore::definitionOf(const QString& key) const
{
    std::shared_lock lock(mutex_);
    const auto it = parameters_.find(key);
    return it == parameters_.end() ? std::nullopt
                                   : std::optional<ParameterDefinition>{it->second->definition()};
}

std::optional<ParameterDefinition> ParameterStore::definitionAt(quint32 address) const
{
    std::shared_lock lock(mutex_);
    const auto it = byAddress_.find(address);
    return it == byAddress_.end() ? std::nullopt
                                  : std::optional<ParameterDefinition>{it->second->definition()};
}

QStringList ParameterStore::keys() const
{
    std::shared_lock lock(mutex_);
    QStringList result;
    result.reserve(static_cast<qsizetype>(parameters_.size()));
    for (const auto& [key, parameter] : parameters_) {
        result.append(key);
    }
    return result;
}

QStringList ParameterStore::groups() const
{
    std::shared_lock lock(mutex_);
    QStringList result;
    for (const auto& [key, parameter] : parameters_) {
        const QString& group = parameter->definition().group;
        if (!result.contains(group)) {
            result.append(group);
        }
    }
    return result;
}

QVector<ParameterDefinition> ParameterStore::definitions() const
{
    std::shared_lock lock(mutex_);
    QVector<ParameterDefinition> result;
    result.reserve(static_cast<qsizetype>(parameters_.size()));
    for (const auto& [key, parameter] : parameters_) {
        result.append(parameter->definition());
    }
    return result;
}

QVariantMap ParameterStore::snapshot() const
{
    std::shared_lock lock(mutex_);
    QVariantMap result;
    for (const auto& [key, parameter] : parameters_) {
        result.insert(key, parameter->value());
    }
    return result;
}

Result<void> ParameterStore::restore(const QVariantMap& values)
{
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        if (!contains(it.key())) {
            continue;  // profile has a parameter this device no longer defines
        }
        if (const auto written = write(it.key(), it.value(), WriteOrigin::Restore);
            written.hasError()) {
            return written;
        }
    }
    return core::success();
}

void ParameterStore::resetToDefaults()
{
    std::vector<ParameterChange> changes;

    {
        std::unique_lock lock(mutex_);
        for (const auto& [key, parameter] : parameters_) {
            const QVariant previous = parameter->value();
            parameter->resetToDefault();
            if (previous != parameter->value()) {
                ParameterChange change;
                change.deviceName = deviceName_;
                change.key = key;
                change.address = parameter->definition().address;
                change.hasAddress = parameter->definition().hasAddress;
                change.previousValue = previous;
                change.newValue = parameter->value();
                change.origin = WriteOrigin::Restore;
                change.timestampMs = core::wallClockMs();
                changes.push_back(std::move(change));
            }
        }
    }

    for (const ParameterChange& change : changes) {
        notify(change);
    }
}

// --- Notification ----------------------------------------------------------

ParameterStore::HandlerId ParameterStore::onChanged(ChangeHandler handler)
{
    if (!handler) {
        return 0;
    }
    std::unique_lock lock(handlerMutex_);
    const HandlerId id = nextHandlerId_++;
    handlers_.push_back(HandlerEntry{id, std::move(handler)});
    return id;
}

void ParameterStore::removeHandler(HandlerId id)
{
    std::unique_lock lock(handlerMutex_);
    std::erase_if(handlers_, [id](const HandlerEntry& entry) { return entry.id == id; });
}

void ParameterStore::notify(const ParameterChange& change) const
{
    std::vector<ChangeHandler> snapshot;
    {
        std::shared_lock lock(handlerMutex_);
        snapshot.reserve(handlers_.size());
        for (const HandlerEntry& entry : handlers_) {
            snapshot.push_back(entry.handler);
        }
    }
    for (const ChangeHandler& handler : snapshot) {
        handler(change);
    }
}

// --- Persistence -----------------------------------------------------------

QJsonArray ParameterStore::definitionsToJson() const
{
    std::shared_lock lock(mutex_);
    QJsonArray array;
    for (const auto& [key, parameter] : parameters_) {
        array.append(parameter->definition().toJson());
    }
    return array;
}

Result<void> ParameterStore::loadDefinitions(const QJsonArray& json)
{
    for (const QJsonValue& value : json) {
        if (!value.isObject()) {
            return makeError(ErrorCode::ConfigInvalid,
                             QStringLiteral("parameter list contains a non-object entry"));
        }
        const auto definition = ParameterDefinition::fromJson(value.toObject());
        if (definition.hasError()) {
            return definition.error();
        }
        if (const auto defined = define(definition.value()); defined.hasError()) {
            return defined;
        }
    }
    return core::success();
}

} // namespace hwsim::simulator
