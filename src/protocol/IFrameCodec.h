#pragma once

#include "ProtocolTypes.h"

#include <core/ConfigSchema.h>

#include <memory>
#include <span>

namespace hwsim::protocol {

/// Framing: where a message starts, where it ends, and how it is checksummed.
///
/// Kept separate from the message types on purpose. Modbus RTU and Modbus TCP
/// share every function code but frame them completely differently, so the same
/// message types and handlers are reused with a different codec. A codec knows
/// nothing about what the messages mean.
class HWSIM_PROTOCOL_API IFrameCodec {
public:
    virtual ~IFrameCodec() = default;

    [[nodiscard]] virtual QString name() const = 0;

    /// Looks for one complete frame at the front of `buffer` without consuming
    /// anything. The session acts on the returned status:
    ///
    ///   NeedMoreData - wait for the next read and scan again
    ///   FrameReady   - consume `consumed` bytes and dispatch `frame`
    ///   Discard      - consume `consumed` bytes and resynchronise
    ///
    /// Returning Discard rather than an error is what lets a codec recover from
    /// line noise on a serial link instead of wedging the session.
    [[nodiscard]] virtual FrameScanResult scan(std::span<const std::byte> buffer,
                                               transport::Direction direction) const = 0;

    /// Adds header, addressing and checksum around an encoded message body.
    [[nodiscard]] virtual core::Result<QByteArray> wrap(OpCode opcode, const QByteArray& body,
                                                        const EncodeContext& context) const = 0;

    /// Correlates a response with its request in initiator mode. Return a
    /// stable key (a Modbus transaction id, a sequence number) when the
    /// protocol has one; return an empty string to fall back to matching the
    /// oldest outstanding request, which is correct for strictly serial
    /// protocols such as Modbus RTU.
    [[nodiscard]] virtual QString correlationKey(const Frame& frame) const
    {
        Q_UNUSED(frame)
        return {};
    }

    /// Allocates the correlation token for a new outgoing request, writing it
    /// into `context.attributes` so wrap() can put it on the wire, and
    /// returning the key the eventual response will produce. Codecs without
    /// correlation leave this alone and get oldest-first matching.
    [[nodiscard]] virtual QString prepareRequest(EncodeContext& context) const
    {
        Q_UNUSED(context)
        return {};
    }

    /// True when this codec must not have more than one request in flight.
    [[nodiscard]] virtual bool isHalfDuplex() const { return false; }

    [[nodiscard]] virtual core::ConfigSchema configSchema() const { return {}; }

    [[nodiscard]] virtual core::Result<void> configure(const QVariantMap& values)
    {
        Q_UNUSED(values)
        return core::success();
    }
};

using FrameCodecPtr = std::unique_ptr<IFrameCodec>;

} // namespace hwsim::protocol
