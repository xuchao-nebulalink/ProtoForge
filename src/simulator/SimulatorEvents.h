#pragma once

#include "Parameter.h"

#include <core/Clock.h>

namespace hwsim::simulator {

/// Value-typed notifications published on the core EventBus, mirroring the
/// transport and protocol event families. The UI observes these; it never holds
/// a pointer into a device that lives on another thread.

struct ParameterChangedEvent {
    QString deviceName;
    QString key;
    quint32 address{0};
    bool hasAddress{false};
    QVariant previousValue;
    QVariant newValue;
    QString origin;
    qint64 timestampMs{core::wallClockMs()};
};

struct DeviceStateChangedEvent {
    QString deviceName;
    QString fromState;
    QString toState;
    QString reason;
    bool responsive{true};
    qint64 timestampMs{core::wallClockMs()};
};

struct FaultActivatedEvent {
    QString deviceName;
    QString ruleId;
    QString note;
    qint64 timestampMs{core::wallClockMs()};
};

struct DeviceLifecycleEvent {
    QString deviceName;
    bool running{false};
    bool online{true};
    qint64 timestampMs{core::wallClockMs()};
};

} // namespace hwsim::simulator
