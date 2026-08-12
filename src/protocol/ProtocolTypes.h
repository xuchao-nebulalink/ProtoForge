#pragma once

#include "ProtocolGlobal.h"

#include <core/Clock.h>
#include <core/Result.h>
#include <transport/TransportEvents.h>
#include <transport/TransportTypes.h>

#include <QByteArray>
#include <QString>
#include <QVariantMap>

#include <cstddef>

namespace hwsim::protocol {

/// Wire identifier for a message kind: a Modbus function code, a CAN message
/// id, a command byte in a private protocol. Wide enough that no plugin has to
/// pack its identifier into something smaller.
using OpCode = quint32;

/// One complete protocol data unit as it appeared on the wire.
struct HWSIM_PROTOCOL_API Frame {
    /// Complete bytes including header and checksum, kept for the packet view.
    QByteArray raw;

    OpCode opcode{0};

    /// Body without framing overhead. This is what a message type decodes.
    QByteArray payload;

    /// Codec-specific extras that the response needs to echo, such as a Modbus
    /// unit id or transaction id. Keeping them here means the message types
    /// stay free of framing concerns.
    QVariantMap attributes;

    transport::LinkId linkId{transport::kInvalidLinkId};
    transport::Direction direction{transport::Direction::Inbound};
    qint64 timestampMs{0};

    [[nodiscard]] QVariant attribute(const QString& key, const QVariant& fallback = {}) const
    {
        return attributes.value(key, fallback);
    }
};

enum class FrameScanStatus {
    /// Not enough bytes yet; wait for more and rescan.
    NeedMoreData,
    /// A complete frame was found.
    FrameReady,
    /// The leading bytes cannot start a valid frame and must be dropped. This
    /// is how a codec resynchronises after line noise.
    Discard,
};

/// Outcome of looking at a receive buffer. Separating "need more data" from a
/// genuine error is what makes stream reassembly work without treating a
/// half-arrived frame as a protocol violation.
struct HWSIM_PROTOCOL_API FrameScanResult {
    FrameScanStatus status{FrameScanStatus::NeedMoreData};
    std::size_t consumed{0};
    Frame frame;
    QString diagnostic;

    [[nodiscard]] static FrameScanResult needMoreData()
    {
        return FrameScanResult{FrameScanStatus::NeedMoreData, 0, {}, {}};
    }

    [[nodiscard]] static FrameScanResult ready(Frame frame, std::size_t consumed)
    {
        return FrameScanResult{FrameScanStatus::FrameReady, consumed, std::move(frame), {}};
    }

    [[nodiscard]] static FrameScanResult discard(std::size_t consumed, QString reason)
    {
        return FrameScanResult{FrameScanStatus::Discard, consumed, {}, std::move(reason)};
    }
};

/// Everything a message needs in order to serialise itself back onto the wire.
struct HWSIM_PROTOCOL_API EncodeContext {
    /// The frame being answered. Null for an unsolicited message or for a
    /// request built by an initiator.
    const Frame* request{nullptr};

    /// Values the codec needs for framing, seeded from the request when there
    /// is one so that unit ids and transaction ids round-trip automatically.
    QVariantMap attributes;

    transport::LinkId linkId{transport::kInvalidLinkId};

    [[nodiscard]] QVariant attribute(const QString& key, const QVariant& fallback = {}) const
    {
        return attributes.value(key, fallback);
    }

    [[nodiscard]] static EncodeContext forReply(const Frame& requestFrame)
    {
        EncodeContext context;
        context.request = &requestFrame;
        context.attributes = requestFrame.attributes;
        context.linkId = requestFrame.linkId;
        return context;
    }
};

} // namespace hwsim::protocol
