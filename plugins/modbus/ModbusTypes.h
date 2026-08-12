#pragma once

#include <core/ConfigSchema.h>
#include <core/Result.h>
#include <protocol/ProtocolTypes.h>

#include <QVariantMap>

namespace hwsim::plugins::modbus {

/// Function codes.
namespace fc {
inline constexpr quint8 kReadCoils = 0x01;
inline constexpr quint8 kReadDiscreteInputs = 0x02;
inline constexpr quint8 kReadHoldingRegisters = 0x03;
inline constexpr quint8 kReadInputRegisters = 0x04;
inline constexpr quint8 kWriteSingleCoil = 0x05;
inline constexpr quint8 kWriteSingleRegister = 0x06;
inline constexpr quint8 kWriteMultipleCoils = 0x0F;
inline constexpr quint8 kWriteMultipleRegisters = 0x10;

/// An exception response echoes the request's function code with bit 7 set.
inline constexpr quint8 kExceptionFlag = 0x80;

[[nodiscard]] constexpr bool isException(quint8 functionCode) noexcept
{
    return (functionCode & kExceptionFlag) != 0;
}
} // namespace fc

enum class ExceptionCode : quint8 {
    IllegalFunction = 0x01,
    IllegalDataAddress = 0x02,
    IllegalDataValue = 0x03,
    ServerDeviceFailure = 0x04,
    Acknowledge = 0x05,
    ServerDeviceBusy = 0x06,
    MemoryParityError = 0x08,
    GatewayPathUnavailable = 0x0A,
    GatewayTargetNoResponse = 0x0B,
};

[[nodiscard]] QString exceptionCodeName(ExceptionCode code);
[[nodiscard]] QString functionCodeName(quint8 functionCode);

/// Maps Modbus's four separate tables onto the device's single flat address
/// space.
///
/// Modbus addresses coils, discrete inputs, input registers and holding
/// registers independently, all starting at zero. The device model has one
/// address space, so each table gets a configurable base. The defaults follow
/// the conventional 0x/1x/3x/4x numbering that most register maps are written
/// against, which keeps profiles readable.
struct AddressMap {
    quint32 coilBase{0};
    quint32 discreteInputBase{10000};
    quint32 inputRegisterBase{30000};
    quint32 holdingRegisterBase{40000};

    /// Specification limits: 0x07D0 bits and 0x007D registers per read,
    /// 0x07B0 coils and 0x007B registers per write.
    quint16 maxReadBits{2000};
    quint16 maxReadRegisters{125};
    quint16 maxWriteBits{1968};
    quint16 maxWriteRegisters{123};

    /// Device address for a Modbus offset under the given function code.
    [[nodiscard]] core::Result<quint32> resolve(quint8 functionCode, quint16 offset) const;

    /// Upper bound on quantity for a read, per the specification.
    [[nodiscard]] quint16 readLimit(quint8 functionCode) const;

    [[nodiscard]] static AddressMap fromConfig(const QVariantMap& config);
    [[nodiscard]] static core::ConfigSchema schema();
};

/// Frame attribute keys shared between the codecs and the handlers.
namespace attributes {
inline constexpr auto kUnitId = "modbus.unitId";
inline constexpr auto kTransactionId = "modbus.transactionId";
} // namespace attributes

} // namespace hwsim::plugins::modbus
