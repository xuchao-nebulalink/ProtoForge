#pragma once

#include "IProtocolPlugin.h"

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

namespace hwsim::protocol {

/// Discovers and owns protocol plugins.
///
/// Both linkage modes end up in the same table: dynamic plugins are found by
/// scanning a directory with QPluginLoader, static ones are handed in by the
/// generated StaticPluginImports.cpp. Everything downstream looks plugins up by
/// id and cannot tell the difference, so switching a plugin between static and
/// dynamic is purely a build-time decision.
///
/// A plugin that fails to load never takes the application down with it: the
/// reason is recorded in failures() and shown in the UI.
class HWSIM_PROTOCOL_API PluginManager : public QObject {
    Q_OBJECT

public:
    using StaticFactory = QObject* (*)();

    struct LoadedPlugin {
        PluginMetadata metadata;
        IProtocolPlugin* plugin{nullptr};
        QString filePath;
        bool isStatic{false};
    };

    struct LoadFailure {
        QString source;
        QString reason;
    };

    explicit PluginManager(QObject* parent = nullptr);
    ~PluginManager() override;

    /// Registers a statically linked plugin. Called by the generated import
    /// file before loadFromDirectory().
    void addStaticPlugin(const QString& id, StaticFactory factory);

    /// Instantiates everything registered through addStaticPlugin().
    int instantiateStaticPlugins();

    /// Loads every shared library in `directory` that exposes the plugin
    /// interface. Returns the number successfully loaded.
    int loadFromDirectory(const QString& directory);

    /// Convenience: instantiates static plugins, then scans each directory.
    int loadAll(const QStringList& directories);

    [[nodiscard]] QList<LoadedPlugin> plugins() const;
    [[nodiscard]] IProtocolPlugin* find(const QString& id) const;
    [[nodiscard]] QStringList ids() const;
    [[nodiscard]] bool contains(const QString& id) const;
    [[nodiscard]] QList<LoadFailure> failures() const { return failures_; }

    /// bin/plugins next to the running executable.
    [[nodiscard]] static QString defaultPluginDirectory();

signals:
    void pluginLoaded(const QString& id);
    void pluginRejected(const QString& source, const QString& reason);

private:
    /// Validates ABI and id uniqueness, then takes the plugin into the table.
    bool accept(QObject* instance, const QString& source, bool isStatic);

    struct StaticEntry {
        QString id;
        StaticFactory factory;
    };

    QList<LoadedPlugin> plugins_;
    QList<LoadFailure> failures_;
    std::vector<StaticEntry> staticFactories_;
    std::vector<std::unique_ptr<QObject>> ownedInstances_;
    bool staticPluginsInstantiated_{false};
};

/// Defined by the CMake-generated StaticPluginImports.cpp that is compiled into
/// the application. Declared here so the composition root can call it without
/// knowing which plugins the build selected. Not exported: the definition lives
/// in the executable, not in this library.
void registerStaticPlugins(PluginManager& manager);

} // namespace hwsim::protocol
