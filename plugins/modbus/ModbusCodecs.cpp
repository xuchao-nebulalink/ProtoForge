#include "ModbusCodecs.h"

#include <core/Crc.h>
#include <core/Endian.h>
#include <core/HexUtils.h>
#include <protocol/IProtocolPlugin.h>

#include <optional>

using hwsim::core::ConfigField;
using hwsim::core::ConfigSchema;
using hwsim::core::ErrorCode;
using hwsim::core::makeError;
using hwsim::core::Result;
using hwsim::protocol::Frame;
using hwsim::protocol::FrameScanStatus;

namespace hwsim::plugins::modbus {
namespace {

constexpr std::size_t kRtuOverhead = 4;   // unit + function + crc16
constexpr std::size_t kMbapHeader = 6;    // transaction + protocol + length
constexpr quint16 kMbapProtocolId = 0;

enum class LengthStatus {
    Known,
    NeedMore,
    Unknown,
};

struct LengthResult {
    LengthStatus status{LengthStatus::NeedMore};
    std::size_t total{0};
};

/// How to determine the length of one RTU frame for a given function code.
///
/// Either the frame is a fixed size, or it carries a byte count at a known
/// offset. Expressing it as a table rather than nested switches means a
/// vendor-specific function code is one more row, and the same table shape
/// serves both parsing directions.
struct RtuLayout {
    /// Total frame length when the size is fixed, otherwise 0.
    std::size_t fixedTotal{0};
    /// Offset of the byte-count field, or -1 when the size is fixed.
    int byteCountOffset{-1};
    /// Bytes surrounding the counted data: everything except the data itself.
    std::size_t overhead{0};
};

const QHash<quint8, RtuLayout>& rtuRequestLayouts()
{
    // unit + fn + start(2) + qty(2) + crc(2) = 8
    static const RtuLayout kFixed8{8, -1, 0};
    // unit + fn + start(2) + qty(2) + byteCount(1) + data + crc(2) = 9 + data
    static const RtuLayout kCounted{0, 6, 9};

    static const QHash<quint8, RtuLayout> layouts{
        {fc::kReadCoils, kFixed8},
        {fc::kReadDiscreteInputs, kFixed8},
        {fc::kReadHoldingRegisters, kFixed8},
        {fc::kReadInputRegisters, kFixed8},
        {fc::kWriteSingleCoil, kFixed8},
        {fc::kWriteSingleRegister, kFixed8},
        {fc::kWriteMultipleCoils, kCounted},
        {fc::kWriteMultipleRegisters, kCounted},
    };
    return layouts;
}

const QHash<quint8, RtuLayout>& rtuResponseLayouts()
{
    static const RtuLayout kFixed8{8, -1, 0};
    // unit + fn + byteCount(1) + data + crc(2) = 5 + data
    static const RtuLayout kCounted{0, 2, 5};

    static const QHash<quint8, RtuLayout> layouts{
        {fc::kReadCoils, kCounted},
        {fc::kReadDiscreteInputs, kCounted},
        {fc::kReadHoldingRegisters, kCounted},
        {fc::kReadInputRegisters, kCounted},
        {fc::kWriteSingleCoil, kFixed8},
        {fc::kWriteSingleRegister, kFixed8},
        {fc::kWriteMultipleCoils, kFixed8},
        {fc::kWriteMultipleRegisters, kFixed8},
    };
    return layouts;
}

/// Total RTU frame length including the two CRC bytes.
LengthResult expectedRtuLength(std::span<const std::byte> buffer, bool decodingRequests)
{
    if (buffer.size() < 2) {
        return {LengthStatus::NeedMore, 0};
    }

    const auto functionCode = std::to_integer<quint8>(buffer[1]);

    // unit + function + exception code + crc16, whichever function failed.
    if (fc::isException(functionCode)) {
        return {LengthStatus::Known, 5};
    }

    const auto& layouts = decodingRequests ? rtuRequestLayouts() : rtuResponseLayouts();
    const auto it = layouts.constFind(functionCode);
    if (it == layouts.constEnd()) {
        return {LengthStatus::Unknown, 0};
    }

    const RtuLayout& layout = it.value();
    if (layout.byteCountOffset < 0) {
        return {LengthStatus::Known, layout.fixedTotal};
    }

    const auto offset = static_cast<std::size_t>(layout.byteCountOffset);
    if (buffer.size() <= offset) {
        return {LengthStatus::NeedMore, 0};
    }
    const auto byteCount = std::to_integer<std::size_t>(buffer[offset]);
    return {LengthStatus::Known, layout.overhead + byteCount};
}

} // namespace

// --- Options ---------------------------------------------------------------

ConfigSchema ModbusCodecOptions::schema()
{
    ConfigSchema schema(QStringLiteral("Modbus 站点"));
    schema.add(ConfigField::integer(QStringLiteral("unitId"), QStringLiteral("站号"), 1)
                   .range(0, 247)
                   .describedAs(QStringLiteral("RTU 从站地址；TCP 下作为 MBAP 单元标识")));
    schema.add(ConfigField::boolean(QStringLiteral("acceptAnyUnitId"),
                                    QStringLiteral("接受任意站号"), false)
                   .describedAs(QStringLiteral("开启后对所有站号的请求都应答，便于抓包分析"))
                   .asAdvanced());
    return schema;
}

ModbusCodecOptions ModbusCodecOptions::fromConfig(const QVariantMap& config)
{
    ModbusCodecOptions options;
    options.unitId = static_cast<quint8>(config.value(QStringLiteral("unitId"), 1).toUInt());
    options.acceptAnyUnitId = config.value(QStringLiteral("acceptAnyUnitId"), false).toBool();

    options.decodesRequests =
        config.value(QString::fromLatin1(protocol::reserved::kRole),
                     QString::fromLatin1(protocol::reserved::kRoleResponder))
            .toString()
        != QString::fromLatin1(protocol::reserved::kRoleInitiator);
    return options;
}

bool ModbusCodecOptions::configHasRole(const QVariantMap& config)
{
    return config.contains(QString::fromLatin1(protocol::reserved::kRole));
}

// --- RTU -------------------------------------------------------------------

ModbusRtuCodec::ModbusRtuCodec(ModbusCodecOptions options) : options_(options) {}

FrameScanResult ModbusRtuCodec::scan(std::span<const std::byte> buffer,
                                     transport::Direction direction) const
{
    Q_UNUSED(direction)

    if (buffer.size() < kRtuOverhead) {
        return FrameScanResult::needMoreData();
    }

    const LengthResult length = expectedRtuLength(buffer, options_.decodesRequests);
    if (length.status == LengthStatus::NeedMore) {
        return FrameScanResult::needMoreData();
    }
    if (length.status == LengthStatus::Unknown) {
        // Cannot tell where this frame ends, so drop one byte and resynchronise
        // rather than guessing and consuming part of the next frame.
        return FrameScanResult::discard(
            1, QStringLiteral("unsupported function code 0x%1")
                   .arg(std::to_integer<quint8>(buffer[1]), 2, 16, QLatin1Char('0')));
    }
    if (buffer.size() < length.total) {
        return FrameScanResult::needMoreData();
    }

    const auto body = buffer.first(length.total - 2);
    const quint16 computed = core::crc::modbus(body);
    const quint16 received = core::endian::readLittle<quint16>(
        buffer.subspan(length.total - 2, 2));

    if (computed != received) {
        // Drop a single byte, not the whole computed length. That length came
        // from a header we have just failed to validate, and the usual cause of
        // a CRC failure is junk *before* a real frame, so consuming the guessed
        // length would eat the following frame's header and cascade the loss.
        return FrameScanResult::discard(
            1, QStringLiteral("CRC mismatch: computed %1, received %2")
                   .arg(computed, 4, 16, QLatin1Char('0'))
                   .arg(received, 4, 16, QLatin1Char('0')));
    }

    const auto unitId = std::to_integer<quint8>(buffer[0]);
    if (!options_.acceptAnyUnitId && options_.decodesRequests && unitId != options_.unitId
        && unitId != 0) {
        // Address 0 is the broadcast address and is always accepted.
        return FrameScanResult::discard(
            length.total,
            QStringLiteral("addressed to unit %1, we are %2").arg(unitId).arg(options_.unitId));
    }

    Frame frame;
    frame.opcode = std::to_integer<OpCode>(buffer[1]);
    frame.payload = core::hex::toByteArray(buffer.subspan(2, length.total - kRtuOverhead));
    frame.raw = core::hex::toByteArray(buffer.first(length.total));
    frame.attributes.insert(QString::fromLatin1(attributes::kUnitId), unitId);

    return FrameScanResult::ready(std::move(frame), length.total);
}

Result<QByteArray> ModbusRtuCodec::wrap(OpCode opcode, const QByteArray& body,
                                        const EncodeContext& context) const
{
    const auto unitId = static_cast<quint8>(
        context.attribute(QString::fromLatin1(attributes::kUnitId), options_.unitId).toUInt());

    QByteArray frame;
    frame.reserve(body.size() + static_cast<qsizetype>(kRtuOverhead));
    frame.append(static_cast<char>(unitId));
    frame.append(static_cast<char>(opcode & 0xFF));
    frame.append(body);

    if (frame.size() + 2 > 256) {
        return makeError(ErrorCode::FrameTooLarge,
                         QStringLiteral("RTU frame would be %1 bytes, the limit is 256")
                             .arg(frame.size() + 2));
    }

    const quint16 crc = core::crc::modbus(core::hex::asBytes(frame));
    frame.append(static_cast<char>(crc & 0xFF));
    frame.append(static_cast<char>((crc >> 8) & 0xFF));
    return frame;
}

ConfigSchema ModbusRtuCodec::configSchema() const
{
    return ModbusCodecOptions::schema();
}

Result<void> ModbusRtuCodec::configure(const QVariantMap& config)
{
    const bool previousDecodesRequests = options_.decodesRequests;
    options_ = ModbusCodecOptions::fromConfig(config);

    // Re-applying settings from the generated panel must not change the framing
    // direction. That panel is built from this plugin's schema, which by
    // contract excludes the reserved role key, so a plain fromConfig() would
    // flip an initiator back to parsing requests on every settings change --
    // and for RTU that silently mis-frames every response.
    if (!ModbusCodecOptions::configHasRole(config)) {
        options_.decodesRequests = previousDecodesRequests;
    }
    return core::success();
}

// --- TCP -------------------------------------------------------------------

ModbusTcpCodec::ModbusTcpCodec(ModbusCodecOptions options) : options_(options) {}

FrameScanResult ModbusTcpCodec::scan(std::span<const std::byte> buffer,
                                     transport::Direction direction) const
{
    Q_UNUSED(direction)

    if (buffer.size() < kMbapHeader + 1) {
        return FrameScanResult::needMoreData();
    }

    const quint16 protocolId = core::endian::readBig<quint16>(buffer.subspan(2, 2));
    if (protocolId != kMbapProtocolId) {
        return FrameScanResult::discard(
            1, QStringLiteral("MBAP protocol id %1 is not Modbus").arg(protocolId));
    }

    const quint16 declaredLength = core::endian::readBig<quint16>(buffer.subspan(4, 2));
    if (declaredLength < 2 || declaredLength > 253) {
        return FrameScanResult::discard(
            1, QStringLiteral("MBAP length field %1 is out of range").arg(declaredLength));
    }

    const std::size_t total = kMbapHeader + declaredLength;
    if (buffer.size() < total) {
        return FrameScanResult::needMoreData();
    }

    const quint16 transactionId = core::endian::readBig<quint16>(buffer.first(2));
    const auto unitId = std::to_integer<quint8>(buffer[6]);

    if (!options_.acceptAnyUnitId && options_.decodesRequests && unitId != options_.unitId
        && unitId != 0) {
        return FrameScanResult::discard(
            total, QStringLiteral("addressed to unit %1, we are %2").arg(unitId).arg(options_.unitId));
    }

    Frame frame;
    frame.opcode = std::to_integer<OpCode>(buffer[7]);
    frame.payload = core::hex::toByteArray(buffer.subspan(8, total - 8));
    frame.raw = core::hex::toByteArray(buffer.first(total));
    frame.attributes.insert(QString::fromLatin1(attributes::kUnitId), unitId);
    frame.attributes.insert(QString::fromLatin1(attributes::kTransactionId), transactionId);

    return FrameScanResult::ready(std::move(frame), total);
}

Result<QByteArray> ModbusTcpCodec::wrap(OpCode opcode, const QByteArray& body,
                                        const EncodeContext& context) const
{
    const auto unitId = static_cast<quint8>(
        context.attribute(QString::fromLatin1(attributes::kUnitId), options_.unitId).toUInt());
    const auto transactionId = static_cast<quint16>(
        context.attribute(QString::fromLatin1(attributes::kTransactionId), 0).toUInt());

    const qsizetype pduLength = body.size() + 2;  // unit + function + body
    if (pduLength > 253) {
        return makeError(ErrorCode::FrameTooLarge,
                         QStringLiteral("MBAP PDU would be %1 bytes, the limit is 253")
                             .arg(pduLength));
    }

    QByteArray frame;
    frame.reserve(body.size() + static_cast<qsizetype>(kMbapHeader) + 2);
    frame.append(static_cast<char>((transactionId >> 8) & 0xFF));
    frame.append(static_cast<char>(transactionId & 0xFF));
    frame.append(static_cast<char>(0));
    frame.append(static_cast<char>(0));
    frame.append(static_cast<char>((pduLength >> 8) & 0xFF));
    frame.append(static_cast<char>(pduLength & 0xFF));
    frame.append(static_cast<char>(unitId));
    frame.append(static_cast<char>(opcode & 0xFF));
    frame.append(body);
    return frame;
}

QString ModbusTcpCodec::correlationKey(const Frame& frame) const
{
    const QVariant transactionId = frame.attribute(QString::fromLatin1(attributes::kTransactionId));
    return transactionId.isValid() ? transactionId.toString() : QString{};
}

QString ModbusTcpCodec::prepareRequest(EncodeContext& context) const
{
    // Wraps at 65535 and skips 0, which some gateways treat as "unset".
    quint16 id = nextTransactionId_.fetch_add(1, std::memory_order_relaxed);
    if (id == 0) {
        id = nextTransactionId_.fetch_add(1, std::memory_order_relaxed);
    }

    context.attributes.insert(QString::fromLatin1(attributes::kTransactionId), id);
    if (!context.attributes.contains(QString::fromLatin1(attributes::kUnitId))) {
        context.attributes.insert(QString::fromLatin1(attributes::kUnitId), options_.unitId);
    }
    return QString::number(id);
}

ConfigSchema ModbusTcpCodec::configSchema() const
{
    return ModbusCodecOptions::schema();
}

Result<void> ModbusTcpCodec::configure(const QVariantMap& config)
{
    const bool previousDecodesRequests = options_.decodesRequests;
    options_ = ModbusCodecOptions::fromConfig(config);

    // Framing here comes from the MBAP length field, so the role does not
    // affect it; preserved anyway to keep the two codecs behaving alike.
    if (!ModbusCodecOptions::configHasRole(config)) {
        options_.decodesRequests = previousDecodesRequests;
    }
    return core::success();
}

} // namespace hwsim::plugins::modbus
