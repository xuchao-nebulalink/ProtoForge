#pragma once

#include "ProtocolTypes.h"

#include <core/Clock.h>
#include <core/ErrorCode.h>

namespace hwsim::protocol {

/// A frame that completed a full trip through the pipeline, in either
/// direction. This is the packet view's data source: it carries the raw bytes
/// for the hex tab and the decoded description for the parse tab, so the view
/// needs no protocol knowledge of its own.
struct ProtocolFrameEvent {
    QString sessionName;
    QString deviceName;
    transport::LinkId linkId{transport::kInvalidLinkId};
    transport::Direction direction{transport::Direction::Inbound};

    OpCode opcode{0};
    QByteArray raw;

    /// IMessage::describe() output, or empty when decoding failed.
    QString messageDescription;
    bool decoded{false};

    /// Fault injection notes and decode diagnostics, shown as a row annotation.
    QString annotation;

    qint64 timestampMs{core::wallClockMs()};
};

/// A frame that could not be processed: unknown opcode, bad checksum, decode
/// failure, or a handler that returned an error.
struct ProtocolErrorEvent {
    QString sessionName;
    transport::LinkId linkId{transport::kInvalidLinkId};
    core::Error error;
    QByteArray raw;
    qint64 timestampMs{core::wallClockMs()};
};

/// Bytes discarded by the codec while resynchronising, usually line noise.
struct ProtocolResyncEvent {
    QString sessionName;
    transport::LinkId linkId{transport::kInvalidLinkId};
    QByteArray discarded;
    QString reason;
    qint64 timestampMs{core::wallClockMs()};
};

/// An outstanding request in initiator mode that never got an answer.
struct RequestTimedOutEvent {
    QString sessionName;
    transport::LinkId linkId{transport::kInvalidLinkId};
    QString requestDescription;
    int timeoutMs{0};
    qint64 timestampMs{core::wallClockMs()};
};

} // namespace hwsim::protocol
