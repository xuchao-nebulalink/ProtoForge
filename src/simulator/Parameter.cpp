#include "Parameter.h"

#include <core/Clock.h>

#include <QHash>
#include <QJsonValue>

using hwsim::core::ErrorCode;
using hwsim::core::makeError;
using hwsim::core::Result;

namespace hwsim::simulator {
namespace {

const QHash<ParameterType, QString>& typeNames()
{
    static const QHash<ParameterType, QString> names{
        {ParameterType::Bool, QStringLiteral("bool")},
        {ParameterType::Int, QStringLiteral("int")},
        {ParameterType::UInt, QStringLiteral("uint")},
        {ParameterType::Float, QStringLiteral("float")},
        {ParameterType::Double, QStringLiteral("double")},
        {ParameterType::String, QStringLiteral("string")},
        {ParameterType::Bytes, QStringLiteral("bytes")},
    };
    return names;
}

const QHash<AccessMode, QString>& accessNames()
{
    static const QHash<AccessMode, QString> names{
        {AccessMode::ReadOnly, QStringLiteral("r")},
        {AccessMode::WriteOnly, QStringLiteral("w")},
        {AccessMode::ReadWrite, QStringLiteral("rw")},
    };
    return names;
}

bool isNumericType(ParameterType type)
{
    return type == ParameterType::Int || type == ParameterType::UInt
        || type == ParameterType::Float || type == ParameterType::Double;
}

} // namespace

QString parameterTypeName(ParameterType type)
{
    return typeNames().value(type, QStringLiteral("double"));
}

Result<ParameterType> parameterTypeFromName(const QString& name)
{
    const QString normalised = name.trimmed().toLower();
    for (auto it = typeNames().constBegin(); it != typeNames().constEnd(); ++it) {
        if (it.value() == normalised) {
            return it.key();
        }
    }
    return makeError(ErrorCode::ConfigInvalid,
                     QStringLiteral("unknown parameter type '%1'").arg(name));
}

QString accessModeName(AccessMode mode)
{
    return accessNames().value(mode, QStringLiteral("rw"));
}

Result<AccessMode> accessModeFromName(const QString& name)
{
    const QString normalised = name.trimmed().toLower();
    for (auto it = accessNames().constBegin(); it != accessNames().constEnd(); ++it) {
        if (it.value() == normalised) {
            return it.key();
        }
    }
    return makeError(ErrorCode::ConfigInvalid, QStringLiteral("unknown access mode '%1'").arg(name));
}

QString writeOriginName(WriteOrigin origin)
{
    static const QHash<WriteOrigin, QString> names{
        {WriteOrigin::Protocol, QStringLiteral("protocol")},
        {WriteOrigin::Ui, QStringLiteral("ui")},
        {WriteOrigin::Script, QStringLiteral("script")},
        {WriteOrigin::SignalSource, QStringLiteral("signal")},
        {WriteOrigin::StateMachine, QStringLiteral("state-machine")},
        {WriteOrigin::Restore, QStringLiteral("restore")},
    };
    return names.value(origin, QStringLiteral("unknown"));
}

// --- ParameterDefinition ---------------------------------------------------

ParameterDefinition ParameterDefinition::make(QString key, ParameterType type, QVariant defaultValue)
{
    ParameterDefinition definition;
    definition.key = std::move(key);
    definition.displayName = definition.key;
    definition.type = type;
    definition.defaultValue = std::move(defaultValue);
    return definition;
}

QJsonObject ParameterDefinition::toJson() const
{
    QJsonObject json;
    json.insert(QStringLiteral("key"), key);
    json.insert(QStringLiteral("type"), parameterTypeName(type));
    json.insert(QStringLiteral("access"), accessModeName(access));
    json.insert(QStringLiteral("default"), QJsonValue::fromVariant(defaultValue));

    if (!displayName.isEmpty() && displayName != key) {
        json.insert(QStringLiteral("displayName"), displayName);
    }
    if (!description.isEmpty()) json.insert(QStringLiteral("description"), description);
    if (!group.isEmpty()) json.insert(QStringLiteral("group"), group);
    if (!unit.isEmpty()) json.insert(QStringLiteral("unit"), unit);
    if (hasAddress) json.insert(QStringLiteral("address"), static_cast<qint64>(address));
    if (minimum) json.insert(QStringLiteral("minimum"), *minimum);
    if (maximum) json.insert(QStringLiteral("maximum"), *maximum);
    if (!qFuzzyCompare(scale, 1.0)) json.insert(QStringLiteral("scale"), scale);
    if (!qFuzzyIsNull(offset)) json.insert(QStringLiteral("offset"), offset);
    if (!persistent) json.insert(QStringLiteral("persistent"), false);

    return json;
}

Result<ParameterDefinition> ParameterDefinition::fromJson(const QJsonObject& json)
{
    ParameterDefinition definition;

    definition.key = json.value(QStringLiteral("key")).toString();
    if (definition.key.isEmpty()) {
        return makeError(ErrorCode::ConfigInvalid, QStringLiteral("parameter is missing 'key'"));
    }

    const auto type = parameterTypeFromName(
        json.value(QStringLiteral("type")).toString(QStringLiteral("double")));
    if (type.hasError()) {
        return type.error();
    }
    definition.type = type.value();

    const auto access = accessModeFromName(
        json.value(QStringLiteral("access")).toString(QStringLiteral("rw")));
    if (access.hasError()) {
        return access.error();
    }
    definition.access = access.value();

    definition.displayName =
        json.value(QStringLiteral("displayName")).toString(definition.key);
    definition.description = json.value(QStringLiteral("description")).toString();
    definition.group = json.value(QStringLiteral("group")).toString();
    definition.unit = json.value(QStringLiteral("unit")).toString();
    definition.defaultValue = json.value(QStringLiteral("default")).toVariant();

    if (json.contains(QStringLiteral("address"))) {
        definition.address = static_cast<quint32>(json.value(QStringLiteral("address")).toInteger());
        definition.hasAddress = true;
    }
    if (json.contains(QStringLiteral("minimum"))) {
        definition.minimum = json.value(QStringLiteral("minimum")).toDouble();
    }
    if (json.contains(QStringLiteral("maximum"))) {
        definition.maximum = json.value(QStringLiteral("maximum")).toDouble();
    }
    definition.scale = json.value(QStringLiteral("scale")).toDouble(1.0);
    definition.offset = json.value(QStringLiteral("offset")).toDouble(0.0);
    definition.persistent = json.value(QStringLiteral("persistent")).toBool(true);

    return definition;
}

// --- Parameter -------------------------------------------------------------

Parameter::Parameter(ParameterDefinition definition) : definition_(std::move(definition))
{
    resetToDefault();
}

QVariant Parameter::engineeringValue() const
{
    if (!isNumericType(definition_.type)) {
        return value_;
    }
    return value_.toDouble() * definition_.scale + definition_.offset;
}

void Parameter::resetToDefault()
{
    const auto coerced = coerce(definition_.defaultValue);
    value_ = coerced.hasValue() ? coerced.value() : definition_.defaultValue;
    lastChangedMs_ = core::monotonicMs();
}

Result<QVariant> Parameter::coerce(const QVariant& input) const
{
    switch (definition_.type) {
    case ParameterType::Bool:
        return QVariant(input.toBool());

    case ParameterType::String:
        return QVariant(input.toString());

    case ParameterType::Bytes:
        return QVariant(input.toByteArray());

    case ParameterType::Int:
    case ParameterType::UInt: {
        bool ok = false;
        qint64 numeric = input.toLongLong(&ok);
        if (!ok) {
            // Accept a floating point source, e.g. a signal generator feeding
            // an integer register.
            const double asDouble = input.toDouble(&ok);
            if (!ok) {
                return makeError(ErrorCode::InvalidArgument,
                                 QStringLiteral("'%1' is not an integer").arg(input.toString()),
                                 definition_.key);
            }
            numeric = static_cast<qint64>(qRound(asDouble));
        }
        if (definition_.minimum) numeric = qMax(numeric, static_cast<qint64>(*definition_.minimum));
        if (definition_.maximum) numeric = qMin(numeric, static_cast<qint64>(*definition_.maximum));
        if (definition_.type == ParameterType::UInt && numeric < 0) {
            numeric = 0;
        }
        return QVariant::fromValue(numeric);
    }

    case ParameterType::Float:
    case ParameterType::Double: {
        bool ok = false;
        double numeric = input.toDouble(&ok);
        if (!ok) {
            return makeError(ErrorCode::InvalidArgument,
                             QStringLiteral("'%1' is not a number").arg(input.toString()),
                             definition_.key);
        }
        if (definition_.minimum) numeric = qMax(numeric, *definition_.minimum);
        if (definition_.maximum) numeric = qMin(numeric, *definition_.maximum);
        return QVariant(numeric);
    }
    }

    return QVariant(input);
}

Result<void> Parameter::setValue(const QVariant& input, WriteOrigin origin)
{
    // The access mode describes what the protocol may do. The operator and the
    // signal generators drive the device from the inside and are not restricted
    // by it, otherwise a read-only sensor value could never change.
    if (origin == WriteOrigin::Protocol && definition_.access == AccessMode::ReadOnly) {
        return makeError(ErrorCode::ParameterReadOnly,
                         QStringLiteral("parameter '%1' is read-only").arg(definition_.key),
                         definition_.key);
    }

    const auto coerced = coerce(input);
    if (coerced.hasError()) {
        return coerced.error();
    }

    value_ = coerced.value();
    lastChangedMs_ = core::monotonicMs();
    ++writeCount_;
    return core::success();
}

} // namespace hwsim::simulator
