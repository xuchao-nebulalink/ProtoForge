#include "DeviceProfile.h"

#include <core/Logger.h>

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>

namespace {
constexpr auto kLogCategory = "simulator.profile";
}

using hwsim::core::ErrorCode;
using hwsim::core::makeError;
using hwsim::core::Result;

namespace hwsim::simulator {

QJsonObject DeviceProfile::toJson() const
{
    QJsonObject json;
    json.insert(QStringLiteral("formatVersion"), kFormatVersion);
    json.insert(QStringLiteral("name"), name);
    if (!description.isEmpty()) {
        json.insert(QStringLiteral("description"), description);
    }
    json.insert(QStringLiteral("role"), role);

    QJsonObject protocol;
    protocol.insert(QStringLiteral("id"), protocolId);
    protocol.insert(QStringLiteral("config"), QJsonObject::fromVariantMap(protocolConfig));
    json.insert(QStringLiteral("protocol"), protocol);

    json.insert(QStringLiteral("transport"), transportConfig);
    json.insert(QStringLiteral("parameters"), parameters);

    if (!stateMachine.isEmpty()) {
        json.insert(QStringLiteral("stateMachine"), stateMachine);
    }
    if (!signalBindings.isEmpty()) {
        json.insert(QStringLiteral("signals"), signalBindings);
    }
    if (!faultRules.isEmpty()) {
        json.insert(QStringLiteral("faults"), faultRules);
    }
    if (!initialValues.isEmpty()) {
        json.insert(QStringLiteral("initialValues"), QJsonObject::fromVariantMap(initialValues));
    }

    return json;
}

Result<DeviceProfile> DeviceProfile::fromJson(const QJsonObject& json)
{
    const int version = json.value(QStringLiteral("formatVersion")).toInt(kFormatVersion);
    if (version > kFormatVersion) {
        return makeError(ErrorCode::ConfigInvalid,
                         QStringLiteral("profile format version %1 is newer than the supported %2")
                             .arg(version)
                             .arg(kFormatVersion));
    }

    DeviceProfile profile;
    profile.name = json.value(QStringLiteral("name")).toString();
    if (profile.name.isEmpty()) {
        return makeError(ErrorCode::ConfigInvalid, QStringLiteral("profile is missing 'name'"));
    }

    profile.description = json.value(QStringLiteral("description")).toString();
    profile.role = json.value(QStringLiteral("role")).toString(QStringLiteral("responder"));

    const QJsonObject protocol = json.value(QStringLiteral("protocol")).toObject();
    profile.protocolId = protocol.value(QStringLiteral("id")).toString();
    profile.protocolConfig = protocol.value(QStringLiteral("config")).toObject().toVariantMap();

    profile.transportConfig = json.value(QStringLiteral("transport")).toObject();
    profile.parameters = json.value(QStringLiteral("parameters")).toArray();
    profile.stateMachine = json.value(QStringLiteral("stateMachine")).toObject();
    profile.signalBindings = json.value(QStringLiteral("signals")).toArray();
    profile.faultRules = json.value(QStringLiteral("faults")).toArray();
    profile.initialValues = json.value(QStringLiteral("initialValues")).toObject().toVariantMap();

    if (const auto valid = profile.validate(); valid.hasError()) {
        return valid.error();
    }
    return profile;
}

Result<DeviceProfile> DeviceProfile::load(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return makeError(ErrorCode::IoError, file.errorString(), path);
    }

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        return makeError(ErrorCode::ConfigInvalid,
                         QStringLiteral("%1 at offset %2")
                             .arg(parseError.errorString())
                             .arg(parseError.offset),
                         path);
    }
    if (!document.isObject()) {
        return makeError(ErrorCode::ConfigInvalid,
                         QStringLiteral("profile root must be a JSON object"), path);
    }

    auto profile = fromJson(document.object());
    if (profile.hasError()) {
        return makeError(profile.error().code, profile.error().message, path);
    }

    HWSIM_LOG_INFO(kLogCategory) << "loaded device profile '" << profile.value().name
                                 << "' from " << path;
    return profile;
}

Result<void> DeviceProfile::save(const QString& path) const
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return makeError(ErrorCode::IoError, file.errorString(), path);
    }

    const QJsonDocument document(toJson());
    if (file.write(document.toJson(QJsonDocument::Indented)) < 0) {
        return makeError(ErrorCode::IoError, file.errorString(), path);
    }

    HWSIM_LOG_INFO(kLogCategory) << "saved device profile to " << path;
    return core::success();
}

Result<void> DeviceProfile::validate() const
{
    if (name.isEmpty()) {
        return makeError(ErrorCode::ConfigInvalid, QStringLiteral("profile name must not be empty"));
    }
    if (role != QStringLiteral("responder") && role != QStringLiteral("initiator")) {
        return makeError(ErrorCode::ConfigInvalid,
                         QStringLiteral("role must be 'responder' or 'initiator', got '%1'").arg(role));
    }
    return core::success();
}

Result<void> DeviceProfile::applyTo(DeviceModel& device) const
{
    device.setName(name);

    device.parameters().clear();
    if (const auto loaded = device.parameters().loadDefinitions(parameters); loaded.hasError()) {
        return loaded;
    }

    if (!stateMachine.isEmpty()) {
        if (const auto loaded = device.stateMachine().loadJson(stateMachine); loaded.hasError()) {
            return loaded;
        }
    } else {
        device.stateMachine().clear();
    }

    if (const auto loaded = device.signalEngine().loadJson(signalBindings); loaded.hasError()) {
        return loaded;
    }

    if (const auto loaded = device.faults().loadJson(faultRules); loaded.hasError()) {
        return loaded;
    }

    // Applied last so that a captured snapshot overrides the declared defaults.
    if (const auto restored = device.parameters().restore(initialValues); restored.hasError()) {
        return restored;
    }

    return core::success();
}

DeviceProfile DeviceProfile::captureFrom(DeviceModel& device)
{
    DeviceProfile profile;
    profile.name = device.deviceName();
    profile.parameters = device.parameters().definitionsToJson();
    profile.stateMachine = device.stateMachine().toJson();
    profile.signalBindings = device.signalEngine().toJson();
    profile.faultRules = device.faults().toJson();

    // Only persist parameters that asked to be persisted.
    const QVariantMap current = device.parameters().snapshot();
    for (const ParameterDefinition& definition : device.parameters().definitions()) {
        if (definition.persistent && current.contains(definition.key)) {
            profile.initialValues.insert(definition.key, current.value(definition.key));
        }
    }

    return profile;
}

} // namespace hwsim::simulator
