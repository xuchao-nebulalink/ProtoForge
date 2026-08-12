#include "Sw6Codec.h"

#include "Sw6Commands.h"

#include <core/Crc.h>
#include <core/Endian.h>
#include <core/HexUtils.h>

#include <algorithm>

using hwsim::core::ConfigField;
using hwsim::core::ConfigSchema;
using hwsim::core::ErrorCode;
using hwsim::core::makeError;

namespace hwsim::plugins::sw6 {
namespace {

constexpr std::size_t kNotFound = static_cast<std::size_t>(-1);

char charAt(std::span<const std::byte> buffer, std::size_t index)
{
    return static_cast<char>(std::to_integer<quint8>(buffer[index]));
}

bool isCommandLetter(char character)
{
    return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z');
}

int hexDigit(char character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    return -1;
}

} // namespace

FrameScanResult Sw6Codec::scan(std::span<const std::byte> buffer, transport::Direction) const
{
    if (buffer.empty()) {
        return FrameScanResult::needMoreData();
    }

    // Section 3: the first byte routes the frame, everything else is noise to
    // be dropped one byte at a time until a header shows up.
    const auto lead = std::to_integer<quint8>(buffer[0]);
    if (lead == static_cast<quint8>(kAsciiStart)) {
        return scanAsciiCommand(buffer);
    }
    if (lead == kStreamStart) {
        return scanRealtimeStream(buffer);
    }

    return FrameScanResult::discard(
        1, QStringLiteral("lead byte 0x%1 is neither '$' nor 0x81")
               .arg(lead, 2, 16, QLatin1Char('0')));
}

FrameScanResult Sw6Codec::scanAsciiCommand(std::span<const std::byte> buffer) const
{
    // Every discard below drops a single byte rather than the whole candidate:
    // a frame that fails validation may itself be the tail of line noise, and
    // the next '$' is the only trustworthy re-anchor.
    const std::size_t limit = std::min(buffer.size(), kMaxAsciiFrameBytes);

    std::size_t terminator = kNotFound;
    for (std::size_t index = 1; index < limit; ++index) {
        const char character = charAt(buffer, index);
        if (character == kAsciiStart) {
            // Section 4.7 calls this a repeated header: the frame that started
            // here was truncated, so resume at the newer one.
            return FrameScanResult::discard(index, QStringLiteral("start byte inside a frame"));
        }
        if (character == kAsciiTerminator) {
            terminator = index;
            break;
        }
    }

    if (terminator == kNotFound) {
        return buffer.size() < kMaxAsciiFrameBytes
                   ? FrameScanResult::needMoreData()
                   : FrameScanResult::discard(
                         1, QStringLiteral("no ';' within %1 bytes").arg(kMaxAsciiFrameBytes));
    }

    // ';' plus the two checksum characters.
    const std::size_t total = terminator + 3;
    if (total > kMaxAsciiFrameBytes) {
        return FrameScanResult::discard(
            1, QStringLiteral("frame of %1 bytes exceeds the 256 byte limit").arg(total));
    }
    if (buffer.size() < total) {
        return FrameScanResult::needMoreData();
    }

    const int high = hexDigit(charAt(buffer, terminator + 1));
    const int low = hexDigit(charAt(buffer, terminator + 2));
    if (high < 0 || low < 0) {
        return FrameScanResult::discard(1, QStringLiteral("checksum is not two hex digits"));
    }

    const QByteArray raw = core::hex::toByteArray(buffer.first(total));
    const auto received = static_cast<quint8>(high * 16 + low);
    const quint8 computed = asciiChecksum(raw.first(static_cast<qsizetype>(terminator) + 1));
    if (computed != received) {
        return FrameScanResult::discard(1,
                                        QStringLiteral("checksum mismatch: computed %1, received %2")
                                            .arg(computed, 2, 16, QLatin1Char('0'))
                                            .arg(received, 2, 16, QLatin1Char('0')));
    }

    std::size_t nameEnd = terminator;
    for (std::size_t index = 1; index < terminator; ++index) {
        if (charAt(buffer, index) == ',') {
            nameEnd = index;
            break;
        }
    }

    const std::size_t nameLength = nameEnd - 1;
    if (nameLength == 0 || nameLength > static_cast<std::size_t>(kMaxCommandNameLength)) {
        return FrameScanResult::discard(
            1, QStringLiteral("command name of %1 characters").arg(nameLength));
    }
    for (std::size_t index = 1; index < nameEnd; ++index) {
        if (!isCommandLetter(charAt(buffer, index))) {
            return FrameScanResult::discard(1, QStringLiteral("command name has a non-letter"));
        }
    }

    const QString command = QString::fromLatin1(raw.mid(1, static_cast<qsizetype>(nameLength)));

    Frame frame;
    // An unrecognised name still produces a frame: section 4.6 wants
    // `$<Cmd>,0x03000001;XX` back, and only a decoded frame can be answered.
    const OpCode opcode = commandOpcodeOf(command);
    frame.opcode = isKnownCommand(opcode) ? opcode : kUnknownCommandOpcode;
    frame.raw = raw;
    if (nameEnd < terminator) {
        frame.payload = raw.mid(static_cast<qsizetype>(nameEnd) + 1,
                                static_cast<qsizetype>(terminator - nameEnd) - 1);
    }
    frame.attributes.insert(QString::fromLatin1(kCommandAttribute), command);

    return FrameScanResult::ready(std::move(frame), total);
}

FrameScanResult Sw6Codec::scanRealtimeStream(std::span<const std::byte> buffer) const
{
    if (buffer.size() < 2) {
        return FrameScanResult::needMoreData();
    }

    const auto length = std::to_integer<std::size_t>(buffer[1]);
    if (length % kStreamRecordBytes != 0) {
        return FrameScanResult::discard(
            1, QStringLiteral("realtime length %1 is not a multiple of 5").arg(length));
    }
    if (length / kStreamRecordBytes > maxStreamRecords_) {
        return FrameScanResult::discard(
            1, QStringLiteral("realtime frame declares %1 records, more than the %2 allowed")
                   .arg(length / kStreamRecordBytes)
                   .arg(maxStreamRecords_));
    }

    const std::size_t total = kStreamOverheadBytes + length;
    if (buffer.size() < total) {
        return FrameScanResult::needMoreData();
    }

    if (std::to_integer<quint8>(buffer[total - 1]) != kStreamEnd) {
        return FrameScanResult::discard(1, QStringLiteral("realtime frame has no 0x55 tail"));
    }

    // Section 6.4: the CRC covers header, length and the record area.
    const quint16 computed = core::crc::umts(buffer.first(2 + length));
    const quint16 received = core::endian::readLittle<quint16>(buffer.subspan(2 + length, 2));
    if (computed != received) {
        return FrameScanResult::discard(1,
                                        QStringLiteral("realtime CRC mismatch: computed %1, received %2")
                                            .arg(computed, 4, 16, QLatin1Char('0'))
                                            .arg(received, 4, 16, QLatin1Char('0')));
    }

    Frame frame;
    frame.opcode = kRealtimeStreamOpcode;
    frame.payload = core::hex::toByteArray(buffer.subspan(2, length));
    frame.raw = core::hex::toByteArray(buffer.first(total));

    return FrameScanResult::ready(std::move(frame), total);
}

Result<QByteArray> Sw6Codec::wrap(OpCode opcode, const QByteArray& body,
                                  const EncodeContext& context) const
{
    if (opcode == kRealtimeStreamOpcode) {
        if (body.size() % static_cast<qsizetype>(kStreamRecordBytes) != 0) {
            return makeError(ErrorCode::FrameMalformed,
                             QStringLiteral("realtime payload of %1 bytes is not a multiple of 5")
                                 .arg(body.size()));
        }
        if (body.size() > 0xFF) {
            return makeError(ErrorCode::FrameTooLarge,
                             QStringLiteral("realtime payload of %1 bytes does not fit the "
                                            "single length byte")
                                 .arg(body.size()));
        }

        QByteArray frame;
        frame.reserve(body.size() + static_cast<qsizetype>(kStreamOverheadBytes));
        frame.append(static_cast<char>(kStreamStart));
        frame.append(static_cast<char>(body.size()));
        frame.append(body);

        const quint16 crc = core::crc::umts(core::hex::asBytes(frame));
        frame.append(static_cast<char>(crc & 0xFF));
        frame.append(static_cast<char>((crc >> 8) & 0xFF));
        frame.append(static_cast<char>(kStreamEnd));
        return frame;
    }

    // The command table names known opcodes; the attribute covers the reply to
    // a command this build has never heard of, whose name still has to be
    // echoed back.
    QString command = commandNameFor(opcode);
    if (command.isEmpty()) {
        command = context.attribute(QString::fromLatin1(kCommandAttribute)).toString();
    }
    if (command.isEmpty()) {
        return makeError(ErrorCode::Unsupported,
                         QStringLiteral("opcode 0x%1 has no SW6 command name")
                             .arg(opcode, 8, 16, QLatin1Char('0')));
    }

    const QByteArray frame = buildAsciiFrame(command, body);
    if (frame.size() > static_cast<qsizetype>(kMaxAsciiFrameBytes)) {
        return makeError(ErrorCode::FrameTooLarge,
                         QStringLiteral("$%1 frame of %2 bytes exceeds the 256 byte limit")
                             .arg(command)
                             .arg(frame.size()));
    }

    // prepareRequest() allocated the token before the command name was known,
    // because the session picks the opcode only once the body is encoded. This
    // is where the two meet, so the reply can be matched by name.
    if (const QVariant token = context.attribute(QString::fromLatin1(kRequestTokenAttribute));
        token.isValid()) {
        tokenByCommand_.insert(command, token.toString());
    }

    return frame;
}

QString Sw6Codec::correlationKey(const Frame& frame) const
{
    if (frame.opcode == kRealtimeStreamOpcode) {
        return QString::fromLatin1(kStreamCorrelationKey);
    }

    const QString command = frame.attribute(QString::fromLatin1(kCommandAttribute)).toString();
    if (command.isEmpty()) {
        return {};
    }

    // Falling back to the name itself keeps the key non-empty, so a reply to
    // something we never sent is reported as unsolicited instead of completing
    // the oldest request.
    return tokenByCommand_.value(command, command);
}

QString Sw6Codec::prepareRequest(EncodeContext& context) const
{
    const QString token = QString::number(nextToken_++);
    context.attributes.insert(QString::fromLatin1(kRequestTokenAttribute), token);
    return token;
}

ConfigSchema Sw6Codec::configSchema() const
{
    ConfigSchema schema(QStringLiteral("SW6 组帧"));
    schema.add(ConfigField::integer(QStringLiteral("maxStreamRecords"),
                                    QStringLiteral("实时帧最大记录数"), 51)
                   .range(6, 51)
                   .withUnit(QStringLiteral("条"))
                   .describedAs(QStringLiteral("0x81 帧的长度字段超过该记录数即视为噪声，"
                                               "丢弃并重新寻找帧头"))
                   .asAdvanced());
    return schema;
}

Result<void> Sw6Codec::configure(const QVariantMap& config)
{
    maxStreamRecords_ = config.value(QStringLiteral("maxStreamRecords"), 51).toUInt();
    if (maxStreamRecords_ == 0 || maxStreamRecords_ > 51) {
        return makeError(ErrorCode::ConfigInvalid,
                         QStringLiteral("maxStreamRecords must be between 1 and 51"));
    }
    return core::success();
}

} // namespace hwsim::plugins::sw6
