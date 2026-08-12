#pragma once

// -----------------------------------------------------------------------------
// Scaffold for a private protocol. Copy this directory, rename, and replace the
// wire format with your own; the surrounding structure is what every plugin
// needs and is deliberately kept minimal.
//
// The example format is a TLV protocol that exercises every framework feature a
// real protocol tends to need:
//
//   frame:   A5 | ver(1) | seq(2) | cmd(2) | len(2) | payload(len) | crc16-ccitt(2)
//   payload: sequence of TLV items, each  tag(2) | type(1) | len(1) | value(len)
//
//   - a start byte, so resynchronisation after noise is possible
//   - an explicit length, so framing does not depend on the command
//   - a checksum, so corruption is detectable
//   - a sequence number, so responses can be correlated (initiator mode)
//   - request/response command codes that differ by a high bit
// -----------------------------------------------------------------------------

#include <core/ConfigSchema.h>
#include <core/Result.h>
#include <protocol/ExecutionContext.h>
#include <protocol/IFrameCodec.h>
#include <protocol/IMessage.h>

#include <QVariantMap>
#include <QVector>

namespace hwsim::plugins::tlv {

using hwsim::core::Result;
using hwsim::protocol::EncodeContext;
using hwsim::protocol::ExecutionContext;
using hwsim::protocol::Frame;
using hwsim::protocol::FrameScanResult;
using hwsim::protocol::IFrameCodec;
using hwsim::protocol::MessageBase;
using hwsim::protocol::MessagePtr;
using hwsim::protocol::OpCode;

inline constexpr quint8 kStartByte = 0xA5;
inline constexpr quint8 kProtocolVersion = 0x01;

/// start + version + sequence(2) + command(2) + length(2)
inline constexpr std::size_t kHeaderSize = 8;
inline constexpr std::size_t kChecksumSize = 2;

/// Frame attribute carrying the sequence number between codec and session.
inline constexpr auto kSequenceAttribute = "tlv.sequence";

/// Command codes. Responses are the request code with bit 15 set, which is a
/// common convention and keeps the two decodable in one registry.
namespace cmd {
inline constexpr quint16 kResponseFlag = 0x8000;

inline constexpr quint16 kHeartbeat = 0x0001;
inline constexpr quint16 kReadTags = 0x0010;
inline constexpr quint16 kWriteTags = 0x0011;
inline constexpr quint16 kError = 0x00FF;

[[nodiscard]] constexpr quint16 responseOf(quint16 request) noexcept
{
    return static_cast<quint16>(request | kResponseFlag);
}
} // namespace cmd

enum class TlvType : quint8 {
    Bool = 0x01,
    UInt16 = 0x02,
    UInt32 = 0x03,
    Float = 0x04,
    String = 0x05,
};

/// One tag/value pair.
struct TlvItem {
    quint16 tag{0};
    TlvType type{TlvType::UInt16};
    QVariant value;

    [[nodiscard]] QByteArray encode() const;
    [[nodiscard]] static Result<TlvItem> decode(const QByteArray& payload, qsizetype& offset);
};

[[nodiscard]] Result<QVector<TlvItem>> decodeItems(const QByteArray& payload);
[[nodiscard]] QByteArray encodeItems(const QVector<TlvItem>& items);

// --- Messages --------------------------------------------------------------

struct HeartbeatRequest : MessageBase<HeartbeatRequest> {
    quint32 uptimeSeconds{0};

    [[nodiscard]] static constexpr OpCode opcode() { return cmd::kHeartbeat; }

    [[nodiscard]] static Result<HeartbeatRequest> decode(const Frame& frame);
    [[nodiscard]] Result<QByteArray> encodeBody(const EncodeContext& context) const;
    [[nodiscard]] QString describe() const override;
};

struct HeartbeatResponse : MessageBase<HeartbeatResponse> {
    quint32 uptimeSeconds{0};
    QString state;

    [[nodiscard]] static constexpr OpCode opcode() { return cmd::responseOf(cmd::kHeartbeat); }

    [[nodiscard]] static Result<HeartbeatResponse> decode(const Frame& frame);
    [[nodiscard]] Result<QByteArray> encodeBody(const EncodeContext& context) const;
    [[nodiscard]] QString describe() const override;
};

struct ReadTagsRequest : MessageBase<ReadTagsRequest> {
    QVector<quint16> tags;

