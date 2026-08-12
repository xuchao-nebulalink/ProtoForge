#pragma once

#include "DeviceStateMachine.h"
#include "FaultInjector.h"
#include "ParameterStore.h"
#include "SignalEngine.h"

#include <core/EventBus.h>
#include <protocol/IDeviceAccess.h>

#include <QObject>

namespace hwsim::simulator {

/// A simulated device: parameters, operating states, live data generation and
/// fault injection, presented to the protocol layer through IDeviceAccess.
///
/// Implementing the protocol layer's interface here rather than exposing this
/// class to plugins is what keeps the dependency pointing one way. A protocol
/// plugin compiles against IDeviceAccess alone and can be unit tested against a
/// twenty-line stub.
class HWSIM_SIMULATOR_API DeviceModel : public QObject, public protocol::IDeviceAccess {
    Q_OBJECT

public:
    explicit DeviceModel(QString name, QObject* parent = nullptr);
    ~DeviceModel() override;

    void setName(QString name);

    [[nodiscard]] ParameterStore& parameters() noexcept { return parameters_; }
    [[nodiscard]] const ParameterStore& parameters() const noexcept { return parameters_; }

    [[nodiscard]] DeviceStateMachine& stateMachine() noexcept { return stateMachine_; }
    [[nodiscard]] SignalEngine& signalEngine() noexcept { return signalEngine_; }
    [[nodiscard]] FaultInjector& faults() noexcept { return faults_; }

    void start();
    void stop();
    [[nodiscard]] bool isRunning() const noexcept { return running_; }

    /// Master switch that is independent of the state machine, for the "power
    /// off the device" button. An offline device stops answering entirely.
    void setOnline(bool online);
    [[nodiscard]] bool isOnline() const noexcept { return online_; }

    void setEventBus(core::EventBus* bus);
    [[nodiscard]] core::EventBus* eventBus() const noexcept { return eventBus_; }

    /// Restores every parameter to its declared default and re-enters the
    /// initial state. Used between test cases.
    void reset();

    // --- IDeviceAccess ---

    [[nodiscard]] QString deviceName() const override { return name_; }
    [[nodiscard]] bool isResponsive() const override;
    [[nodiscard]] QString currentState() const override;

    [[nodiscard]] core::Result<QVariant> readParameter(const QString& key) override;
    [[nodiscard]] core::Result<void> writeParameter(const QString& key, const QVariant& value) override;
    [[nodiscard]] core::Result<QVariant> readAddress(quint32 address) override;
    [[nodiscard]] core::Result<void> writeAddress(quint32 address, const QVariant& value) override;
    [[nodiscard]] core::Result<QVector<QVariant>> readAddressRange(quint32 startAddress,
                                                                   quint32 count) override;
    [[nodiscard]] core::Result<void> writeAddressRange(quint32 startAddress,
                                                       const QVector<QVariant>& values) override;
    [[nodiscard]] core::Result<void> postEvent(const QString& eventName) override;

signals:
    void parameterChanged(const QString& key, const QVariant& value);
    void stateChanged(const QString& fromState, const QString& toState);
    void onlineChanged(bool online);
    void faultActivated(const QString& ruleId, const QString& note);

private:
    void wireInternalSignals();

    QString name_;
    ParameterStore parameters_;
    DeviceStateMachine stateMachine_;
    SignalEngine signalEngine_;
    FaultInjector faults_;

    core::EventBus* eventBus_{nullptr};
    bool running_{false};
    bool online_{true};
};

} // namespace hwsim::simulator
