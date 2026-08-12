#pragma once

#include "CoreGlobal.h"

#include <QString>

namespace hwsim::core {

/// Failure categories shared by every layer. Protocol plugins map their own
/// exception codes onto these before they reach the framework, so the UI and
/// the scripting layer only ever deal with this closed set.
enum class ErrorCode {
    None = 0,

    // Caller mistakes
    InvalidArgument,
    OutOfRange,
    NotFound,
    AlreadyExists,
    Unsupported,
    PermissionDenied,

    // Timing and lifecycle
    Timeout,
    Cancelled,
    NotReady,

    // Transport
    IoError,
    NotConnected,
    ConnectionRefused,
    AddressInUse,

    // Framing and protocol
    FrameMalformed,
    FrameTooLarge,
    ChecksumMismatch,
    ProtocolError,
    UnknownCommand,

    // Device model
    DeviceBusy,
    DeviceFault,
    ParameterReadOnly,

    // Infrastructure
    ConfigInvalid,
    PluginError,
    ScriptError,
    Internal,
};

[[nodiscard]] HWSIM_CORE_API QString errorCodeName(ErrorCode code);

/// A failure value. Deliberately not an exception: errors cross thread and DLL
/// boundaries here, and most of them are expected outcomes of a simulation
/// (a timeout fault, a deliberately corrupted CRC) rather than bugs.
struct HWSIM_CORE_API Error {
    ErrorCode code{ErrorCode::Internal};
    QString message;
    QString context;

    [[nodiscard]] QString toString() const;
    [[nodiscard]] bool isTransient() const noexcept;
};

[[nodiscard]] HWSIM_CORE_API Error makeError(ErrorCode code, QString message, QString context = {});

} // namespace hwsim::core
