#include "ModbusHandlers.h"

#include <core/Logger.h>

#include <QHash>

namespace {
constexpr auto kLogCategory = "plugin.modbus";
}

using hwsim::core::ErrorCode;
using hwsim::core::Result;

namespace hwsim::plugins::modbus {
namespace {

/// Device-model failures map onto the exception codes a real slave would send.
ExceptionCode exceptionFor(const core::Error& error)
{
    static const QHash<ErrorCode, ExceptionCode> mapping{
        {ErrorCode::OutOfRange, ExceptionCode::IllegalDataAddress},
        {ErrorCode::NotFound, ExceptionCode::IllegalDataAddress},
        {ErrorCode::ParameterReadOnly, ExceptionCode::IllegalDataAddress},
        {ErrorCode::InvalidArgument, ExceptionCode::IllegalDataValue},
        {ErrorCode::Unsupported, ExceptionCode::IllegalFunction},
        {ErrorCode::DeviceBusy, ExceptionCode::ServerDeviceBusy},
        {ErrorCode::NotReady, ExceptionCode::ServerDeviceBusy},
    };
    return mapping.value(error.code, ExceptionCode::ServerDeviceFailure);
}

MessagePtr exceptionFrom(quint8 functionCode, const core::Error& error)
{
    HWSIM_LOG_DEBUG(kLogCategory) << functionCodeName(functionCode) << " -> exception: "
                                  << error.toString();
    return ExceptionResponse::make(functionCode, exceptionFor(error));
}

MessagePtr exceptionOf(quint8 functionCode, ExceptionCode code)
{
    return ExceptionResponse::make(functionCode, code);
}

} // namespace

// --- Reads -----------------------------------------------------------------

Result<MessagePtr> ReadBitsHandler::handle(const ReadBitsRequest& request, ExecutionContext& context)
{
    if (request.quantity > map_.readLimit(request.functionCode)) {
        return exceptionOf(request.functionCode, ExceptionCode::IllegalDataValue);
    }

    const auto base = map_.resolve(request.functionCode, request.startAddress);
    if (base.hasError()) {
        return exceptionFrom(request.functionCode, base.error());
    }

    const auto values = context.readRange(base.value(), request.quantity);
    if (values.hasError()) {
        return exceptionFrom(request.functionCode, values.error());
    }

    auto response = std::make_shared<ReadBitsResponse>();
    response->functionCode = request.functionCode;
    response->values.reserve(values.value().size());
    for (const QVariant& value : values.value()) {
        // Any non-zero numeric or a true boolean reads as a set bit, so a coil
        // can be backed by whatever parameter type the profile chose.
        response->values.append(value.toBool() || value.toDouble() != 0.0);
    }

    return MessagePtr(response);
}

Result<MessagePtr> ReadRegistersHandler::handle(const ReadRegistersRequest& request,
                                                ExecutionContext& context)
{
    if (request.quantity > map_.readLimit(request.functionCode)) {
        return exceptionOf(request.functionCode, ExceptionCode::IllegalDataValue);
    }

    const auto base = map_.resolve(request.functionCode, request.startAddress);
    if (base.hasError()) {
        return exceptionFrom(request.functionCode, base.error());
    }

    const auto values = context.readRange(base.value(), request.quantity);
    if (values.hasError()) {
        return exceptionFrom(request.functionCode, values.error());
    }

    auto response = std::make_shared<ReadRegistersResponse>();
    response->functionCode = request.functionCode;
    response->values.reserve(values.value().size());
    for (const QVariant& value : values.value()) {
        response->values.append(static_cast<quint16>(value.toUInt() & 0xFFFF));
    }

    return MessagePtr(response);
}

// --- Single writes ---------------------------------------------------------

Result<MessagePtr> WriteSingleCoilHandler::handle(const WriteSingleCoilRequest& request,
                                                  ExecutionContext& context)
{
    const auto address = map_.resolve(fc::kWriteSingleCoil, request.address);
    if (address.hasError()) {
        return exceptionFrom(fc::kWriteSingleCoil, address.error());
    }

    if (const auto written = context.write(address.value(), request.value); written.hasError()) {
        return exceptionFrom(fc::kWriteSingleCoil, written.error());
    }

    // A successful single write is acknowledged by echoing the request verbatim.
    auto response = std::make_shared<WriteSingleResponse>();
    response->functionCode = fc::kWriteSingleCoil;
    response->address = request.address;
    response->rawValue = request.value ? 0xFF00 : 0x0000;
    return MessagePtr(response);
}

Result<MessagePtr> WriteSingleRegisterHandler::handle(const WriteSingleRegisterRequest& request,
                                                      ExecutionContext& context)
{
    const auto address = map_.resolve(fc::kWriteSingleRegister, request.address);
    if (address.hasError()) {
        return exceptionFrom(fc::kWriteSingleRegister, address.error());
    }

    if (const auto written = context.write(address.value(), request.value); written.hasError()) {
        return exceptionFrom(fc::kWriteSingleRegister, written.error());
    }

    auto response = std::make_shared<WriteSingleResponse>();
    response->functionCode = fc::kWriteSingleRegister;
    response->address = request.address;
    response->rawValue = request.value;
    return MessagePtr(response);
}

// --- Multiple writes -------------------------------------------------------

Result<MessagePtr> WriteMultipleCoilsHandler::handle(const WriteMultipleCoilsRequest& request,
                                                     ExecutionContext& context)
{
    const auto base = map_.resolve(fc::kWriteMultipleCoils, request.startAddress);
    if (base.hasError()) {
        return exceptionFrom(fc::kWriteMultipleCoils, base.error());
    }

    QVector<QVariant> values;
    values.reserve(request.values.size());
    for (const bool value : request.values) {
        values.append(value);
    }

    // writeAddressRange validates the whole span before applying any of it, so
    // a request that runs off the end of the map leaves the device untouched.
    if (const auto written = context.writeRange(base.value(), values); written.hasError()) {
        return exceptionFrom(fc::kWriteMultipleCoils, written.error());
    }

    auto response = std::make_shared<WriteMultipleResponse>();
    response->functionCode = fc::kWriteMultipleCoils;
    response->startAddress = request.startAddress;
    response->quantity = static_cast<quint16>(request.values.size());
    return MessagePtr(response);
}

Result<MessagePtr> WriteMultipleRegistersHandler::handle(
    const WriteMultipleRegistersRequest& request, ExecutionContext& context)
{
    if (request.values.size() > map_.maxWriteRegisters) {
        return exceptionOf(fc::kWriteMultipleRegisters, ExceptionCode::IllegalDataValue);
    }

    const auto base = map_.resolve(fc::kWriteMultipleRegisters, request.startAddress);
    if (base.hasError()) {
        return exceptionFrom(fc::kWriteMultipleRegisters, base.error());
    }

    QVector<QVariant> values;
    values.reserve(request.values.size());
    for (const quint16 value : request.values) {
        values.append(value);
    }

    if (const auto written = context.writeRange(base.value(), values); written.hasError()) {
        return exceptionFrom(fc::kWriteMultipleRegisters, written.error());
    }

    auto response = std::make_shared<WriteMultipleResponse>();
    response->functionCode = fc::kWriteMultipleRegisters;
    response->startAddress = request.startAddress;
    response->quantity = static_cast<quint16>(request.values.size());
    return MessagePtr(response);
}

} // namespace hwsim::plugins::modbus
