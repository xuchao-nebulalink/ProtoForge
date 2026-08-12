#pragma once

#include "IDeviceAccess.h"
#include "ProtocolTypes.h"

#include <core/EventBus.h>

namespace hwsim::protocol {

/// Everything a handler is given besides the message itself.
///
/// Handlers are plain objects with a single handle() method; all their
/// collaborators arrive through here rather than through constructor
/// injection, which keeps handler registration to one line and keeps handlers
/// trivially unit-testable against a stub IDeviceAccess.
class HWSIM_PROTOCOL_API ExecutionContext {
public:
    ExecutionContext(IDeviceAccess* device, const Frame& frame);

    [[nodiscard]] IDeviceAccess* device() const noexcept { return device_; }

    /// Handlers that cannot work without a device use this instead of a null
    /// check, so the failure turns into a protocol error rather than a crash.
    [[nodiscard]] core::Result<IDeviceAccess*> requireDevice() const;

    [[nodiscard]] const Frame& frame() const noexcept { return *frame_; }
    [[nodiscard]] transport::LinkId linkId() const noexcept { return frame_->linkId; }
    [[nodiscard]] qint64 receivedAtMs() const noexcept { return frame_->timestampMs; }

    [[nodiscard]] QString sessionName() const { return sessionName_; }
    void setSessionName(QString name) { sessionName_ = std::move(name); }

    [[nodiscard]] core::EventBus* eventBus() const noexcept { return eventBus_; }
    void setEventBus(core::EventBus* bus) noexcept { eventBus_ = bus; }

    /// Free-form storage shared between middleware and the handler for the
    /// duration of one request, e.g. a timing mark set by a stats middleware.
    [[nodiscard]] QVariantMap& scratch() noexcept { return scratch_; }
    [[nodiscard]] const QVariantMap& scratch() const noexcept { return scratch_; }

    /// Thin pass-throughs so handler bodies read as device operations.
    [[nodiscard]] core::Result<QVariant> read(quint32 address) const;
    [[nodiscard]] core::Result<void> write(quint32 address, const QVariant& value) const;
    [[nodiscard]] core::Result<QVector<QVariant>> readRange(quint32 startAddress, quint32 count) const;
    [[nodiscard]] core::Result<void> writeRange(quint32 startAddress, const QVector<QVariant>& values) const;

private:
    IDeviceAccess* device_{nullptr};
    const Frame* frame_{nullptr};
    core::EventBus* eventBus_{nullptr};
    QString sessionName_;
    QVariantMap scratch_;
};

} // namespace hwsim::protocol
