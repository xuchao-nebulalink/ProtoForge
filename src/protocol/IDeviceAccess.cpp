#include "IDeviceAccess.h"

namespace hwsim::protocol {

core::Result<QVector<QVariant>> IDeviceAccess::readAddressRange(quint32 startAddress, quint32 count)
{
    QVector<QVariant> values;
    values.reserve(static_cast<qsizetype>(count));

    for (quint32 offset = 0; offset < count; ++offset) {
        auto value = readAddress(startAddress + offset);
        if (value.hasError()) {
            return value.error();
        }
        values.append(std::move(value).value());
    }
    return values;
}

core::Result<void> IDeviceAccess::writeAddressRange(quint32 startAddress,
                                                    const QVector<QVariant>& values)
{
    for (qsizetype index = 0; index < values.size(); ++index) {
        const auto written = writeAddress(startAddress + static_cast<quint32>(index), values.at(index));
        if (written.hasError()) {
            return written.error();
        }
    }
    return core::success();
}

} // namespace hwsim::protocol
