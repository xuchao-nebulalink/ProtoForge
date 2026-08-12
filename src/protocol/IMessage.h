#pragma once

#include "ProtocolTypes.h"

#include <core/TypeId.h>

#include <concepts>
#include <memory>
#include <optional>

namespace hwsim::protocol {

/// Base of every decoded protocol message, in both directions.
///
/// Requests and responses share one base on purpose: this platform is
/// symmetric. The same ReadHoldingRegisters type is produced by a simulated
/// device decoding an incoming request and by a test master building an
/// outgoing one, so splitting the hierarchy would only duplicate it.
class HWSIM_PROTOCOL_API IMessage {
public:
    virtual ~IMessage() = default;

    /// Identity used for handler and encoder lookup. Implemented by
    /// MessageBase, never by hand.
    [[nodiscard]] virtual core::TypeId messageType() const noexcept = 0;

    /// One line for the packet view and the log, e.g.
    /// "ReadHoldingRegisters addr=0x0000 count=10".
    [[nodiscard]] virtual QString describe() const = 0;

    /// Overrides the opcode this instance serialises as.
    ///
    /// Most messages map to exactly one opcode and leave this alone. It exists
    /// for protocols where the wire opcode depends on the message content: a
    /// Modbus exception echoes the original function code with the high bit
    /// set, and one Modbus message type serves both "read coils" and "read
    /// discrete inputs".
    [[nodiscard]] virtual std::optional<OpCode> dynamicOpcode() const { return std::nullopt; }

protected:
    // Declaring the virtual destructor would otherwise suppress the implicit
    // move operations, leaving derived messages movable only through a copy.
    // They are restored here, and kept protected so a derived message cannot be
    // sliced by assigning through an IMessage reference. Derived types still
    // get public copy and move, which is what DecodableMessage's std::movable
    // requirement needs.
    IMessage() = default;
    IMessage(const IMessage&) = default;
    IMessage(IMessage&&) = default;
    IMessage& operator=(const IMessage&) = default;
    IMessage& operator=(IMessage&&) = default;
};

using MessagePtr = std::shared_ptr<IMessage>;

/// CRTP base that supplies messageType(). Derive as
/// `struct ReadCoils : MessageBase<ReadCoils> { ... };`
template <typename Derived>
class MessageBase : public IMessage {
public:
    [[nodiscard]] core::TypeId messageType() const noexcept final
    {
        return core::TypeId::of<Derived>();
    }
};

// --- Concepts --------------------------------------------------------------
//
// These turn a mis-shaped message or handler into a readable compile error at
// the registration call site, instead of a template instantiation backtrace.

template <typename T>
concept MessageType = std::derived_from<T, IMessage>;

/// A message that can be produced from a received frame.
template <typename T>
concept DecodableMessage = MessageType<T> && std::movable<T> && requires(const Frame& frame) {
    { T::decode(frame) } -> std::same_as<core::Result<T>>;
};

/// A message that can be turned back into a frame body. The codec adds the
/// framing around whatever this returns.
template <typename T>
concept EncodableMessage = MessageType<T>
    && requires(const T& message, const EncodeContext& context) {
           { message.encodeBody(context) } -> std::same_as<core::Result<QByteArray>>;
           { T::opcode() } -> std::convertible_to<OpCode>;
       };

class ExecutionContext;

/// A handler for exactly one message type. Returning a null MessagePtr means
/// "no reply", which is what a broadcast or a fire-and-forget write does.
template <typename H, typename M>
concept HandlerFor = MessageType<M>
    && requires(H& handler, const M& message, ExecutionContext& context) {
           { handler.handle(message, context) } -> std::same_as<core::Result<MessagePtr>>;
       };

} // namespace hwsim::protocol
