#include "TemplateProtocol.h"

#include <core/Crc.h>
#include <core/Endian.h>
#include <core/HexUtils.h>

#include <QHash>

#include <bit>

using hwsim::core::ConfigField;
using hwsim::core::ConfigSchema;
using hwsim::core::ErrorCode;
using hwsim::core::makeError;
using hwsim::protocol::FrameScanStatus;

namespace hwsim::plugins::tlv {
namespace {

void appendWord(QByteArray& out, quint16 value)
{
    out.append(static_cast<char>((value >> 8) & 0xFF));
    out.append(static_cast<char>(value & 0xFF));
}

void appendDoubleWord(QByteArray& out, quint32 value)
{
    appendWord(out, static_cast<quint16>((value >> 16) & 0xFFFF));
    appendWord(out, static_cast<quint16>(value & 0xFFFF));
}

quint16 readWord(const QByteArray& data, qsizetype offset)
{
    return core::endian::readBig<quint16>(
        core::hex::asBytes(data).subspan(static_cast<std::size_t>(offset), 2));
}

quint32 readDoubleWord(const QByteArray& data, qsizetype offset)
{
    return core::endian::readBig<quint32>(
        core::hex::asBytes(data).subspan(static_cast<std::size_t>(offset), 4));
}

/// Byte width of a fixed-size TLV value, or 0 when the value is variable length.
std::size_t fixedWidthOf(TlvType type)
{
    static const QHash<TlvType, std::size_t> widths{
        {TlvType::Bool, 1},
        {TlvType::UInt16, 2},
        {TlvType::UInt32, 4},
        {TlvType::Float, 4},
        {TlvType::String, 0},
    };
    return widths.value(type, 0);
}

} // namespace

// --- TLV items -------------------------------------------------------------

QByteArray TlvItem::encode() const
{
    QByteArray encoded;
    appendWord(encoded, tag);
    encoded.append(static_cast<char>(type));

    QByteArray body;
    if (type == TlvType::Bool) {
        body.append(static_cast<char>(value.toBool() ? 1 : 0));
    } else if (type == TlvType::UInt16) {
        appendWord(body, static_cast<quint16>(value.toUInt() & 0xFFFF));
    } else if (type == TlvType::UInt32) {
        appendDoubleWord(body, value.toUInt());
    } else if (type == TlvType::Float) {
        appendDoubleWord(body, std::bit_cast<quint32>(value.toFloat()));
    } else {
        body = value.toString().toUtf8().left(255);
    }

    encoded.append(static_cast<char>(body.size()));
    encoded.append(body);
    return encoded;
}

Result<TlvItem> TlvItem::decode(const QByteArray& payload, qsizetype& offset)
{
    if (payload.size() - offset < 4) {
        return makeError(ErrorCode::FrameMalformed,
                         QStringLiteral("truncated TLV header at offset %1").arg(offset));
    }

    TlvItem item;
    item.tag = readWord(payload, offset);
    item.type = static_cast<TlvType>(static_cast<quint8>(payload.at(offset + 2)));
    const auto length = static_cast<qsizetype>(static_cast<quint8>(payload.at(offset + 3)));

    if (payload.size() - offset - 4 < length) {
        return makeError(ErrorCode::FrameMalformed,
                         QStringLiteral("TLV tag %1 declares %2 bytes but only %3 remain")
                             .arg(item.tag)
                             .arg(length)
                             .arg(payload.size() - offset - 4));
    }

    const std::size_t expected = fixedWidthOf(item.type);
    if (expected != 0 && static_cast<std::size_t>(length) != expected) {
        return makeError(ErrorCode::FrameMalformed,
                         QStringLiteral("TLV tag %1 of type %2 must be %3 bytes, got %4")
                             .arg(item.tag)
                             .arg(static_cast<int>(item.type))
                             .arg(expected)
                             .arg(length));
    }

    const QByteArray body = payload.mid(offset + 4, length);
    if (item.type == TlvType::Bool) {
        item.value = body.at(0) != 0;
    } else if (item.type == TlvType::UInt16) {
        item.value = readWord(body, 0);
    } else if (item.type == TlvType::UInt32) {
        item.value = readDoubleWord(body, 0);
    } else if (item.type == TlvType::Float) {
        item.value = std::bit_cast<float>(readDoubleWord(body, 0));
    } else {
        item.value = QString::fromUtf8(body);
    }

    offset += 4 + length;
    return item;
}

Result<QVector<TlvItem>> decodeItems(const QByteArray& payload)
{
    QVector<TlvItem> items;
    qsizetype offset = 0;

    while (offset < payload.size()) {
        auto item = TlvItem::decode(payload, offset);
        if (item.hasError()) {
            return item.error();
        }
        items.append(std::move(item).value());
    }
    return items;
}

QByteArray encodeItems(const QVector<TlvItem>& items)
{
    QByteArray payload;
    for (const TlvItem& item : items) {
        payload.append(item.encode());
    }
    return payload;
}

// --- Messages --------------------------------------------------------------

Result<HeartbeatRequest> HeartbeatRequest::decode(const Frame& frame)
{
    HeartbeatRequest request;
    if (frame.payload.size() >= 4) {
        request.uptimeSeconds = readDoubleWord(frame.payload, 0);
    }
    return request;
}

Result<QByteArray> HeartbeatRequest::encodeBody(const EncodeContext&) const
{
    QByteArray body;
    appendDoubleWord(body, uptimeSeconds);
    return body;
}

QString HeartbeatRequest::describe() const
{
    return QStringLiteral("Heartbeat uptime=%1s").arg(uptimeSeconds);
}

Result<HeartbeatResponse> HeartbeatResponse::decode(const Frame& frame)
{
    if (frame.payload.size() < 5) {
        return makeError(ErrorCode::FrameMalformed,
                         QStringLiteral("heartbeat response needs at least 5 bytes"));
    }

    HeartbeatResponse response;
    response.uptimeSeconds = readDoubleWord(frame.payload, 0);

    const auto stateLength = static_cast<qsizetype>(static_cast<quint8>(frame.payload.at(4)));
    response.state = QString::fromUtf8(frame.payload.mid(5, stateLength));
    return response;
}

Result<QByteArray> HeartbeatResponse::encodeBody(const EncodeContext&) const
{
    const QByteArray stateBytes = state.toUtf8().left(255);

    QByteArray body;
    appendDoubleWord(body, uptimeSeconds);
    body.append(static_cast<char>(stateBytes.size()));
    body.append(stateBytes);
    return body;
}

QString HeartbeatResponse::describe() const
{
    return QStringLiteral("Heartbeat response uptime=%1s state=%2").arg(uptimeSeconds).arg(state);
}

Result<ReadTagsRequest> ReadTagsRequest::decode(const Frame& frame)
{
    if (frame.payload.size() % 2 != 0) {
        return makeError(ErrorCode::FrameMalformed,
                         QStringLiteral("tag list length %1 is not a multiple of 2")
                             .arg(frame.payload.size()));
    }

    ReadTagsRequest request;
    request.tags.reserve(frame.payload.size() / 2);
    for (qsizetype offset = 0; offset < frame.payload.size(); offset += 2) {
        request.tags.append(readWord(frame.payload, offset));
    }
    return request;
}

Result<QByteArray> ReadTagsRequest::encodeBody(const EncodeContext&) const
{
    QByteArray body;
    for (const quint16 tag : tags) {
        appendWord(body, tag);
    }
    return body;
}

QString ReadTagsRequest::describe() const
{
    return QStringLiteral("ReadTags count=%1").arg(tags.size());
}

Result<ReadTagsResponse> ReadTagsResponse::decode(const Frame& frame)
{
    auto items = decodeItems(frame.payload);
    if (items.hasError()) {
        return items.error();
    }

    ReadTagsResponse response;
    response.items = std::move(items).value();
    return response;
}

Result<QByteArray> ReadTagsResponse::encodeBody(const EncodeContext&) const
{
    return encodeItems(items);
}

QString ReadTagsResponse::describe() const
{
    return QStringLiteral("ReadTags response, %1 item(s)").arg(items.size());
}

Result<WriteTagsRequest> WriteTagsRequest::decode(const Frame& frame)
{
    auto items = decodeItems(frame.payload);
    if (items.hasError()) {
        return items.error();
    }

    WriteTagsRequest request;
    request.items = std::move(items).value();
    return request;
}

Result<QByteArray> WriteTagsRequest::encodeBody(const EncodeContext&) const
{
    return encodeItems(items);
}

QString WriteTagsRequest::describe() const
{
    return QStringLiteral("WriteTags count=%1").arg(items.size());
}

Result<WriteTagsResponse> WriteTagsResponse::decode(const Frame& frame)
{
    if (frame.payload.size() < 2) {
        return makeError(ErrorCode::FrameMalformed,
                         QStringLiteral("write response needs 2 bytes"));
    }

    WriteTagsResponse response;
    response.acceptedCount = readWord(frame.payload, 0);
    return response;
}

Result<QByteArray> WriteTagsResponse::encodeBody(const EncodeContext&) const
{
    QByteArray body;
    appendWord(body, acceptedCount);
    return body;
}

QString WriteTagsResponse::describe() const
{
    return QStringLiteral("WriteTags response, %1 accepted").arg(acceptedCount);
}

Result<ErrorResponse> ErrorResponse::decode(const Frame& frame)
{
    if (frame.payload.size() < 3) {
        return makeError(ErrorCode::FrameMalformed,
                         QStringLiteral("error response needs at least 3 bytes"));
    }

    ErrorResponse response;
    response.originalCommand = readWord(frame.payload, 0);
    response.code = static_cast<quint8>(frame.payload.at(2));
    response.detail = QString::fromUtf8(frame.payload.mid(3));
    return response;
}

Result<QByteArray> ErrorResponse::encodeBody(const EncodeContext&) const
{
    QByteArray body;
    appendWord(body, originalCommand);
    body.append(static_cast<char>(code));
    body.append(detail.toUtf8().left(200));
    return body;
}

QString ErrorResponse::describe() const
{
    return QStringLiteral("Error on 0x%1 code=%2 (%3)")
        .arg(originalCommand, 4, 16, QLatin1Char('0'))
        .arg(code)
        .arg(detail);
}

std::shared_ptr<ErrorResponse> ErrorResponse::make(quint16 command, quint8 errorCode, QString detail)
{
    auto response = std::make_shared<ErrorResponse>();
    response->originalCommand = command;
    response->code = errorCode;
    response->detail = std::move(detail);
    return response;
}

// --- Codec -----------------------------------------------------------------

FrameScanResult TlvCodec::scan(std::span<const std::byte> buffer, transport::Direction) const
{
    if (buffer.size() < kHeaderSize) {
        return FrameScanResult::needMoreData();
    }

    if (std::to_integer<quint8>(buffer[0]) != kStartByte) {
        // Drop one byte and look again: this is what lets the session recover
        // from line noise instead of wedging.
        return FrameScanResult::discard(1, QStringLiteral("start byte mismatch"));
    }

    const auto version = std::to_integer<quint8>(buffer[1]);
    if (version != kProtocolVersion) {
        return FrameScanResult::discard(
            1, QStringLiteral("unsupported protocol version %1").arg(version));
    }

    const quint16 payloadLength = core::endian::readBig<quint16>(buffer.subspan(6, 2));
    if (payloadLength > maxPayloadBytes_) {
        return FrameScanResult::discard(
            1, QStringLiteral("declared payload %1 exceeds the %2 byte limit")
                   .arg(payloadLength)
                   .arg(maxPayloadBytes_));
    }

    const std::size_t total = kHeaderSize + payloadLength + kChecksumSize;
    if (buffer.size() < total) {
        return FrameScanResult::needMoreData();
    }

    const quint16 computed = core::crc::ccittFalse(buffer.first(total - kChecksumSize));
    const quint16 received = core::endian::readBig<quint16>(
        buffer.subspan(total - kChecksumSize, kChecksumSize));

    if (computed != received) {
        return FrameScanResult::discard(total,
                                        QStringLiteral("CRC mismatch: computed %1, received %2")
                                            .arg(computed, 4, 16, QLatin1Char('0'))
                                            .arg(received, 4, 16, QLatin1Char('0')));
    }

    Frame frame;
    frame.opcode = core::endian::readBig<quint16>(buffer.subspan(4, 2));
    frame.payload = core::hex::toByteArray(buffer.subspan(kHeaderSize, payloadLength));
    frame.raw = core::hex::toByteArray(buffer.first(total));
    frame.attributes.insert(QString::fromLatin1(kSequenceAttribute),
                            core::endian::readBig<quint16>(buffer.subspan(2, 2)));

    return FrameScanResult::ready(std::move(frame), total);
}

Result<QByteArray> TlvCodec::wrap(OpCode opcode, const QByteArray& body,
                                  const EncodeContext& context) const
{
    if (static_cast<quint32>(body.size()) > maxPayloadBytes_) {
        return makeError(ErrorCode::FrameTooLarge,
                         QStringLiteral("payload of %1 bytes exceeds the %2 byte limit")
                             .arg(body.size())
                             .arg(maxPayloadBytes_));
    }

    const auto sequence = static_cast<quint16>(
        context.attribute(QString::fromLatin1(kSequenceAttribute), 0).toUInt());

    QByteArray frame;
    frame.reserve(body.size() + static_cast<qsizetype>(kHeaderSize + kChecksumSize));
    frame.append(static_cast<char>(kStartByte));
    frame.append(static_cast<char>(kProtocolVersion));
    appendWord(frame, sequence);
    appendWord(frame, static_cast<quint16>(opcode & 0xFFFF));
    appendWord(frame, static_cast<quint16>(body.size()));
    frame.append(body);

    appendWord(frame, core::crc::ccittFalse(core::hex::asBytes(frame)));
    return frame;
}

QString TlvCodec::correlationKey(const Frame& frame) const
{
    const QVariant sequence = frame.attribute(QString::fromLatin1(kSequenceAttribute));
    return sequence.isValid() ? sequence.toString() : QString{};
}

QString TlvCodec::prepareRequest(EncodeContext& context) const
{
    const quint16 sequence = nextSequence_++;
    if (nextSequence_ == 0) {
        nextSequence_ = 1;
    }
    context.attributes.insert(QString::fromLatin1(kSequenceAttribute), sequence);
    return QString::number(sequence);
}

ConfigSchema TlvCodec::configSchema() const
{
    ConfigSchema schema(QStringLiteral("TLV 组帧"));
    schema.add(ConfigField::integer(QStringLiteral("maxPayloadBytes"),
                                    QStringLiteral("最大负载长度"), 4096)
                   .range(16, 65535)
                   .withUnit(QStringLiteral("字节"))
                   .describedAs(QStringLiteral("超过此长度的长度字段视为噪声，触发重同步")));
    return schema;
}

Result<void> TlvCodec::configure(const QVariantMap& config)
{
    maxPayloadBytes_ = config.value(QStringLiteral("maxPayloadBytes"), 4096).toUInt();
    return core::success();
}

// --- Handlers --------------------------------------------------------------

Result<MessagePtr> HeartbeatHandler::handle(const HeartbeatRequest& request,
                                            ExecutionContext& context)
{
    auto response = std::make_shared<HeartbeatResponse>();
    response->uptimeSeconds = request.uptimeSeconds;
    response->state = context.device() != nullptr ? context.device()->currentState()
                                                  : QStringLiteral("Unknown");
    return MessagePtr(response);
}

Result<MessagePtr> ReadTagsHandler::handle(const ReadTagsRequest& request,
                                           ExecutionContext& context)
{
    auto response = std::make_shared<ReadTagsResponse>();
    response->items.reserve(request.tags.size());

    for (const quint16 tag : request.tags) {
        const auto value = context.read(tagBase_ + tag);
        if (value.hasError()) {
            // A private protocol can be strict: one bad tag fails the request.
            return MessagePtr(ErrorResponse::make(cmd::kReadTags, 0x02,
                                                  QStringLiteral("unknown tag %1").arg(tag)));
        }

        TlvItem item;
        item.tag = tag;
        item.type = TlvType::Float;
        item.value = value.value().toFloat();
        response->items.append(std::move(item));
    }

    return MessagePtr(response);
}

Result<MessagePtr> WriteTagsHandler::handle(const WriteTagsRequest& request,
                                            ExecutionContext& context)
{
    quint16 accepted = 0;
    for (const TlvItem& item : request.items) {
        if (const auto written = context.write(tagBase_ + item.tag, item.value);
            written.hasError()) {
            return MessagePtr(ErrorResponse::make(cmd::kWriteTags, 0x03,
                                                  QStringLiteral("tag %1: %2")
                                                      .arg(item.tag)
                                                      .arg(written.error().message)));
        }
        ++accepted;
    }

    auto response = std::make_shared<WriteTagsResponse>();
    response->acceptedCount = accepted;
    return MessagePtr(response);
}

} // namespace hwsim::plugins::tlv
