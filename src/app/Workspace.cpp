#include "Workspace.h"

#include <core/Logger.h>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {
constexpr auto kLogCategory = "app.workspace";
constexpr int kWorkspaceFormatVersion = 1;
} // namespace

using hwsim::core::ErrorCode;
using hwsim::core::makeError;
using hwsim::core::Result;

namespace hwsim::app {

Workspace::Workspace(protocol::PluginManager& plugins, core::EventBus& bus, QObject* parent)
    : QObject(parent), plugins_(&plugins), bus_(&bus)
{
}

Workspace::~Workspace()
{
    clear();
}

Result<QString> Workspace::addDevice(DeviceRuntime::Config config)
{
    if (config.id.isEmpty()) {
        config.id = config.name;
    }
    if (config.id.isEmpty()) {
        return makeError(ErrorCode::InvalidArgument, QStringLiteral("device needs a name"));
    }
    if (devices_.contains(config.id)) {
        return makeError(ErrorCode::AlreadyExists,
                         QStringLiteral("a device named '%1' already exists").arg(config.id));
    }

    const QString id = config.id;
    auto created = DeviceRuntime::create(std::move(config), *plugins_, *bus_);
    if (created.hasError()) {
        return created.error();
    }

    auto runtime = std::move(created).value();
    connect(runtime.get(), &DeviceRuntime::linkCountChanged, this,
            [this, id] { emit deviceUpdated(id); });

    devices_.emplace(id, std::move(runtime));
    emit devicesChanged();
    return id;
}

bool Workspace::removeDevice(const QString& deviceId)
{
    const auto it = devices_.find(deviceId);
    if (it == devices_.end()) {
        return false;
    }

    it->second->stop();
    devices_.erase(it);
    emit devicesChanged();
    return true;
}

void Workspace::clear()
{
    for (auto& [id, runtime] : devices_) {
        runtime->stop();
    }
    devices_.clear();
    emit devicesChanged();
}

Result<void> Workspace::startDevice(const QString& deviceId)
{
    DeviceRuntime* runtime = device(deviceId);
    if (runtime == nullptr) {
        return makeError(ErrorCode::NotFound, QStringLiteral("no device '%1'").arg(deviceId));
    }

    const auto started = runtime->start();
    emit deviceUpdated(deviceId);
    return started;
}

void Workspace::stopDevice(const QString& deviceId)
{
    if (DeviceRuntime* runtime = device(deviceId); runtime != nullptr) {
        runtime->stop();
        emit deviceUpdated(deviceId);
    }
}

Result<void> Workspace::startAll()
{
    // Report the first failure but keep going, so one misconfigured endpoint
    // does not leave the rest of the workspace down.
    Result<void> firstFailure = core::success();

    for (auto& [id, runtime] : devices_) {
        if (const auto started = runtime->start(); started.hasError()) {
            HWSIM_LOG_WARNING(kLogCategory) << id << ": " << started.error().toString();
            if (firstFailure.hasValue()) {
                firstFailure = started;
            }
        }
    }

    emit devicesChanged();
    return firstFailure;
}

void Workspace::stopAll()
{
    for (auto& [id, runtime] : devices_) {
        runtime->stop();
    }
    emit devicesChanged();
}

DeviceRuntime* Workspace::device(const QString& deviceId) const
{
    const auto it = devices_.find(deviceId);
    return it == devices_.end() ? nullptr : it->second.get();
}

QStringList Workspace::deviceIds() const
{
    QStringList ids;
    ids.reserve(static_cast<qsizetype>(devices_.size()));
    for (const auto& [id, runtime] : devices_) {
        ids.append(id);
    }
    return ids;
}

ui::DeviceNodeInfo Workspace::deviceNode(const QString& deviceId) const
{
    ui::DeviceNodeInfo node;

    DeviceRuntime* runtime = device(deviceId);
    if (runtime == nullptr) {
        return node;
    }

    node.deviceId = deviceId;
    node.name = runtime->name();
    node.protocolDisplayName = runtime->config().protocolId;
    node.transportDescription = runtime->config().transport.describe();
    node.running = runtime->isRunning();
    node.linkDescriptions = runtime->linkDescriptions();
    node.linkCount = static_cast<int>(node.linkDescriptions.size());

    withDevice(deviceId, [&node](simulator::DeviceModel& model) {
        node.state = model.currentState();
        node.online = model.isOnline();
        node.parameterGroups = model.parameters().groups();

        for (const auto& binding : model.signalEngine().bindings()) {
            node.signalBindings.append(
                QStringLiteral("%1 <- %2").arg(binding.parameterKey, binding.sourceKind));
        }
        for (const auto& rule : model.faults().rules()) {
            node.faultRules.append(rule.enabled ? rule.id
                                                : QStringLiteral("%1 (停用)").arg(rule.id));
        }
    });

    return node;
}

QVector<ui::DeviceNodeInfo> Workspace::deviceNodes() const
{
    QVector<ui::DeviceNodeInfo> nodes;
    nodes.reserve(static_cast<qsizetype>(devices_.size()));

    for (const auto& [id, runtime] : devices_) {
        nodes.append(deviceNode(id));
    }
    return nodes;
}

bool Workspace::withDevice(const QString& deviceId,
                           const std::function<void(simulator::DeviceModel&)>& work) const
{
    DeviceRuntime* runtime = device(deviceId);
    if (runtime == nullptr || runtime->deviceUnsafe() == nullptr) {
        return false;
    }

    // The name says "unsafe" because the pointer belongs to another thread;
    // invoke() is what makes touching it safe.
    runtime->invoke([runtime, &work] { work(*runtime->deviceUnsafe()); });
    return true;
}

// --- Parameters ------------------------------------------------------------

QVector<simulator::ParameterDefinition> Workspace::parameterDefinitions(const QString& deviceId) const
{
    QVector<simulator::ParameterDefinition> definitions;
    withDevice(deviceId, [&definitions](simulator::DeviceModel& model) {
        definitions = model.parameters().definitions();
    });
    return definitions;
}

QVariantMap Workspace::parameterValues(const QString& deviceId) const
{
    QVariantMap values;
    withDevice(deviceId, [&values](simulator::DeviceModel& model) {
        values = model.parameters().snapshot();
    });
    return values;
}

Result<void> Workspace::setParameter(const QString& deviceId, const QString& key,
                                     const QVariant& value)
{
    Result<void> result = makeError(ErrorCode::NotFound,
                                    QStringLiteral("no device '%1'").arg(deviceId));
    withDevice(deviceId, [&](simulator::DeviceModel& model) {
        // Origin Ui, not Protocol: the operator may drive read-only registers.
        result = model.parameters().write(key, value, simulator::WriteOrigin::Ui);
    });
    return result;
}

void Workspace::resetParameters(const QString& deviceId)
{
    withDevice(deviceId, [](simulator::DeviceModel& model) { model.parameters().resetToDefaults(); });
}

// --- Signals ---------------------------------------------------------------

QVector<simulator::SignalBindingInfo> Workspace::signalBindings(const QString& deviceId) const
{
    QVector<simulator::SignalBindingInfo> bindings;
    withDevice(deviceId, [&bindings](simulator::DeviceModel& model) {
        bindings = model.signalEngine().bindings();
    });
    return bindings;
}

Result<QString> Workspace::addSignalBinding(const QString& deviceId, const QString& parameterKey,
                                            const QString& sourceKind,
                                            const QVariantMap& configuration, int intervalMs,
                                            simulator::CombineMode combine)
{
    Result<QString> result = makeError(ErrorCode::NotFound,
                                       QStringLiteral("no device '%1'").arg(deviceId));
    withDevice(deviceId, [&](simulator::DeviceModel& model) {
        result = model.signalEngine().addBinding(parameterKey, sourceKind, configuration,
                                                 intervalMs, combine);
    });
    if (result.hasValue()) {
        emit deviceUpdated(deviceId);
    }
    return result;
}

bool Workspace::removeSignalBinding(const QString& deviceId, const QString& bindingId)
{
    bool removed = false;
    withDevice(deviceId, [&](simulator::DeviceModel& model) {
        removed = model.signalEngine().removeBinding(bindingId);
    });
    if (removed) {
        emit deviceUpdated(deviceId);
    }
    return removed;
}

bool Workspace::setSignalBindingEnabled(const QString& deviceId, const QString& bindingId,
                                        bool enabled)
{
    bool changed = false;
    withDevice(deviceId, [&](simulator::DeviceModel& model) {
        changed = model.signalEngine().setBindingEnabled(bindingId, enabled);
    });
    return changed;
}

// --- Faults ----------------------------------------------------------------

QVector<simulator::FaultRuleInfo> Workspace::faultRules(const QString& deviceId) const
{
    QVector<simulator::FaultRuleInfo> rules;
    withDevice(deviceId, [&rules](simulator::DeviceModel& model) {
        rules = model.faults().rules();
    });
    return rules;
}

Result<QString> Workspace::addFaultRule(const QString& deviceId, const QString& kind,
                                        const QVariantMap& configuration)
{
    Result<QString> result = makeError(ErrorCode::NotFound,
                                       QStringLiteral("no device '%1'").arg(deviceId));
    withDevice(deviceId, [&](simulator::DeviceModel& model) {
        result = model.faults().addRule(kind, configuration);
    });
    if (result.hasValue()) {
        emit deviceUpdated(deviceId);
    }
    return result;
}

bool Workspace::removeFaultRule(const QString& deviceId, const QString& ruleId)
{
    bool removed = false;
    withDevice(deviceId, [&](simulator::DeviceModel& model) {
        removed = model.faults().removeRule(ruleId);
    });
    if (removed) {
        emit deviceUpdated(deviceId);
    }
    return removed;
}

bool Workspace::setFaultRuleEnabled(const QString& deviceId, const QString& ruleId, bool enabled)
{
    bool changed = false;
    withDevice(deviceId, [&](simulator::DeviceModel& model) {
        changed = model.faults().setRuleEnabled(ruleId, enabled);
    });
    return changed;
}

bool Workspace::armFaultRule(const QString& deviceId, const QString& ruleId)
{
    bool armed = false;
    withDevice(deviceId, [&](simulator::DeviceModel& model) {
        armed = model.faults().armRule(ruleId);
    });
    return armed;
}

void Workspace::setFaultInjectionEnabled(const QString& deviceId, bool enabled)
{
    withDevice(deviceId,
               [enabled](simulator::DeviceModel& model) { model.faults().setGloballyEnabled(enabled); });
}

bool Workspace::isFaultInjectionEnabled(const QString& deviceId) const
{
    bool enabled = false;
    withDevice(deviceId, [&enabled](simulator::DeviceModel& model) {
        enabled = model.faults().isGloballyEnabled();
    });
    return enabled;
}

// --- Lifecycle -------------------------------------------------------------

QStringList Workspace::stateNames(const QString& deviceId) const
{
    QStringList names;
    withDevice(deviceId, [&names](simulator::DeviceModel& model) {
        names = model.stateMachine().stateNames();
    });
    return names;
}

QStringList Workspace::eventNames(const QString& deviceId) const
{
    QStringList names;
    withDevice(deviceId, [&names](simulator::DeviceModel& model) {
        names = model.stateMachine().eventNames();
    });
    return names;
}

QString Workspace::currentState(const QString& deviceId) const
{
    QString state;
    withDevice(deviceId, [&state](simulator::DeviceModel& model) { state = model.currentState(); });
    return state;
}

Result<void> Workspace::forceState(const QString& deviceId, const QString& stateName)
{
    Result<void> result = makeError(ErrorCode::NotFound,
                                    QStringLiteral("no device '%1'").arg(deviceId));
    withDevice(deviceId, [&](simulator::DeviceModel& model) {
        result = model.stateMachine().forceState(stateName);
    });
    if (result.hasValue()) {
        emit deviceUpdated(deviceId);
    }
    return result;
}

Result<void> Workspace::postEvent(const QString& deviceId, const QString& eventName)
{
    Result<void> result = makeError(ErrorCode::NotFound,
                                    QStringLiteral("no device '%1'").arg(deviceId));
    withDevice(deviceId, [&](simulator::DeviceModel& model) {
        result = model.postEvent(eventName);
    });
    if (result.hasValue()) {
        emit deviceUpdated(deviceId);
    }
    return result;
}

void Workspace::setDeviceOnline(const QString& deviceId, bool online)
{
    withDevice(deviceId, [online](simulator::DeviceModel& model) { model.setOnline(online); });
    emit deviceUpdated(deviceId);
}

bool Workspace::isDeviceOnline(const QString& deviceId) const
{
    bool online = false;
    withDevice(deviceId, [&online](simulator::DeviceModel& model) { online = model.isOnline(); });
    return online;
}

Result<void> Workspace::sendRaw(const QString& deviceId, const QByteArray& bytes)
{
    DeviceRuntime* runtime = device(deviceId);
    if (runtime == nullptr) {
        return makeError(ErrorCode::NotFound, QStringLiteral("no device '%1'").arg(deviceId));
    }
    return runtime->sendRaw(bytes);
}

// --- Persistence -----------------------------------------------------------

Result<void> Workspace::save(const QString& path) const
{
    QJsonArray deviceArray;
    for (const auto& [id, runtime] : devices_) {
        const DeviceRuntime::Config& config = runtime->config();

        // Capture the live device so the saved workspace reflects the current
        // register values, signal bindings and fault rules, not just the shape
        // it was created with.
        simulator::DeviceProfile profile = config.profile;
        if (runtime->deviceUnsafe() != nullptr) {
            const simulator::DeviceProfile base = profile;
            runtime->invoke([&profile, &base, &runtime] {
                profile = simulator::DeviceProfile::captureFrom(*runtime->deviceUnsafe(), base);
            });
        }
        profile.protocolId = config.protocolId;
        profile.protocolConfig = config.protocolConfig;
        profile.transportConfig = config.transport.toJson();
        profile.role = config.role == transport::TransportRole::Initiator
                           ? QStringLiteral("initiator")
                           : QStringLiteral("responder");

        QJsonObject entry = profile.toJson();
        entry.insert(QStringLiteral("id"), id);
        deviceArray.append(entry);
    }

    QJsonObject root;
    root.insert(QStringLiteral("formatVersion"), kWorkspaceFormatVersion);
    root.insert(QStringLiteral("devices"), deviceArray);

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return makeError(ErrorCode::IoError, file.errorString(), path);
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));

    HWSIM_LOG_INFO(kLogCategory) << "saved " << devices_.size() << " device(s) to " << path;
    return core::success();
}

