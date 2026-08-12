#include "CommandRegistry.h"

#include <core/Logger.h>

#include <algorithm>

namespace {
constexpr auto kLogCategory = "protocol.registry";

QString typeName(hwsim::core::TypeId id)
{
    return QString::fromUtf8(id.name().data(), static_cast<qsizetype>(id.name().size()));
}
} // namespace

namespace hwsim::protocol {

bool CommandRegistry::addParser(OpCode opcode, core::TypeId messageType, ParseFunction parse,
                                QString displayName)
{
    std::unique_lock lock(mutex_);
    if (parsers_.contains(opcode)) {
        HWSIM_LOG_WARNING(kLogCategory)
            << "opcode 0x" << QString::number(opcode, 16)
            << " is already bound to " << typeName(parsers_.at(opcode).messageType);
        return false;
    }
    parsers_.emplace(opcode, ParserEntry{messageType, std::move(displayName), std::move(parse)});
    return true;
}

bool CommandRegistry::addHandler(core::TypeId messageType, DispatchFunction dispatch,
                                 QString displayName)
{
    std::unique_lock lock(mutex_);
    if (handlers_.contains(messageType)) {
        HWSIM_LOG_WARNING(kLogCategory)
            << "handler for " << typeName(messageType) << " is already registered";
        return false;
    }
    handlers_.emplace(messageType, HandlerEntry{std::move(displayName), std::move(dispatch)});
    return true;
}

bool CommandRegistry::addEncoder(core::TypeId messageType, OpCode opcode, EncodeFunction encode)
{
    std::unique_lock lock(mutex_);
    encoders_.insert_or_assign(messageType, EncoderEntry{opcode, std::move(encode)});
    return true;
}

core::Result<MessagePtr> CommandRegistry::parse(const Frame& frame) const
{
    ParseFunction parse;
    {
        std::shared_lock lock(mutex_);
        const auto it = parsers_.find(frame.opcode);
        if (it == parsers_.end()) {
            return core::makeError(
                core::ErrorCode::UnknownCommand,
                QStringLiteral("no message type registered for opcode 0x%1")
                    .arg(frame.opcode, 2, 16, QLatin1Char('0')));
        }
        parse = it->second.parse;
    }
    return parse(frame);
}

core::Result<MessagePtr> CommandRegistry::dispatch(const IMessage& message,
                                                   ExecutionContext& context) const
{
    DispatchFunction dispatch;
    {
        std::shared_lock lock(mutex_);
        const auto it = handlers_.find(message.messageType());
        if (it == handlers_.end()) {
            return core::makeError(core::ErrorCode::UnknownCommand,
                                   QStringLiteral("no handler registered for %1")
                                       .arg(typeName(message.messageType())));
        }
        dispatch = it->second.dispatch;
    }
    return dispatch(message, context);
}

core::Result<QByteArray> CommandRegistry::encodeBody(const IMessage& message,
                                                     const EncodeContext& context) const
{
    EncodeFunction encode;
    {
        std::shared_lock lock(mutex_);
        const auto it = encoders_.find(message.messageType());
        if (it == encoders_.end()) {
            return core::makeError(core::ErrorCode::Unsupported,
                                   QStringLiteral("no encoder registered for %1")
                                       .arg(typeName(message.messageType())));
        }
        encode = it->second.encode;
    }
    return encode(message, context);
}

core::Result<OpCode> CommandRegistry::opcodeFor(core::TypeId messageType) const
{
    std::shared_lock lock(mutex_);
    const auto it = encoders_.find(messageType);
    if (it == encoders_.end()) {
        return core::makeError(core::ErrorCode::NotFound,
                               QStringLiteral("no opcode known for %1").arg(typeName(messageType)));
    }
    return it->second.opcode;
}

bool CommandRegistry::hasDecoder(OpCode opcode) const
{
    std::shared_lock lock(mutex_);
    return parsers_.contains(opcode);
}

bool CommandRegistry::hasHandler(core::TypeId messageType) const
{
    std::shared_lock lock(mutex_);
    return handlers_.contains(messageType);
}

bool CommandRegistry::hasEncoder(core::TypeId messageType) const
{
    std::shared_lock lock(mutex_);
    return encoders_.contains(messageType);
}

std::vector<CommandRegistry::Binding> CommandRegistry::bindings() const
{
    std::shared_lock lock(mutex_);

    std::vector<Binding> result;
    result.reserve(parsers_.size());

    for (const auto& [opcode, parser] : parsers_) {
        Binding binding;
        binding.opcode = opcode;
        binding.messageTypeName = typeName(parser.messageType);
        binding.displayName = parser.displayName;
        binding.hasDecoder = true;
        binding.hasHandler = handlers_.contains(parser.messageType);
        binding.hasEncoder = encoders_.contains(parser.messageType);
        result.push_back(std::move(binding));
    }

    std::sort(result.begin(), result.end(),
              [](const Binding& lhs, const Binding& rhs) { return lhs.opcode < rhs.opcode; });
    return result;
}

QStringList CommandRegistry::describeBindings() const
{
    QStringList lines;
    for (const Binding& binding : bindings()) {
        lines.append(QStringLiteral("0x%1  %2%3%4  %5")
                         .arg(binding.opcode, 2, 16, QLatin1Char('0'))
                         .arg(binding.hasDecoder ? QLatin1Char('D') : QLatin1Char('-'))
                         .arg(binding.hasHandler ? QLatin1Char('H') : QLatin1Char('-'))
                         .arg(binding.hasEncoder ? QLatin1Char('E') : QLatin1Char('-'))
                         .arg(binding.displayName.isEmpty() ? binding.messageTypeName
                                                            : binding.displayName));
    }
    return lines;
}

std::size_t CommandRegistry::size() const
{
    std::shared_lock lock(mutex_);
    return parsers_.size();
}

void CommandRegistry::clear()
{
    std::unique_lock lock(mutex_);
    parsers_.clear();
    handlers_.clear();
    encoders_.clear();
}

} // namespace hwsim::protocol
