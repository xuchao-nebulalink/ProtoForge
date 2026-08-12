#pragma once

#include "DeviceRuntime.h"

#include <ui/DeviceTreeWidget.h>
#include <ui/IDeviceController.h>

#include <QObject>

#include <map>
#include <memory>

namespace hwsim::app {

/// The set of configured devices, and the bridge between them and the UI.
///
/// Implements ui::IDeviceController by marshalling every call onto the target
/// device's own thread through DeviceRuntime::invoke. That is the single place
/// where the UI thread meets the device threads, so it is the only place that
/// has to be careful about it.
class Workspace : public QObject, public ui::IDeviceController {
    Q_OBJECT

public:
    Workspace(protocol::PluginManager& plugins, core::EventBus& bus, QObject* parent = nullptr);
    ~Workspace() override;

    [[nodiscard]] core::Result<QString> addDevice(DeviceRuntime::Config config);
    bool removeDevice(const QString& deviceId);
    void clear();

    [[nodiscard]] core::Result<void> startDevice(const QString& deviceId);
    void stopDevice(const QString& deviceId);
    [[nodiscard]] core::Result<void> startAll();
    void stopAll();

    [[nodiscard]] DeviceRuntime* device(const QString& deviceId) const;
    [[nodiscard]] QVector<ui::DeviceNodeInfo> deviceNodes() const;

    /// Snapshot of one device. Building the whole tree costs several blocking
    /// round trips per device, so the UI refreshes just the device that changed.
    [[nodiscard]] ui::DeviceNodeInfo deviceNode(const QString& deviceId) const;
    [[nodiscard]] int deviceCount() const { return static_cast<int>(devices_.size()); }

    [[nodiscard]] core::Result<void> load(const QString& path);
    [[nodiscard]] core::Result<void> save(const QString& path) const;

    [[nodiscard]] core::Result<void> sendRaw(const QString& deviceId, const QByteArray& bytes);

    // --- ui::IDeviceController ---

    [[nodiscard]] QStringList deviceIds() const override;

    [[nodiscard]] QVector<simulator::ParameterDefinition> parameterDefinitions(
        const QString& deviceId) const override;
    [[nodiscard]] QVariantMap parameterValues(const QString& deviceId) const override;
    [[nodiscard]] core::Result<void> setParameter(const QString& deviceId, const QString& key,
                                                  const QVariant& value) override;
    void resetParameters(const QString& deviceId) override;

    [[nodiscard]] QVector<simulator::SignalBindingInfo> signalBindings(
        const QString& deviceId) const override;
    [[nodiscard]] core::Result<QString> addSignalBinding(
        const QString& deviceId, const QString& parameterKey, const QString& sourceKind,
        const QVariantMap& configuration, int intervalMs,
        simulator::CombineMode combine) override;
    bool removeSignalBinding(const QString& deviceId, const QString& bindingId) override;
    bool setSignalBindingEnabled(const QString& deviceId, const QString& bindingId,
                                 bool enabled) override;

    [[nodiscard]] QVector<simulator::FaultRuleInfo> faultRules(
        const QString& deviceId) const override;
    [[nodiscard]] core::Result<QString> addFaultRule(const QString& deviceId, const QString& kind,
                                                     const QVariantMap& configuration) override;
    bool removeFaultRule(const QString& deviceId, const QString& ruleId) override;
    bool setFaultRuleEnabled(const QString& deviceId, const QString& ruleId, bool enabled) override;
    bool armFaultRule(const QString& deviceId, const QString& ruleId) override;
    void setFaultInjectionEnabled(const QString& deviceId, bool enabled) override;
    [[nodiscard]] bool isFaultInjectionEnabled(const QString& deviceId) const override;

    [[nodiscard]] QStringList stateNames(const QString& deviceId) const override;
    [[nodiscard]] QStringList eventNames(const QString& deviceId) const override;
    [[nodiscard]] QString currentState(const QString& deviceId) const override;
    [[nodiscard]] core::Result<void> forceState(const QString& deviceId,
                                                const QString& stateName) override;
    [[nodiscard]] core::Result<void> postEvent(const QString& deviceId,
                                               const QString& eventName) override;
    void setDeviceOnline(const QString& deviceId, bool online) override;
    [[nodiscard]] bool isDeviceOnline(const QString& deviceId) const override;

signals:
    void devicesChanged();
    void deviceUpdated(const QString& deviceId);

private:
    /// Runs `work` with the device model, on the device's thread. Returns false
    /// when the device does not exist.
    bool withDevice(const QString& deviceId,
                    const std::function<void(simulator::DeviceModel&)>& work) const;

    protocol::PluginManager* plugins_{nullptr};
    core::EventBus* bus_{nullptr};

    /// Ordered so the device tree and saved workspaces keep a stable order.
    std::map<QString, std::unique_ptr<DeviceRuntime>> devices_;
};

} // namespace hwsim::app
