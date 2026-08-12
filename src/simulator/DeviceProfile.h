#pragma once

#include "DeviceModel.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVariantMap>

namespace hwsim::simulator {

/// A complete device definition, serialisable to JSON.
///
/// This is the unit of reuse and the unit of regression testing: a profile
/// captures the register map, the state model, the live-data generators, the
/// fault rules and the transport binding, so a scenario can be committed to the
/// repository, diffed in review and replayed identically in CI.
struct HWSIM_SIMULATOR_API DeviceProfile {
    /// Bumped when the on-disk shape changes, so old files can be migrated
    /// rather than silently misread.
    static constexpr int kFormatVersion = 1;

    QString name;
    QString description;

    /// Plugin id, e.g. "modbus", plus its instance settings.
    QString protocolId;
    QVariantMap protocolConfig;

    /// transport::TransportConfig serialised form.
    QJsonObject transportConfig;

    /// "responder" for a simulated device, "initiator" for a test master.
    QString role{QStringLiteral("responder")};

    QJsonArray parameters;
    QJsonObject stateMachine;
    QJsonArray signalBindings;
    QJsonArray faultRules;

    /// Values applied after the parameter definitions are loaded, so a profile
    /// can capture a specific device state rather than just its shape.
    QVariantMap initialValues;

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static core::Result<DeviceProfile> fromJson(const QJsonObject& json);

    [[nodiscard]] static core::Result<DeviceProfile> load(const QString& path);
    [[nodiscard]] core::Result<void> save(const QString& path) const;

    /// Builds the device: definitions, state model, signal bindings, faults and
    /// initial values, in that order.
    [[nodiscard]] core::Result<void> applyTo(DeviceModel& device) const;

    /// Snapshots a live device, including its current parameter values.
    ///
    /// A DeviceModel knows nothing about which protocol or transport it is
    /// bound to, so those fields are carried over from `base`. Without it the
    /// snapshot would round-trip into a profile with no protocol id and no
    /// transport, which validate() still accepts because role has a default,
    /// so the loss would surface only as a device that cannot be started.
    [[nodiscard]] static DeviceProfile captureFrom(DeviceModel& device,
                                                   const DeviceProfile& base = {});

    [[nodiscard]] core::Result<void> validate() const;
};

} // namespace hwsim::simulator