    [[nodiscard]] static constexpr OpCode opcode() { return cmd::kReadTags; }

    [[nodiscard]] static Result<ReadTagsRequest> decode(const Frame& frame);
    [[nodiscard]] Result<QByteArray> encodeBody(const EncodeContext& context) const;
    [[nodiscard]] QString describe() const override;
};

struct ReadTagsResponse : MessageBase<ReadTagsResponse> {
    QVector<TlvItem> items;

    [[nodiscard]] static constexpr OpCode opcode() { return cmd::responseOf(cmd::kReadTags); }

    [[nodiscard]] static Result<ReadTagsResponse> decode(const Frame& frame);
    [[nodiscard]] Result<QByteArray> encodeBody(const EncodeContext& context) const;
    [[nodiscard]] QString describe() const override;
};

struct WriteTagsRequest : MessageBase<WriteTagsRequest> {
    QVector<TlvItem> items;

    [[nodiscard]] static constexpr OpCode opcode() { return cmd::kWriteTags; }

    [[nodiscard]] static Result<WriteTagsRequest> decode(const Frame& frame);
    [[nodiscard]] Result<QByteArray> encodeBody(const EncodeContext& context) const;
    [[nodiscard]] QString describe() const override;
};

struct WriteTagsResponse : MessageBase<WriteTagsResponse> {
    quint16 acceptedCount{0};

    [[nodiscard]] static constexpr OpCode opcode() { return cmd::responseOf(cmd::kWriteTags); }

    [[nodiscard]] static Result<WriteTagsResponse> decode(const Frame& frame);
    [[nodiscard]] Result<QByteArray> encodeBody(const EncodeContext& context) const;
    [[nodiscard]] QString describe() const override;
};

struct ErrorResponse : MessageBase<ErrorResponse> {
    quint16 originalCommand{0};
    quint8 code{0};
    QString detail;

    [[nodiscard]] static constexpr OpCode opcode() { return cmd::responseOf(cmd::kError); }

    [[nodiscard]] static Result<ErrorResponse> decode(const Frame& frame);
    [[nodiscard]] Result<QByteArray> encodeBody(const EncodeContext& context) const;
    [[nodiscard]] QString describe() const override;

    [[nodiscard]] static std::shared_ptr<ErrorResponse> make(quint16 command, quint8 errorCode,
                                                             QString detail);
};

// --- Codec -----------------------------------------------------------------

class TlvCodec final : public IFrameCodec {
public:
    [[nodiscard]] QString name() const override { return QStringLiteral("tlv"); }

    [[nodiscard]] FrameScanResult scan(std::span<const std::byte> buffer,
                                       transport::Direction direction) const override;
    [[nodiscard]] Result<QByteArray> wrap(OpCode opcode, const QByteArray& body,
                                          const EncodeContext& context) const override;

    [[nodiscard]] QString correlationKey(const Frame& frame) const override;
    [[nodiscard]] QString prepareRequest(EncodeContext& context) const override;

    [[nodiscard]] core::ConfigSchema configSchema() const override;
    [[nodiscard]] Result<void> configure(const QVariantMap& config) override;

private:
    quint32 maxPayloadBytes_{4096};
    mutable quint16 nextSequence_{1};
};

// --- Handlers --------------------------------------------------------------

class HeartbeatHandler {
public:
    [[nodiscard]] Result<MessagePtr> handle(const HeartbeatRequest& request,
                                            ExecutionContext& context);
};

class ReadTagsHandler {
public:
    explicit ReadTagsHandler(quint32 tagBaseAddress) : tagBase_(tagBaseAddress) {}
    [[nodiscard]] Result<MessagePtr> handle(const ReadTagsRequest& request,
                                            ExecutionContext& context);

private:
    quint32 tagBase_;
};

class WriteTagsHandler {
public:
    explicit WriteTagsHandler(quint32 tagBaseAddress) : tagBase_(tagBaseAddress) {}
    [[nodiscard]] Result<MessagePtr> handle(const WriteTagsRequest& request,
                                            ExecutionContext& context);

private:
    quint32 tagBase_;
};

} // namespace hwsim::plugins::tlv
