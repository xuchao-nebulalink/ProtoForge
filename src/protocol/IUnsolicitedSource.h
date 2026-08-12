#pragma once

#include "IMessage.h"

#include <QString>

#include <memory>
#include <vector>

namespace hwsim::protocol {

/// Traffic a session emits without having been asked: a telemetry stream, a
/// keep-alive, an event report.
///
/// A handler cannot express this, because a handler only ever runs in reply to
/// a received frame. The session polls this on a timer instead and pushes what
/// it returns through send(), which registers no correlation entry - exactly
/// the semantics an unsolicited report needs.
///
/// A plugin installs one on its CommandRegistry while registering commands,
/// which is the point where it still holds the device state its handlers share.
/// The framework stays protocol-agnostic: it knows only "poll and send".
class HWSIM_PROTOCOL_API IUnsolicitedSource {
public:
    virtual ~IUnsolicitedSource() = default;

    [[nodiscard]] virtual QString name() const = 0;

    /// Poll period in milliseconds; zero means the session never polls.
    [[nodiscard]] virtual int intervalMs() const = 0;

    /// Messages to push now. Returning none is normal: a source with nothing
    /// to report simply stays quiet this tick.
    [[nodiscard]] virtual std::vector<MessagePtr> poll(qint64 nowMs) = 0;
};

using UnsolicitedSourcePtr = std::shared_ptr<IUnsolicitedSource>;

} // namespace hwsim::protocol
