#pragma once

#include "SimulatorGlobal.h"

#include <core/Result.h>

#include <QJsonObject>
#include <QString>
#include <QVariant>

#include <optional>

namespace hwsim::simulator {

enum class ParameterType {
    Bool,
    Int,
    UInt,
    Float,
    Double,
    String,
    Bytes,
};

/// Access as seen from the protocol. The UI, scripts and signal generators are
/// deliberately not subject to this: making a sensor reading ReadOnly means the
/// master cannot write it, while the operator still must be able to set what
/// the "sensor" reports. That asymmetry is the whole point of a simulator, and
/// it is enforced in ParameterStore by WriteOrigin rather than here.
enum class AccessMode {
    ReadOnly,
    WriteOnly,
    ReadWrite,
};

/// Who is performing a write. Only Protocol writes are checked against AccessMode.
enum class WriteOrigin {
    Protocol,
    Ui,
    Script,
    SignalSource,
    StateMachine,
    Restore,
};

[[nodiscard]] HWSIM_SIMULATOR_API QString parameterTypeName(ParameterType type);
[[nodiscard]] HWSIM_SIMULATOR_API core::Result<ParameterType> parameterTypeFromName(const QString& name);
[[nodiscard]] HWSIM_SIMULATOR_API QString accessModeName(AccessMode mode);
[[nodiscard]] HWSIM_SIMULATOR_API core::Result<AccessMode> accessModeFromName(const QString& name);
[[nodiscard]] HWSIM_SIMULATOR_API QString writeOriginName(WriteOrigin origin);

/// Static description of one device parameter.
struct HWSIM_SIMULATOR_API ParameterDefinition {
    /// Unique name, used by scripts and by protocols that address by tag.
    QString key;
    QString displayName;
    QString description;
    QString group;
    QString unit;

    ParameterType type{ParameterType::Double};
    AccessMode access{AccessMode::ReadWrite};

    /// Numeric address for register-oriented protocols. Optional: a parameter
    /// can be reachable by name only.
    quint32 address{0};
    bool hasAddress{false};

    QVariant defaultValue;
    std::optional<double> minimum;
    std::optional<double> maximum;

    /// engineering value = raw * scale + offset. Lets a device expose 0.1 degree
    /// resolution over a 16-bit register without every handler knowing about it.
    double scale{1.0};
    double offset{0.0};

    /// Included in a saved profile snapshot.
    bool persistent{true};

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static core::Result<ParameterDefinition> fromJson(const QJsonObject& json);

    [[nodiscard]] static ParameterDefinition make(QString key, ParameterType type,
                                                  QVariant defaultValue);
};

/// A parameter's definition plus its current value.
class HWSIM_SIMULATOR_API Parameter {
public:
    explicit Parameter(ParameterDefinition definition);

    [[nodiscard]] const ParameterDefinition& definition() const noexcept { return definition_; }
    [[nodiscard]] const QString& key() const noexcept { return definition_.key; }

    [[nodiscard]] QVariant value() const { return value_; }

    /// Value with scale and offset applied, for display and for scripts.
    [[nodiscard]] QVariant engineeringValue() const;

    /// Coerces to the declared type and clamps into range. Rejects a Protocol
    /// write to a ReadOnly parameter; other origins are always allowed.
    [[nodiscard]] core::Result<void> setValue(const QVariant& value, WriteOrigin origin);

    void resetToDefault();

    [[nodiscard]] qint64 lastChangedMs() const noexcept { return lastChangedMs_; }
    [[nodiscard]] quint64 writeCount() const noexcept { return writeCount_; }

    /// Coercion and clamping without storing, used to validate before applying.
    [[nodiscard]] core::Result<QVariant> coerce(const QVariant& value) const;

private:
    ParameterDefinition definition_;
    QVariant value_;
    qint64 lastChangedMs_{0};
    quint64 writeCount_{0};
};

/// Emitted whenever a value actually changes.
struct HWSIM_SIMULATOR_API ParameterChange {
    QString deviceName;
    QString key;
    quint32 address{0};
    bool hasAddress{false};
    QVariant previousValue;
    QVariant newValue;
    WriteOrigin origin{WriteOrigin::Protocol};
    qint64 timestampMs{0};
};

} // namespace hwsim::simulator
