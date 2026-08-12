#pragma once

#include "ScriptingGlobal.h"

#include <core/Result.h>

#include <QStringList>
#include <QVariantMap>

namespace hwsim::scripting {

/// The operations a script may perform on the running system.
///
/// Narrower than the UI controller on purpose: a scenario script should be able
/// to drive a test and assert on it, not reconfigure the workspace. Keeping the
/// surface small also keeps scripts stable across refactors.
class HWSIM_SCRIPTING_API IScriptHost {
public:
    virtual ~IScriptHost() = default;

    [[nodiscard]] virtual QStringList scriptDeviceIds() const = 0;
    [[nodiscard]] virtual bool hasDevice(const QString& deviceId) const = 0;

    [[nodiscard]] virtual core::Result<void> scriptStartDevice(const QString& deviceId) = 0;
    virtual void scriptStopDevice(const QString& deviceId) = 0;
    [[nodiscard]] virtual int scriptLinkCount(const QString& deviceId) const = 0;

    [[nodiscard]] virtual core::Result<QVariant> scriptReadParameter(const QString& deviceId,
                                                                     const QString& key) const = 0;
    [[nodiscard]] virtual core::Result<void> scriptWriteParameter(const QString& deviceId,
                                                                  const QString& key,
                                                                  const QVariant& value) = 0;

    [[nodiscard]] virtual QString scriptCurrentState(const QString& deviceId) const = 0;
    [[nodiscard]] virtual core::Result<void> scriptPostEvent(const QString& deviceId,
                                                             const QString& eventName) = 0;
    [[nodiscard]] virtual core::Result<void> scriptForceState(const QString& deviceId,
                                                              const QString& stateName) = 0;
    virtual void scriptSetOnline(const QString& deviceId, bool online) = 0;

    [[nodiscard]] virtual core::Result<QString> scriptAddFault(const QString& deviceId,
                                                               const QString& kind,
                                                               const QVariantMap& configuration) = 0;
    virtual bool scriptRemoveFault(const QString& deviceId, const QString& ruleId) = 0;
    virtual bool scriptArmFault(const QString& deviceId, const QString& ruleId) = 0;
    virtual void scriptSetFaultsEnabled(const QString& deviceId, bool enabled) = 0;

    [[nodiscard]] virtual core::Result<void> scriptSendRaw(const QString& deviceId,
                                                           const QByteArray& bytes) = 0;
};

} // namespace hwsim::scripting
