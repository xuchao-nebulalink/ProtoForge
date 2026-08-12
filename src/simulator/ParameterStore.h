#pragma once

#include "Parameter.h"

#include <QJsonArray>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

#include <functional>
#include <map>
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace hwsim::simulator {

/// The device's data model: every parameter, indexed both by name and by
/// numeric address.
///
/// Two indices because the two consumers address differently. Register
/// protocols such as Modbus arrive with an address; scripts, the UI and
/// tag-oriented protocols use the name. Keeping one store with two views avoids
/// the classic split where the register map and the tag list drift apart.
///
/// This is the one class in the device pipeline that takes a lock. Everything
/// else runs single-threaded on the device's I/O thread, but the parameter
/// store is the natural sharing point: scripts and the UI read snapshots from
/// their own threads while protocol handlers write from the I/O thread.
class HWSIM_SIMULATOR_API ParameterStore {
public:
    using ChangeHandler = std::function<void(const ParameterChange&)>;
    using HandlerId = quint64;

    ParameterStore() = default;
    explicit ParameterStore(QString deviceName);

    void setDeviceName(QString name);
    [[nodiscard]] QString deviceName() const;

    // --- Definition ---

    [[nodiscard]] core::Result<void> define(ParameterDefinition definition);
    [[nodiscard]] core::Result<void> defineAll(const QVector<ParameterDefinition>& definitions);
    bool remove(const QString& key);
    void clear();

    [[nodiscard]] bool contains(const QString& key) const;
    [[nodiscard]] bool containsAddress(quint32 address) const;
    [[nodiscard]] qsizetype size() const;

    // --- Access ---

    [[nodiscard]] core::Result<QVariant> read(const QString& key) const;
    [[nodiscard]] core::Result<QVariant> readAddress(quint32 address) const;
    [[nodiscard]] core::Result<QVector<QVariant>> readAddressRange(quint32 startAddress,
                                                                   quint32 count) const;

    [[nodiscard]] core::Result<void> write(const QString& key, const QVariant& value,
                                           WriteOrigin origin);
    [[nodiscard]] core::Result<void> writeAddress(quint32 address, const QVariant& value,
                                                  WriteOrigin origin);
    [[nodiscard]] core::Result<void> writeAddressRange(quint32 startAddress,
                                                       const QVector<QVariant>& values,
                                                       WriteOrigin origin);

    // --- Introspection ---

    [[nodiscard]] std::optional<ParameterDefinition> definitionOf(const QString& key) const;
    [[nodiscard]] std::optional<ParameterDefinition> definitionAt(quint32 address) const;
    [[nodiscard]] QStringList keys() const;
    [[nodiscard]] QStringList groups() const;
    [[nodiscard]] QVector<ParameterDefinition> definitions() const;

    /// key -> current value, for the UI table and for profile snapshots.
    [[nodiscard]] QVariantMap snapshot() const;
    [[nodiscard]] core::Result<void> restore(const QVariantMap& values);

    void resetToDefaults();

    // --- Notification ---

    /// Handlers run on whichever thread performed the write, after the store
    /// lock has been released.
    HandlerId onChanged(ChangeHandler handler);
    void removeHandler(HandlerId id);

    // --- Persistence ---

    [[nodiscard]] QJsonArray definitionsToJson() const;
    [[nodiscard]] core::Result<void> loadDefinitions(const QJsonArray& json);

private:
    struct HandlerEntry {
        HandlerId id;
        ChangeHandler handler;
    };

    [[nodiscard]] core::Result<void> applyWrite(Parameter& parameter, const QVariant& value,
                                                WriteOrigin origin, ParameterChange& change);
    void notify(const ParameterChange& change) const;

    mutable std::shared_mutex mutex_;
    QString deviceName_;

    /// Ordered so that the UI and JSON output keep a stable, address-sorted view.
    std::map<QString, std::unique_ptr<Parameter>> parameters_;
    std::unordered_map<quint32, Parameter*> byAddress_;

    mutable std::shared_mutex handlerMutex_;
    std::vector<HandlerEntry> handlers_;
    HandlerId nextHandlerId_{1};
};

} // namespace hwsim::simulator
