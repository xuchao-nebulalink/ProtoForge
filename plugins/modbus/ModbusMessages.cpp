#include "ModbusMessages.h"

#include <core/Endian.h>
#include <core/HexUtils.h>

using hwsim::core::ErrorCode;
using hwsim::core::makeError;

namespace hwsim::plugins::modbus {
namespace {

/// Modbus is big endian on the wire throughout.
quint16 readWord(const QByteArray& payload, qsizetype offset)
{
    return core::endian::readBig<quint16>(
        core::hex::asBytes(payload).subspan(static_cast<std::size_t>(offset), 2));
}

void appendWord(QByteArray& out, quint16 value)
{
    out.append(static_cast<char>((value >> 8) & 0xFF));
    out.append(static_cast<char>(value & 0xFF));
}

core::Result<void> requireSize(const QByteArray& payload, qsizetype minimum, const char* what)
{
    if (payload.size() < minimum) {
        return makeError(ErrorCode::FrameMalformed,
                         QStringLiteral("%1 needs at least %2 payload bytes, got %3")
                             .arg(QString::fromLatin1(what))
                             .arg(minimum)
                             .arg(payload.size()));
    }
    return core::success();
}

/// Modbus packs bits least-significant-bit first, eight per byte.
QByteArray packBits(const QVector<bool>& values)
{
    const qsizetype byteCount = (values.size() + 7) / 8;
    QByteArray packed(byteCount, '\0');
    for (qsizetype index = 0; index < values.size(); ++index) {
        if (values.at(index)) {
            packed[index / 8] = static_cast<char>(packed.at(index / 8) | (1 << (index % 8)));
        }
    }
    return packed;
}

QVector<bool> unpackBits(const QByteArray& packed, qsizetype count)
{
    QVector<bool> values;
    values.reserve(count);
    for (qsizetype index = 0; index < count; ++index) {
        const qsizetype byteIndex = index / 8;
        if (byteIndex >= packed.size()) {
            break;
        }
        values.append((packed.at(byteIndex) & (1 << (index % 8))) != 0);
    }
    return values;
}

} // namespace

// --- ReadBitsRequest -------------------------------------------------------

core::Result<ReadBitsRequest> ReadBitsRequest::decode(const Frame& frame)
{
    if (const auto sized = requireSize(frame.payload, 4, "read bits request"); sized.hasError()) {
        return sized.error();
    }

    ReadBitsRequest request;
    request.functionCode = static_cast<quint8>(frame.opcode);
    request.startAddress = readWord(frame.payload, 0);
    request.quantity = readWord(frame.payload, 2);

    if (request.quantity == 0 || request.quantity > 2000) {
        return makeError(ErrorCode::ProtocolError,
                         QStringLiteral("quantity %1 is outside 1..2000").arg(request.quantity));
    }
    return request;
}

core::Result<QByteArray> ReadBitsRequest::encodeBody(const EncodeContext&) const
{
    QByteArray body;
    appendWord(body, startAddress);
    appendWord(body, quantity);
    return body;
}

QString ReadBitsRequest::describe() const
{
    return QStringLiteral("%1 addr=%2 count=%3")
        .arg(functionCodeName(functionCode))
        .arg(startAddress)
        .arg(quantity);
}

// --- ReadRegistersRequest --------------------------------------------------

core::Result<ReadRegistersRequest> ReadRegistersRequest::decode(const Frame& frame)
{
    if (const auto sized = requireSize(frame.payload, 4, "read registers request");
        sized.hasError()) {
        return sized.error();
    }

    ReadRegistersRequest request;
    request.functionCode = static_cast<quint8>(frame.opcode);
    request.startAddress = readWord(frame.payload, 0);
    request.quantity = readWord(frame.payload, 2);

    if (request.quantity == 0 || request.quantity > 125) {
        return makeError(ErrorCode::ProtocolError,
                         QStringLiteral("quantity %1 is outside 1..125").arg(request.quantity));
    }
    return request;
}

core::Result<QByteArray> ReadRegistersRequest::encodeBody(const EncodeContext&) const
{
    QByteArray body;
    appendWord(body, startAddress);
    appendWord(body, quantity);
    return body;
}

QString ReadRegistersRequest::describe() const
{
    return QStringLiteral("%1 addr=%2 count=%3")
        .arg(functionCodeName(functionCode))
        .arg(startAddress)
        .arg(quantity);
}

// --- WriteSingleCoilRequest ------------------------------------------------

core::Result<WriteSingleCoilRequest> WriteSingleCoilRequest::decode(const Frame& frame)
{
    if (const auto sized = requireSize(frame.payload, 4, "write single coil"); sized.hasError()) {
        return sized.error();
    }

    WriteSingleCoilRequest request;
    request.address = readWord(frame.payload, 0);

    const quint16 raw = readWord(frame.payload, 2);
    if (raw != 0x0000 && raw != 0xFF00) {
        return makeError(ErrorCode::ProtocolError,
                         QStringLiteral("coil value must be 0x0000 or 0xFF00, got 0x%1")
                             .arg(raw, 4, 16, QLatin1Char('0')));
    }
    request.value = raw == 0xFF00;
    return request;
}

core::Result<QByteArray> WriteSingleCoilRequest::encodeBody(const EncodeContext&) const
{
    QByteArray body;
    appendWord(body, address);
    appendWord(body, value ? 0xFF00 : 0x0000);
    return body;
}

QString WriteSingleCoilRequest::describe() const
{
    return QStringLiteral("WriteSingleCoil addr=%1 value=%2")
        .arg(address)
        .arg(value ? QStringLiteral("ON") : QStringLiteral("OFF"));
}

// --- WriteSingleRegisterRequest --------------------------------------------

core::Result<WriteSingleRegisterRequest> WriteSingleRegisterRequest::decode(const Frame& frame)
{
    if (const auto sized = requireSize(frame.payload, 4, "write single register");
        sized.hasError()) {
        return sized.error();
    }

    WriteSingleRegisterRequest request;
    request.address = readWord(frame.payload, 0);
    request.value = readWord(frame.payload, 2);
    return request;
}

core::Result<QByteArray> WriteSingleRegisterRequest::encodeBody(const EncodeContext&) const
{
    QByteArray body;
    appendWord(body, address);
    appendWord(body, value);
    return body;
}

QString WriteSingleRegisterRequest::describe() const
{
    return QStringLiteral("WriteSingleRegister addr=%1 value=%2").arg(address).arg(value);
}

// --- WriteMultipleCoilsRequest ---------------------------------------------

core::Result<WriteMultipleCoilsRequest> WriteMultipleCoilsRequest::decode(const Frame& frame)
{
    if (const auto sized = requireSize(frame.payload, 5, "write multiple coils"); sized.hasError()) {
        return sized.error();
    }

    WriteMultipleCoilsRequest request;
    request.startAddress = readWord(frame.payload, 0);

    const quint16 quantity = readWord(frame.payload, 2);
    const auto byteCount = static_cast<quint8>(frame.payload.at(4));

    if (quantity == 0 || quantity > 1968) {
        return makeError(ErrorCode::ProtocolError,
                         QStringLiteral("quantity %1 is outside 1..1968").arg(quantity));
    }
    if (byteCount != (quantity + 7) / 8) {
        return makeError(ErrorCode::FrameMalformed,
                         QStringLiteral("byte count %1 does not match quantity %2")
                             .arg(byteCount)
                             .arg(quantity));
    }
    if (frame.payload.size() < 5 + byteCount) {
        return makeError(ErrorCode::FrameMalformed,
                         QStringLiteral("payload is shorter than the declared byte count"));
    }

    request.values = unpackBits(frame.payload.mid(5, byteCount), quantity);
    return request;
}

core::Result<QByteArray> WriteMultipleCoilsRequest::encodeBody(const EncodeContext&) const
{
    const QByteArray packed = packBits(values);

    QByteArray body;
    appendWord(body, startAddress);
    appendWord(body, static_cast<quint16>(values.size()));
    body.append(static_cast<char>(packed.size()));
    body.append(packed);
    return body;
}

QString WriteMultipleCoilsRequest::describe() const
{
    return QStringLiteral("WriteMultipleCoils addr=%1 count=%2").arg(startAddress).arg(values.size());
}

// --- WriteMultipleRegistersRequest -----------------------------------------

core::Result<WriteMultipleRegistersRequest> WriteMultipleRegistersRequest::decode(const Frame& frame)
{
    if (const auto sized = requireSize(frame.payload, 5, "write multiple registers");
        sized.hasError()) {
        return sized.error();
    }

    WriteMultipleRegistersRequest request;
    request.startAddress = readWord(frame.payload, 0);

    const quint16 quantity = readWord(frame.payload, 2);
    const auto byteCount = static_cast<quint8>(frame.payload.at(4));

    if (quantity == 0 || quantity > 123) {
        return makeError(ErrorCode::ProtocolError,
                         QStringLiteral("quantity %1 is outside 1..123").arg(quantity));
    }
    if (byteCount != quantity * 2) {
        return makeError(ErrorCode::FrameMalformed,
                         QStringLiteral("byte count %1 does not match quantity %2")
                             .arg(byteCount)
                             .arg(quantity));
    }
    if (frame.payload.size() < 5 + byteCount) {
        return makeError(ErrorCode::FrameMalformed,
                         QStringLiteral("payload is shorter than the declared byte count"));
    }

    request.values.reserve(quantity);
    for (quint16 index = 0; index < quantity; ++index) {
        request.values.append(readWord(frame.payload, 5 + index * 2));
    }
    return request;
}

core::Result<QByteArray> WriteMultipleRegistersRequest::encodeBody(const EncodeContext&) const
{
    QByteArray body;
    appendWord(body, startAddress);
    appendWord(body, static_cast<quint16>(values.size()));
    body.append(static_cast<char>(values.size() * 2));
    for (const quint16 value : values) {
        appendWord(body, value);
    }
    return body;
}

QString WriteMultipleRegistersRequest::describe() const
{
    return QStringLiteral("WriteMultipleRegisters addr=%1 count=%2")
        .arg(startAddress)
        .arg(values.size());
}

// --- ReadBitsResponse ------------------------------------------------------

core::Result<ReadBitsResponse> ReadBitsResponse::decode(const Frame& frame)
{
    if (const auto sized = requireSize(frame.payload, 1, "read bits response"); sized.hasError()) {
        return sized.error();
    }

    const auto byteCount = static_cast<quint8>(frame.payload.at(0));
    if (frame.payload.size() < 1 + byteCount) {
        return makeError(ErrorCode::FrameMalformed,
                         QStringLiteral("declared %1 data bytes but only %2 present")
                             .arg(byteCount)
                             .arg(frame.payload.size() - 1));
    }

    ReadBitsResponse response;
    response.functionCode = static_cast<quint8>(frame.opcode);
    // The response does not carry the original quantity, so every bit in the
    // payload is surfaced; the caller matches it against what it asked for.
    response.values = unpackBits(frame.payload.mid(1, byteCount), byteCount * 8);
    return response;
}

core::Result<QByteArray> ReadBitsResponse::encodeBody(const EncodeContext&) const
{
    const QByteArray packed = packBits(values);

    QByteArray body;
    body.append(static_cast<char>(packed.size()));
    body.append(packed);
    return body;
}

QString ReadBitsResponse::describe() const
{
    return QStringLiteral("%1 response, %2 bit(s)")
        .arg(functionCodeName(functionCode))
        .arg(values.size());
}

// --- ReadRegistersResponse -------------------------------------------------

core::Result<ReadRegistersResponse> ReadRegistersResponse::decode(const Frame& frame)
{
    if (const auto sized = requireSize(frame.payload, 1, "read registers response");
        sized.hasError()) {
        return sized.error();
    }

    const auto byteCount = static_cast<quint8>(frame.payload.at(0));
    if (byteCount % 2 != 0) {
        return makeError(ErrorCode::FrameMalformed,
                         QStringLiteral("register byte count %1 is odd").arg(byteCount));
    }
    if (frame.payload.size() < 1 + byteCount) {
        return makeError(ErrorCode::FrameMalformed,
                         QStringLiteral("declared %1 data bytes but only %2 present")
                             .arg(byteCount)
                             .arg(frame.payload.size() - 1));
    }

    ReadRegistersResponse response;
    response.functionCode = static_cast<quint8>(frame.opcode);
    response.values.reserve(byteCount / 2);
    for (int index = 0; index < byteCount / 2; ++index) {
        response.values.append(readWord(frame.payload, 1 + index * 2));
    }
    return response;
}

core::Result<QByteArray> ReadRegistersResponse::encodeBody(const EncodeContext&) const
{
    QByteArray body;
    body.append(static_cast<char>(values.size() * 2));
    for (const quint16 value : values) {
        appendWord(body, value);
    }
    return body;
}

QString ReadRegistersResponse::describe() const
{
    QStringList preview;
    for (int index = 0; index < qMin<qsizetype>(values.size(), 8); ++index) {
        preview.append(QString::number(values.at(index)));
    }
    if (values.size() > 8) {
        preview.append(QStringLiteral("..."));
    }

    return QStringLiteral("%1 response, %2 reg(s) [%3]")
        .arg(functionCodeName(functionCode))
        .arg(values.size())
        .arg(preview.join(QStringLiteral(", ")));
}

// --- WriteSingleResponse ---------------------------------------------------

core::Result<WriteSingleResponse> WriteSingleResponse::decode(const Frame& frame)
{
    if (const auto sized = requireSize(frame.payload, 4, "write single response");
        sized.hasError()) {
        return sized.error();
    }

    WriteSingleResponse response;
    response.functionCode = static_cast<quint8>(frame.opcode);
    response.address = readWord(frame.payload, 0);
    response.rawValue = readWord(frame.payload, 2);
    return response;
}

core::Result<QByteArray> WriteSingleResponse::encodeBody(const EncodeContext&) const
{
    QByteArray body;
    appendWord(body, address);
    appendWord(body, rawValue);
    return body;
}

QString WriteSingleResponse::describe() const
{
    return QStringLiteral("%1 response addr=%2 value=%3")
        .arg(functionCodeName(functionCode))
        .arg(address)
        .arg(rawValue);
}

// --- WriteMultipleResponse -------------------------------------------------

core::Result<WriteMultipleResponse> WriteMultipleResponse::decode(const Frame& frame)
{
    if (const auto sized = requireSize(frame.payload, 4, "write multiple response");
        sized.hasError()) {
        return sized.error();
    }

    WriteMultipleResponse response;
    response.functionCode = static_cast<quint8>(frame.opcode);
    response.startAddress = readWord(frame.payload, 0);
    response.quantity = readWord(frame.payload, 2);
    return response;
}

core::Result<QByteArray> WriteMultipleResponse::encodeBody(const EncodeContext&) const
{
    QByteArray body;
    appendWord(body, startAddress);
    appendWord(body, quantity);
    return body;
}

QString WriteMultipleResponse::describe() const
{
    return QStringLiteral("%1 response addr=%2 count=%3")
        .arg(functionCodeName(functionCode))
        .arg(startAddress)
        .arg(quantity);
}

// --- ExceptionResponse -----------------------------------------------------

core::Result<ExceptionResponse> ExceptionResponse::decode(const Frame& frame)
{
    if (const auto sized = requireSize(frame.payload, 1, "exception response"); sized.hasError()) {
        return sized.error();
    }

    ExceptionResponse response;
    response.functionCode = static_cast<quint8>(frame.opcode) & ~fc::kExceptionFlag;
    response.exceptionCode = static_cast<ExceptionCode>(frame.payload.at(0));
    return response;
}

core::Result<QByteArray> ExceptionResponse::encodeBody(const EncodeContext&) const
{
    QByteArray body;
    body.append(static_cast<char>(exceptionCode));
    return body;
}

QString ExceptionResponse::describe() const
{
    return QStringLiteral("Exception on %1: %2 (0x%3)")
        .arg(functionCodeName(functionCode), exceptionCodeName(exceptionCode))
        .arg(static_cast<quint8>(exceptionCode), 2, 16, QLatin1Char('0'));
}

std::shared_ptr<ExceptionResponse> ExceptionResponse::make(quint8 originalFunctionCode,
                                                           ExceptionCode code)
{
    auto response = std::make_shared<ExceptionResponse>();
    response->functionCode = static_cast<quint8>(originalFunctionCode & ~fc::kExceptionFlag);
    response->exceptionCode = code;
    return response;
}

} // namespace hwsim::plugins::modbus
