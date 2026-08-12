#include "DeviceModel.h"

#include "SimulatorEvents.h"

#include <core/Logger.h>

namespace {
constexpr auto kLogCategory = "simulator.device";
}

using hwsim::core::Result;

namespace hwsim::simulator {

DeviceModel::DeviceModel(QString name, QObject* parent)
    : QObject(parent), name_(std::move(name)), parameters_(name_),
      stateMachine_(&parameters_, this), signalEngine_(&parameters_, this), faults_(this)
{
    // Parenting the subsystems to the device is load-bearing, not cosmetic:
    // moveToThread() only carries an object and its children, and the whole
    // device is moved onto its own I/O thread. Without a parent these would
    // stay behind on the creating thread and their QTimers would refuse to
    // start ("Timers cannot be started from another thread"), so live data
    // generation and state evaluation would silently never run.
    //
    // Value members parented to their own owner are safe here: they are
    // destroyed before ~QObject runs, and each removes itself from the child
    // list on the way out, so the base destructor never sees them.
    wireInternalSignals();
}

DeviceModel::~DeviceModel()
{
    stop();
}

void DeviceModel::setName(QString name)
{
    name_ = std::move(name);
    parameters_.setDeviceName(name_);
}

void DeviceModel::wireInternalSignals()
{
    parameters_.onChanged([this](const ParameterChange& change) {
        emit parameterChanged(change.key, change.newValue);

        if (eventBus_ != nullptr) {
            ParameterChangedEvent event;
            event.deviceName = change.deviceName;
            event.key = change.key;
            event.address = change.address;
            event.hasAddress = change.hasAddress;
            event.previousValue = change.previousValue;
            event.newValue = change.newValue;
            event.origin = writeOriginName(change.origin);
            event.timestampMs = change.timestampMs;
            eventBus_->publish(event);
        }
    });

    connect(&stateMachine_, &DeviceStateMachine::stateChanged, this,
            [this](const QString& from, const QString& to, const QString& reason) {
                emit stateChanged(from, to);

                if (eventBus_ != nullptr) {
                    DeviceStateChangedEvent event;
                    event.deviceName = name_;
                    event.fromState = from;
                    event.toState = to;
                    event.reason = reason;
                    event.responsive = stateMachine_.isResponsive();
                    eventBus_->publish(event);
                }
            });

    connect(&faults_, &FaultInjector::faultActivated, this,
            [this](const QString& ruleId, const QString& note) {
                emit faultActivated(ruleId, note);

                if (eventBus_ != nullptr) {
                    FaultActivatedEvent event;
                    event.deviceName = name_;
                    event.ruleId = ruleId;
                    event.note = note;
                    eventBus_->publish(event);
                }
            });
}

void DeviceModel::setEventBus(core::EventBus* bus)
{
    eventBus_ = bus;
}

void DeviceModel::start()
{
    if (running_) {
        return;
    }
    running_ = true;

    // A device without a declared state model is perfectly valid; it is simply
    // always responsive.
    if (!stateMachine_.states().isEmpty()) {
        if (const auto started = stateMachine_.start(); started.hasError()) {
            HWSIM_LOG_WARNING(kLogCategory) << name_ << ": " << started.error().toString();
        }
    }

    signalEngine_.start();

    HWSIM_LOG_INFO(kLogCategory) << "device '" << name_ << "' started";

    if (eventBus_ != nullptr) {
        DeviceLifecycleEvent event;
        event.deviceName = name_;
        event.running = true;
        event.online = online_;
        eventBus_->publish(event);
    }
}

void DeviceModel::stop()
{
    if (!running_) {
        return;
    }
    running_ = false;

    signalEngine_.stop();
    stateMachine_.stop();

    HWSIM_LOG_INFO(kLogCategory) << "device '" << name_ << "' stopped";

    if (eventBus_ != nullptr) {
        DeviceLifecycleEvent event;
        event.deviceName = name_;
        event.running = false;
        event.online = online_;
        eventBus_->publish(event);
    }
}

void DeviceModel::setOnline(bool online)
{
    if (online_ == online) {
        return;
    }
    online_ = online;

    HWSIM_LOG_INFO(kLogCategory) << "device '" << name_ << "' is now "
                                 << (online ? "online" : "offline");
    emit onlineChanged(online_);

    if (eventBus_ != nullptr) {
        DeviceLifecycleEvent event;
        event.deviceName = name_;
        event.running = running_;
        event.online = online_;
        eventBus_->publish(event);
    }
}

void DeviceModel::reset()
{
    parameters_.resetToDefaults();
    signalEngine_.resetTimeBase();
    faults_.resetStatistics();

    if (!stateMachine_.states().isEmpty() && stateMachine_.isRunning()) {
        stateMachine_.stop();
        if (const auto restarted = stateMachine_.start(); restarted.hasError()) {
            HWSIM_LOG_WARNING(kLogCategory) << name_ << ": " << restarted.error().toString();
        }
    }
}

// --- IDeviceAccess ---------------------------------------------------------

bool DeviceModel::isResponsive() const
{
    return online_ && stateMachine_.isResponsive();
}

QString DeviceModel::currentState() const
{
    const QString state = stateMachine_.currentState();
    if (!online_) {
        return QStringLiteral("Offline");
    }
    return state.isEmpty() ? QStringLiteral("Running") : state;
}

Result<QVariant> DeviceModel::readParameter(const QString& key)
{
    return parameters_.read(key);
}

Result<void> DeviceModel::writeParameter(const QString& key, const QVariant& value)
{
    return parameters_.write(key, value, WriteOrigin::Protocol);
}

Result<QVariant> DeviceModel::readAddress(quint32 address)
{
    return parameters_.readAddress(address);
}

Result<void> DeviceModel::writeAddress(quint32 address, const QVariant& value)
{
    return parameters_.writeAddress(address, value, WriteOrigin::Protocol);
}

Result<QVector<QVariant>> DeviceModel::readAddressRange(quint32 startAddress, quint32 count)
{
    return parameters_.readAddressRange(startAddress, count);
}

Result<void> DeviceModel::writeAddressRange(quint32 startAddress, const QVector<QVariant>& values)
{
    return parameters_.writeAddressRange(startAddress, values, WriteOrigin::Protocol);
}

Result<void> DeviceModel::postEvent(const QString& eventName)
{
    return stateMachine_.postEvent(eventName);
}

} // namespace hwsim::simulator