Result<void> Workspace::load(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return makeError(ErrorCode::IoError, file.errorString(), path);
    }

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        return makeError(ErrorCode::ConfigInvalid, parseError.errorString(), path);
    }

    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("formatVersion")).toInt(kWorkspaceFormatVersion)
        > kWorkspaceFormatVersion) {
        return makeError(ErrorCode::ConfigInvalid,
                         QStringLiteral("workspace format is newer than this build supports"), path);
    }

    // Checked before clear(): handing this a file that is not a workspace --
    // a bare device profile is the easy mistake, since the two look alike --
    // used to empty the workspace and report success, leaving no clue that
    // nothing had been read.
    const QJsonValue devicesValue = root.value(QStringLiteral("devices"));
    if (!devicesValue.isArray()) {
        return makeError(ErrorCode::ConfigInvalid,
                         QStringLiteral("not a workspace file: no 'devices' array. A single "
                                        "device profile has to be wrapped as "
                                        "{\"devices\": [ ...profile..., \"id\": \"...\" ]}"),
                         path);
    }

    clear();

    for (const QJsonValue& value : devicesValue.toArray()) {
        const QJsonObject entry = value.toObject();

        const auto profile = simulator::DeviceProfile::fromJson(entry);
        if (profile.hasError()) {
            return profile.error();
        }

        const auto transportConfig =
            transport::TransportConfig::fromJson(profile.value().transportConfig);
        if (transportConfig.hasError()) {
            return transportConfig.error();
        }

        DeviceRuntime::Config config;
        config.id = entry.value(QStringLiteral("id")).toString(profile.value().name);
        config.name = profile.value().name;
        config.protocolId = profile.value().protocolId;
        config.protocolConfig = profile.value().protocolConfig;
        config.transport = transportConfig.value();
        config.role = profile.value().role == QStringLiteral("initiator")
                          ? transport::TransportRole::Initiator
                          : transport::TransportRole::Responder;
        config.profile = profile.value();

        if (const auto added = addDevice(std::move(config)); added.hasError()) {
            return added.error();
        }
    }

    HWSIM_LOG_INFO(kLogCategory) << "loaded " << devices_.size() << " device(s) from " << path;
    emit devicesChanged();
    return core::success();
}

} // namespace hwsim::app
