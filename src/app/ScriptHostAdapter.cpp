#include "ScriptHostAdapter.h"

using hwsim::core::ErrorCode;
using hwsim::core::makeError;
using hwsim::core::Result;

namespace hwsim::app {

QStringList ScriptHostAdapter::scriptDeviceIds() const
{
    return workspace_->deviceIds();
}

bool ScriptHostAdapter::hasDevice(const QString& deviceId) const
{
    return workspace_->device(deviceId) != nullptr;
}

Result<void> ScriptHostAdapter::scriptStartDevice(const QString& deviceId)
{
    return workspace_->startDevice(deviceId);
}

void ScriptHostAdapter::scriptStopDevice(const QString& deviceId)
{
    workspace_->stopDevice(deviceId);
}

int ScriptHostAdapter::scriptLinkCount(const QString& deviceId) const
{
    DeviceRuntime* runtime = workspace_->device(deviceId);
    return runtime == nullptr ? 0 : runtime->linkCount();
}

Result<QVariant> ScriptHostAdapter::scriptReadParameter(const QString& deviceId,
                                                        const QString& key) const
{
    const QVariantMap values = workspace_->parameterValues(deviceId);
    if (!values.contains(key)) {
        return makeError(ErrorCode::NotFound,
                         QStringLiteral("device '%1' has no parameter '%2'").arg(deviceId, key));
    }
    return values.value(key);
}

Result<void> ScriptHostAdapter::scriptWriteParameter(const QString& deviceId, const QString& key,
                                                     const QVariant& value)
{
    return workspace_->setParameter(deviceId, key, value);
}

QString ScriptHostAdapter::scriptCurrentState(const QString& deviceId) const
{
    return workspace_->currentState(deviceId);
}

Result<void> ScriptHostAdapter::scriptPostEvent(const QString& deviceId, const QString& eventName)
{
    return workspace_->postEvent(deviceId, eventName);
}

Result<void> ScriptHostAdapter::scriptForceState(const QString& deviceId, const QString& stateName)
{
    return workspace_->forceState(deviceId, stateName);
}

void ScriptHostAdapter::scriptSetOnline(const QString& deviceId, bool online)
{
    workspace_->setDeviceOnline(deviceId, online);
}

Result<QString> ScriptHostAdapter::scriptAddFault(const QString& deviceId, const QString& kind,
                                                  const QVariantMap& configuration)
{
    return workspace_->addFaultRule(deviceId, kind, configuration);
}

bool ScriptHostAdapter::scriptRemoveFault(const QString& deviceId, const QString& ruleId)
{
    return workspace_->removeFaultRule(deviceId, ruleId);
}

bool ScriptHostAdapter::scriptArmFault(const QString& deviceId, const QString& ruleId)
{
    return workspace_->armFaultRule(deviceId, ruleId);
}

void ScriptHostAdapter::scriptSetFaultsEnabled(const QString& deviceId, bool enabled)
{
    workspace_->setFaultInjectionEnabled(deviceId, enabled);
}

Result<void> ScriptHostAdapter::scriptSendRaw(const QString& deviceId, const QByteArray& bytes)
{
    return workspace_->sendRaw(deviceId, bytes);
}

} // namespace hwsim::app
