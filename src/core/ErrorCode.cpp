#include "ErrorCode.h"

#include <iterator>

namespace hwsim::core {
namespace {

constexpr const char* kNames[] = {
    "None",
    "InvalidArgument",
    "OutOfRange",
    "NotFound",
    "AlreadyExists",
    "Unsupported",
    "PermissionDenied",
    "Timeout",
    "Cancelled",
    "NotReady",
    "IoError",
    "NotConnected",
    "ConnectionRefused",
    "AddressInUse",
    "FrameMalformed",
    "FrameTooLarge",
    "ChecksumMismatch",
    "ProtocolError",
    "UnknownCommand",
    "DeviceBusy",
    "DeviceFault",
    "ParameterReadOnly",
    "ConfigInvalid",
    "PluginError",
    "ScriptError",
    "Internal",
};

} // namespace

QString errorCodeName(ErrorCode code)
{
    const auto index = static_cast<std::size_t>(code);
    if (index >= std::size(kNames)) {
        return QStringLiteral("Unknown(%1)").arg(index);
    }
    return QString::fromLatin1(kNames[index]);
}

QString Error::toString() const
{
    QString result = errorCodeName(code);
    if (!message.isEmpty()) {
        result += QStringLiteral(": ") + message;
    }
    if (!context.isEmpty()) {
        result += QStringLiteral(" [") + context + QLatin1Char(']');
    }
    return result;
}

bool Error::isTransient() const noexcept
{
    return code == ErrorCode::Timeout
        || code == ErrorCode::DeviceBusy
        || code == ErrorCode::NotReady
        || code == ErrorCode::IoError;
}

Error makeError(ErrorCode code, QString message, QString context)
{
    return Error{code, std::move(message), std::move(context)};
}

} // namespace hwsim::core
