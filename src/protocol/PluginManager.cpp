#include "PluginManager.h"

#include <core/Logger.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonObject>
#include <QLibrary>
#include <QPluginLoader>

#include <algorithm>

namespace {
constexpr auto kLogCategory = "protocol.plugins";
}

namespace hwsim::protocol {

PluginManager::PluginManager(QObject* parent) : QObject(parent) {}

PluginManager::~PluginManager() = default;

void PluginManager::addStaticPlugin(const QString& id, StaticFactory factory)
{
    if (factory == nullptr) {
        return;
    }
    const bool duplicate = std::any_of(staticFactories_.begin(), staticFactories_.end(),
                                       [&id](const StaticEntry& entry) { return entry.id == id; });
    if (duplicate) {
        return;
    }
    staticFactories_.push_back(StaticEntry{id, factory});
}

int PluginManager::instantiateStaticPlugins()
{
    if (staticPluginsInstantiated_) {
        return 0;
    }
    staticPluginsInstantiated_ = true;

    int loaded = 0;
    for (const StaticEntry& entry : staticFactories_) {
        auto instance = std::unique_ptr<QObject>(entry.factory());
        if (!instance) {
            failures_.append(LoadFailure{entry.id, QStringLiteral("static factory returned null")});
            emit pluginRejected(entry.id, QStringLiteral("static factory returned null"));
            continue;
        }

        QObject* raw = instance.get();
        ownedInstances_.push_back(std::move(instance));

        if (accept(raw, QStringLiteral("<static:%1>").arg(entry.id), true)) {
            ++loaded;
        }
    }
    return loaded;
}

int PluginManager::loadFromDirectory(const QString& directory)
{
    QDir dir(directory);
    if (!dir.exists()) {
        HWSIM_LOG_DEBUG(kLogCategory) << "plugin directory does not exist: " << directory;
        return 0;
    }

    int loaded = 0;
    const QFileInfoList entries = dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);

    for (const QFileInfo& entry : entries) {
        const QString path = entry.absoluteFilePath();
        if (!QLibrary::isLibrary(path)) {
            continue;
        }

        QPluginLoader loader(path);

        // Check the declared IID before instantiating, so a library from an
        // incompatible build is rejected without running any of its code.
        const QJsonObject metaData = loader.metaData();
        const QString iid = metaData.value(QStringLiteral("IID")).toString();
        if (iid != QLatin1String(HWSIM_PROTOCOL_PLUGIN_IID)) {
            const QString reason =
                QStringLiteral("interface id '%1' does not match '%2'")
                    .arg(iid.isEmpty() ? QStringLiteral("<none>") : iid,
                         QLatin1String(HWSIM_PROTOCOL_PLUGIN_IID));
            failures_.append(LoadFailure{path, reason});
            emit pluginRejected(path, reason);
            continue;
        }

        QObject* instance = loader.instance();
        if (instance == nullptr) {
            const QString reason = loader.errorString();
            failures_.append(LoadFailure{path, reason});
            emit pluginRejected(path, reason);
            continue;
        }

        if (accept(instance, path, false)) {
            ++loaded;
        } else {
            loader.unload();
        }
    }

    HWSIM_LOG_INFO(kLogCategory) << "loaded " << loaded << " plugin(s) from " << directory;
    return loaded;
}

int PluginManager::loadAll(const QStringList& directories)
{
    int loaded = instantiateStaticPlugins();
    for (const QString& directory : directories) {
        loaded += loadFromDirectory(directory);
    }
    return loaded;
}

bool PluginManager::accept(QObject* instance, const QString& source, bool isStatic)
{
    auto* plugin = qobject_cast<IProtocolPlugin*>(instance);
    if (plugin == nullptr) {
        const QString reason = QStringLiteral("does not implement IProtocolPlugin");
        failures_.append(LoadFailure{source, reason});
        emit pluginRejected(source, reason);
        return false;
    }

    if (plugin->abiVersion() != HWSIM_PLUGIN_ABI_VERSION) {
        const QString reason = QStringLiteral("ABI version %1 does not match host version %2")
                                   .arg(plugin->abiVersion())
                                   .arg(HWSIM_PLUGIN_ABI_VERSION);
        failures_.append(LoadFailure{source, reason});
        emit pluginRejected(source, reason);
        return false;
    }

    const PluginMetadata metadata = plugin->metadata();
    if (metadata.id.isEmpty()) {
        const QString reason = QStringLiteral("plugin metadata has an empty id");
        failures_.append(LoadFailure{source, reason});
        emit pluginRejected(source, reason);
        return false;
    }

    if (contains(metadata.id)) {
        const QString reason =
            QStringLiteral("a plugin with id '%1' is already loaded").arg(metadata.id);
        failures_.append(LoadFailure{source, reason});
        emit pluginRejected(source, reason);
        return false;
    }

    LoadedPlugin loaded;
    loaded.metadata = metadata;
    loaded.plugin = plugin;
    loaded.filePath = isStatic ? QString{} : source;
    loaded.isStatic = isStatic;
    plugins_.append(loaded);

    HWSIM_LOG_INFO(kLogCategory)
        << "registered plugin '" << metadata.id << "' v" << metadata.version
        << (isStatic ? " (static)" : " (dynamic)");

    emit pluginLoaded(metadata.id);
    return true;
}

QList<PluginManager::LoadedPlugin> PluginManager::plugins() const
{
    return plugins_;
}

IProtocolPlugin* PluginManager::find(const QString& id) const
{
    const auto it = std::find_if(plugins_.begin(), plugins_.end(),
                                 [&id](const LoadedPlugin& entry) {
                                     return entry.metadata.id == id;
                                 });
    return it == plugins_.end() ? nullptr : it->plugin;
}

QStringList PluginManager::ids() const
{
    QStringList result;
    result.reserve(plugins_.size());
    for (const LoadedPlugin& entry : plugins_) {
        result.append(entry.metadata.id);
    }
    return result;
}

bool PluginManager::contains(const QString& id) const
{
    return find(id) != nullptr;
}

QString PluginManager::defaultPluginDirectory()
{
    return QCoreApplication::applicationDirPath() + QStringLiteral("/plugins");
}

} // namespace hwsim::protocol
