#include "ExecutionContext.h"

namespace hwsim::protocol {

ExecutionContext::ExecutionContext(IDeviceAccess* device, const Frame& frame)
    : device_(device), frame_(&frame)
{
}

core::Result<IDeviceAccess*> ExecutionContext::requireDevice() const
{
    if (device_ == nullptr) {
        return core::makeError(core::ErrorCode::NotReady,
                               QStringLiteral("no device model is attached to this session"),
                               sessionName_);
    }
    return device_;
}

core::Result<QVariant> ExecutionContext::read(quint32 address) const
{
    const auto device = requireDevice();
    if (device.hasError()) {
        return device.error();
    }
    return device.value()->readAddress(address);
}

core::Result<void> ExecutionContext::write(quint32 address, const QVariant& value) const
{
    const auto device = requireDevice();
    if (device.hasError()) {
        return device.error();
    }
    return device.value()->writeAddress(address, value);
}

core::Result<QVector<QVariant>> ExecutionContext::readRange(quint32 startAddress, quint32 count) const
{
    const auto device = requireDevice();
    if (device.hasError()) {
        return device.error();
    }
    return device.value()->readAddressRange(startAddress, count);
}

core::Result<void> ExecutionContext::writeRange(quint32 startAddress,
                                                const QVector<QVariant>& values) const
{
    const auto device = requireDevice();
    if (device.hasError()) {
        return device.error();
    }
    return device.value()->writeAddressRange(startAddress, values);
}

} // namespace hwsim::protocol
