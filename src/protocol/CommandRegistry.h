#pragma once

#include "ExecutionContext.h"
#include "IMessage.h"
#include "IUnsolicitedSource.h"

#include <core/TypeId.h>

#include <QStringList>

#include <functional>
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace hwsim::protocol {

/// The dispatch core, and the reason this codebase has no protocol switch
/// statements.
///
/// Three lookups replace what would otherwise be three parallel switches:
///
///   opcode      -> decoder     turns a received frame into a typed message
///   message type-> handler     runs the business logic for that type
///   message type-> encoder     turns an outgoing message back into a body
///
/// All three are populated by a single bind<Message>(handler) call, so adding a
/// protocol message means writing the message type and its handler, then one
/// registration line. Nothing in the framework needs to be edited, and the
/// compiler rejects a handler whose signature does not match its message via
/// the HandlerFor concept.
///
/// Registration happens while a plugin is being initialised; lookups happen on
/// the session thread afterwards. The lock is there so a plugin that registers
/// lazily cannot corrupt the tables, not because contention is expected.
class HWSIM_PROTOCOL_API CommandRegistry {
public:
    using ParseFunction = std::function<core::Result<MessagePtr>(const Frame&)>;
    using DispatchFunction = std::function<core::Result<MessagePtr>(const IMessage&, ExecutionContext&)>;
    using EncodeFunction = std::function<core::Result<QByteArray>(const IMessage&, const EncodeContext&)>;

    struct Binding {
        OpCode opcode{0};
        QString messageTypeName;
        QString displayName;
        bool hasDecoder{false};
        bool hasHandler{false};
        bool hasEncoder{false};
    };

    CommandRegistry() = default;

    /// The common case: a message that carries its own opcode, plus the handler
    /// that services it.
    ///
    ///     registry.bind<ReadHoldingRegisters>(
    ///         std::make_shared<ReadHoldingRegistersHandler>());
    template <typename M, typename H>
        requires DecodableMessage<M> && EncodableMessage<M> && HandlerFor<H, M>
    bool bind(std::shared_ptr<H> handler, QString displayName = {})
    {
        const OpCode opcode = static_cast<OpCode>(M::opcode());
        return bindAt<M, H>(opcode, std::move(handler), std::move(displayName))
            && bindEncoder<M>();
    }

    /// Same, but the opcode is supplied explicitly. Used when one message type
    /// serves several function codes that differ only in which table they read.
    template <typename M, typename H>
        requires DecodableMessage<M> && HandlerFor<H, M>
    bool bindAt(OpCode opcode, std::shared_ptr<H> handler, QString displayName = {})
    {
        if (!handler) {
            return false;
        }
        const bool decoderAdded = bindDecoder<M>(opcode, displayName);

        auto dispatch = [handler](const IMessage& message,
                                  ExecutionContext& context) -> core::Result<MessagePtr> {
            // Safe downcast: dispatch() only reaches this entry when the
            // message's TypeId matched the key it was registered under.
            return handler->handle(static_cast<const M&>(message), context);
        };
        const bool handlerAdded =
            addHandler(core::TypeId::of<M>(), std::move(dispatch), std::move(displayName));

        return decoderAdded && handlerAdded;
    }

    /// Decoder without a handler. An initiator registers these so it can parse
    /// the responses it receives; the pending-request table consumes them
    /// instead of a handler.
    template <typename M>
        requires DecodableMessage<M>
    bool bindDecoder(OpCode opcode, QString displayName = {})
    {
        auto parse = [](const Frame& frame) -> core::Result<MessagePtr> {
            auto decoded = M::decode(frame);
            if (decoded.hasError()) {
                return decoded.error();
            }
            return MessagePtr(std::make_shared<M>(std::move(decoded).value()));
        };
        return addParser(opcode, core::TypeId::of<M>(), std::move(parse), std::move(displayName));
    }

    /// Encoder for a message this side sends. bind() does this automatically;
    /// call it directly for messages that are only ever transmitted, such as an
    /// exception response or an unsolicited event report.
    template <typename M>
        requires EncodableMessage<M>
    bool bindEncoder()
    {
        auto encode = [](const IMessage& message,
                         const EncodeContext& context) -> core::Result<QByteArray> {
            return static_cast<const M&>(message).encodeBody(context);
        };
        return addEncoder(core::TypeId::of<M>(), static_cast<OpCode>(M::opcode()), std::move(encode));
    }

    /// Handler for a message that never appears on the wire in this direction,
    /// e.g. one synthesised internally by a state machine.
    template <typename M, typename H>
        requires HandlerFor<H, M>
    bool bindHandlerOnly(std::shared_ptr<H> handler, QString displayName = {})
    {
        if (!handler) {
            return false;
        }
        auto dispatch = [handler](const IMessage& message,
                                  ExecutionContext& context) -> core::Result<MessagePtr> {
            return handler->handle(static_cast<const M&>(message), context);
        };
        return addHandler(core::TypeId::of<M>(), std::move(dispatch), std::move(displayName));
    }

    /// Frame -> typed message.
    [[nodiscard]] core::Result<MessagePtr> parse(const Frame& frame) const;

    /// Typed message -> handler -> optional reply. A null reply means the
    /// handler decided not to answer.
    [[nodiscard]] core::Result<MessagePtr> dispatch(const IMessage& message,
                                                    ExecutionContext& context) const;

    /// Typed message -> frame body. The codec wraps this with header and checksum.
    [[nodiscard]] core::Result<QByteArray> encodeBody(const IMessage& message,
                                                      const EncodeContext& context) const;

    /// Opcode a message serialises as, needed by the codec when framing a reply.
    [[nodiscard]] core::Result<OpCode> opcodeFor(core::TypeId messageType) const;

    [[nodiscard]] bool hasDecoder(OpCode opcode) const;
    [[nodiscard]] bool hasHandler(core::TypeId messageType) const;
    [[nodiscard]] bool hasEncoder(core::TypeId messageType) const;

    /// Installs the source of unsolicited traffic for this protocol, if it has
    /// any. Set while registering commands, because that is where the plugin
    /// holds the device state the source has to read; every session built on
    /// this registry picks it up and polls it.
    void setUnsolicitedSource(UnsolicitedSourcePtr source);
    [[nodiscard]] UnsolicitedSourcePtr unsolicitedSource() const;

    [[nodiscard]] std::vector<Binding> bindings() const;
    [[nodiscard]] QStringList describeBindings() const;
    [[nodiscard]] std::size_t size() const;

    void clear();

private:
    struct ParserEntry {
        core::TypeId messageType;
        QString displayName;
        ParseFunction parse;
    };

    struct HandlerEntry {
        QString displayName;
        DispatchFunction dispatch;
    };

    struct EncoderEntry {
        OpCode opcode{0};
        EncodeFunction encode;
    };

    bool addParser(OpCode opcode, core::TypeId messageType, ParseFunction parse, QString displayName);
    bool addHandler(core::TypeId messageType, DispatchFunction dispatch, QString displayName);
    bool addEncoder(core::TypeId messageType, OpCode opcode, EncodeFunction encode);

    mutable std::shared_mutex mutex_;
    std::unordered_map<OpCode, ParserEntry> parsers_;
    std::unordered_map<core::TypeId, HandlerEntry> handlers_;
    std::unordered_map<core::TypeId, EncoderEntry> encoders_;
    UnsolicitedSourcePtr unsolicited_;
};

} // namespace hwsim::protocol
