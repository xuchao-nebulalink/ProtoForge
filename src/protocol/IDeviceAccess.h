#pragma once

#include "ProtocolGlobal.h"

#include <core/Result.h>

#include <QString>
#include <QVariant>
#include <QVector>

namespace hwsim::protocol {

/// The protocol layer's view of a simulated device.
///
/// Handlers need to read and write device state, but the protocol layer must
/// not depend on the simulator: plugins are compiled against protocol alone,
/// and the unit tests substitute a trivial in-memory implementation. So the
/// dependency is inverted here and DeviceModel implements this interface.
class HWSIM_PROTOCOL_API IDeviceAccess {
public:
    virtual ~IDeviceAccess() = default;

    [[nodiscard]] virtual QString deviceName() const = 0;

    /// True when the device is in a state that should answer requests. A
    /// device driven into a fault state by its state machine returns false and
    /// the session stops replying, which is how "device went dark" is simulated.
    [[nodiscard]] virtual bool isResponsive() const = 0;

    [[nodiscard]] virtual QString currentState() const = 0;

    /// Named access, for protocols that address by tag.
    [[nodiscard]] virtual core::Result<QVariant> readParameter(const QString& key) = 0;
    [[nodiscard]] virtual core::Result<void> writeParameter(const QString& key, const QVariant& value) = 0;

    /// Numeric access, for register-oriented protocols such as Modbus.
    [[nodiscard]] virtual core::Result<QVariant> readAddress(quint32 address) = 0;
    [[nodiscard]] virtual core::Result<void> writeAddress(quint32 address, const QVariant& value) = 0;

    /// Bulk variants. The default implementations loop over the single-value
    /// calls; a device model with a contiguous backing store can do better.
    [[nodiscard]] virtual core::Result<QVector<QVariant>> readAddressRange(quint32 startAddress,
                                                                          quint32 count);
    [[nodiscard]] virtual core::Result<void> writeAddressRange(quint32 startAddress,
                                                               const QVector<QVariant>& values);

    /// Feeds a named trigger into the device state machine.
    [[nodiscard]] virtual core::Result<void> postEvent(const QString& eventName) = 0;
};

} // namespace hwsim::protocol
