#pragma once

#include "Workspace.h"

#include <scripting/IScriptHost.h>

namespace hwsim::app {

/// Exposes a Workspace to the scripting engine.
///
/// A separate adapter rather than another base class on Workspace: the script
/// surface is deliberately narrower than the UI surface, and keeping the two
/// apart means widening one does not silently widen the other.
class ScriptHostAdapter final : public scripting::IScriptHost {
public:
    explicit ScriptHostAdapter(Workspace& workspace) : workspace_(&workspace) {}

    [[nodiscard]] QStringList scriptDeviceIds() const override;
    [[nodiscard]] bool hasDevice(const QString& deviceId) const override;

    [[nodiscard]] core::Result<void> scriptStartDevice(const QString& deviceId) override;
    void scriptStopDevice(const QString& deviceId) override;
    [[nodiscard]] int scriptLinkCount(const QString& deviceId) const override;

    [[nodiscard]] core::Result<QVariant> scriptReadParameter(const QString& deviceId,
                                                             const QString& key) const override;
    [[nodiscard]] core::Result<void> scriptWriteParameter(const QString& deviceId,
                                                          const QString& key,
                                                          const QVariant& value) override;

    [[nodiscard]] QString scriptCurrentState(const QString& deviceId) const override;
    [[nodiscard]] core::Result<void> scriptPostEvent(const QString& deviceId,
                                                     const QString& eventName) override;
    [[nodiscard]] core::Result<void> scriptForceState(const QString& deviceId,
                                                      const QString& stateName) override;
    void scriptSetOnline(const QString& deviceId, bool online) override;

    [[nodiscard]] core::Result<QString> scriptAddFault(const QString& deviceId, const QString& kind,
                                                       const QVariantMap& configuration) override;
    bool scriptRemoveFault(const QString& deviceId, const QString& ruleId) override;
    bool scriptArmFault(const QString& deviceId, const QString& ruleId) override;
    void scriptSetFaultsEnabled(const QString& deviceId, bool enabled) override;

    [[nodiscard]] core::Result<void> scriptSendRaw(const QString& deviceId,
                                                   const QByteArray& bytes) override;

private:
    Workspace* workspace_{nullptr};
};

} // namespace hwsim::app
