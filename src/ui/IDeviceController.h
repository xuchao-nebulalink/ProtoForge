#pragma once

#include "UiGlobal.h"

#include <core/Result.h>
#include <simulator/FaultInjector.h>
#include <simulator/Parameter.h>
#include <simulator/SignalEngine.h>

#include <QStringList>
#include <QVector>

namespace hwsim::ui {

/// What the panels are allowed to ask of the running system.
///
/// The widgets never touch a DeviceModel directly: devices live on their own
/// I/O threads, and a panel reaching in would be a data race waiting to happen.
/// The application implements this interface and marshals each call onto the
/// right thread, which also makes every panel testable against a fake.
class HWSIM_UI_API IDeviceController {
public:
    virtual ~IDeviceController() = default;

    [[nodiscard]] virtual QStringList deviceIds() const = 0;

    // --- Parameters ---

    [[nodiscard]] virtual QVector<simulator::ParameterDefinition> parameterDefinitions(
        const QString& deviceId) const = 0;
    [[nodiscard]] virtual QVariantMap parameterValues(const QString& deviceId) const = 0;
    [[nodiscard]] virtual core::Result<void> setParameter(const QString& deviceId,
                                                          const QString& key,
                                                          const QVariant& value) = 0;
    virtual void resetParameters(const QString& deviceId) = 0;

    // --- Signals ---

    [[nodiscard]] virtual QVector<simulator::SignalBindingInfo> signalBindings(
        const QString& deviceId) const = 0;
    [[nodiscard]] virtual core::Result<QString> addSignalBinding(
        const QString& deviceId, const QString& parameterKey, const QString& sourceKind,
        const QVariantMap& configuration, int intervalMs, simulator::CombineMode combine) = 0;
    virtual bool removeSignalBinding(const QString& deviceId, const QString& bindingId) = 0;
    virtual bool setSignalBindingEnabled(const QString& deviceId, const QString& bindingId,
                                         bool enabled) = 0;

    // --- Faults ---

    [[nodiscard]] virtual QVector<simulator::FaultRuleInfo> faultRules(
        const QString& deviceId) const = 0;
    [[nodiscard]] virtual core::Result<QString> addFaultRule(const QString& deviceId,
                                                             const QString& kind,
                                                             const QVariantMap& configuration) = 0;
    virtual bool removeFaultRule(const QString& deviceId, const QString& ruleId) = 0;
    virtual bool setFaultRuleEnabled(const QString& deviceId, const QString& ruleId,
                                     bool enabled) = 0;
    virtual bool armFaultRule(const QString& deviceId, const QString& ruleId) = 0;
    virtual void setFaultInjectionEnabled(const QString& deviceId, bool enabled) = 0;
    [[nodiscard]] virtual bool isFaultInjectionEnabled(const QString& deviceId) const = 0;

    // --- Lifecycle ---

    [[nodiscard]] virtual QStringList stateNames(const QString& deviceId) const = 0;
    [[nodiscard]] virtual QStringList eventNames(const QString& deviceId) const = 0;
    [[nodiscard]] virtual QString currentState(const QString& deviceId) const = 0;
    [[nodiscard]] virtual core::Result<void> forceState(const QString& deviceId,
                                                        const QString& stateName) = 0;
    [[nodiscard]] virtual core::Result<void> postEvent(const QString& deviceId,
                                                       const QString& eventName) = 0;
    virtual void setDeviceOnline(const QString& deviceId, bool online) = 0;
    [[nodiscard]] virtual bool isDeviceOnline(const QString& deviceId) const = 0;
};

} // namespace hwsim::ui
