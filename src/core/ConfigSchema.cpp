#include "ConfigSchema.h"

#include <QHash>
#include <QJsonArray>

#include <algorithm>

namespace hwsim::core {
namespace {

ConfigField makeField(QString key, QString label, FieldType type, QVariant defaultValue)
{
    ConfigField field;
    field.key = std::move(key);
    field.label = std::move(label);
    field.type = type;
    field.defaultValue = std::move(defaultValue);
    return field;
}

/// Whether a field can fall back on its default when no value is supplied.
///
/// Deliberately not QVariant::isNull(): Qt 6 narrowed that to invalid variants
/// and null pointers, so QVariant(QString()) -- what a field declared with an
/// empty default holds -- now answers "not null". Relying on it would let every
/// required field pass validation with nothing behind it.
bool hasUsableDefault(const ConfigField& field)
{
    if (!field.defaultValue.isValid() || field.defaultValue.isNull()) {
        return false;
    }
    return !(field.defaultValue.typeId() == QMetaType::QString
             && field.defaultValue.toString().isEmpty());
}

bool isNumeric(FieldType type)
{
    return type == FieldType::Integer || type == FieldType::Double
        || type == FieldType::Port || type == FieldType::Duration;
}

} // namespace

// --- ConfigField factories -------------------------------------------------

ConfigField ConfigField::boolean(QString key, QString label, bool defaultValue)
{
    return makeField(std::move(key), std::move(label), FieldType::Bool, defaultValue);
}

ConfigField ConfigField::integer(QString key, QString label, qint64 defaultValue)
{
    return makeField(std::move(key), std::move(label), FieldType::Integer,
                     QVariant::fromValue(defaultValue));
}

ConfigField ConfigField::number(QString key, QString label, double defaultValue)
{
    return makeField(std::move(key), std::move(label), FieldType::Double, defaultValue);
}

ConfigField ConfigField::text(QString key, QString label, QString defaultValue)
{
    return makeField(std::move(key), std::move(label), FieldType::Text, std::move(defaultValue));
}

ConfigField ConfigField::enumeration(QString key, QString label, QStringList values,
                                     QString defaultValue)
{
    if (defaultValue.isEmpty() && !values.isEmpty()) {
        defaultValue = values.first();
    }
    ConfigField field = makeField(std::move(key), std::move(label), FieldType::Enum,
                                  std::move(defaultValue));
    field.enumValues = std::move(values);
    return field;
}

ConfigField ConfigField::host(QString key, QString label, QString defaultValue)
{
    return makeField(std::move(key), std::move(label), FieldType::Host, std::move(defaultValue));
}

ConfigField ConfigField::port(QString key, QString label, quint16 defaultValue)
{
    ConfigField field = makeField(std::move(key), std::move(label), FieldType::Port,
                                  static_cast<int>(defaultValue));
    // Minimum 0, not 1: port 0 means "let the OS pick a free one", which the
    // transports rely on and which the tests use to avoid collisions.
    field.minimum = 0.0;
    field.maximum = 65535.0;
    field.description = QStringLiteral("0 表示由系统自动分配空闲端口");
    return field;
}

ConfigField ConfigField::filePath(QString key, QString label, QString defaultValue)
{
    return makeField(std::move(key), std::move(label), FieldType::FilePath, std::move(defaultValue));
}

ConfigField ConfigField::hexBytes(QString key, QString label, QString defaultValue)
{
    return makeField(std::move(key), std::move(label), FieldType::HexBytes, std::move(defaultValue));
}

ConfigField ConfigField::duration(QString key, QString label, qint64 defaultMs)
{
    ConfigField field = makeField(std::move(key), std::move(label), FieldType::Duration,
                                  QVariant::fromValue(defaultMs));
    field.unit = QStringLiteral("ms");
    field.minimum = 0.0;
    return field;
}

// --- ConfigField fluent setters --------------------------------------------

ConfigField& ConfigField::range(double minimumValue, double maximumValue)
{
    minimum = minimumValue;
    maximum = maximumValue;
    return *this;
}

ConfigField& ConfigField::describedAs(QString text)
{
    description = std::move(text);
    return *this;
}

ConfigField& ConfigField::inGroup(QString name)
{
    group = std::move(name);
    return *this;
}

ConfigField& ConfigField::withUnit(QString text)
{
    unit = std::move(text);
    return *this;
}

ConfigField& ConfigField::withLabels(QStringList labels)
{
    enumLabels = std::move(labels);
    return *this;
}

ConfigField& ConfigField::withPlaceholder(QString text)
{
    placeholder = std::move(text);
    return *this;
}

ConfigField& ConfigField::optional()
{
    required = false;
    return *this;
}

ConfigField& ConfigField::asAdvanced()
{
    advanced = true;
    return *this;
}

ConfigField& ConfigField::shownWhen(QString expression)
{
    visibleWhen = std::move(expression);
    return *this;
}

// --- ConfigSchema ----------------------------------------------------------

ConfigSchema& ConfigSchema::add(ConfigField field)
{
    const auto existing = std::find_if(fields_.begin(), fields_.end(),
                                       [&field](const ConfigField& candidate) {
                                           return candidate.key == field.key;
                                       });
    if (existing != fields_.end()) {
        *existing = std::move(field);
    } else {
        fields_.push_back(std::move(field));
    }
    return *this;
}

ConfigSchema& ConfigSchema::merge(const ConfigSchema& other, const QString& groupOverride)
{
    for (ConfigField field : other.fields_) {
        if (!groupOverride.isEmpty()) {
            field.group = groupOverride;
        }
        add(std::move(field));
    }
    return *this;
}

const ConfigField* ConfigSchema::field(const QString& key) const
{
    const auto it = std::find_if(fields_.begin(), fields_.end(),
                                 [&key](const ConfigField& candidate) { return candidate.key == key; });
    return it == fields_.end() ? nullptr : &(*it);
}

QStringList ConfigSchema::groups() const
{
    QStringList result;
    for (const ConfigField& field : fields_) {
        if (!result.contains(field.group)) {
            result.append(field.group);
        }
    }
    return result;
}

QVariantMap ConfigSchema::defaults() const
{
    QVariantMap result;
    for (const ConfigField& field : fields_) {
        result.insert(field.key, field.defaultValue);
    }
    return result;
}

QVariantMap ConfigSchema::withDefaults(QVariantMap values) const
{
    for (const ConfigField& field : fields_) {
        if (!values.contains(field.key)) {
            values.insert(field.key, field.defaultValue);
        }
    }
    return values;
}

bool ConfigSchema::isVisible(const ConfigField& field, const QVariantMap& values) const
{
    if (field.visibleWhen.isEmpty()) {
        return true;
    }

    const bool negated = field.visibleWhen.contains(QStringLiteral("!="));
    const QString separator = negated ? QStringLiteral("!=") : QStringLiteral("==");
    const auto parts = field.visibleWhen.split(separator);
    if (parts.size() != 2) {
        return true;
    }

    const QString dependencyKey = parts.at(0).trimmed();
    const QString expected = parts.at(1).trimmed();
    const QString actual = values.value(dependencyKey).toString();
    return negated ? actual != expected : actual == expected;
}

Result<void> ConfigSchema::validate(const QVariantMap& values) const
{
    for (const ConfigField& field : fields_) {
        if (!isVisible(field, values)) {
            continue;
        }

        const bool present = values.contains(field.key);
        const QVariant value = values.value(field.key);

        if (!present || !value.isValid() || (value.typeId() == QMetaType::QString && value.toString().isEmpty())) {
            if (field.required && !hasUsableDefault(field)) {
                return makeError(ErrorCode::ConfigInvalid,
                                 QStringLiteral("required field '%1' is missing").arg(field.label),
                                 field.key);
            }
            continue;
        }

        if (field.type == FieldType::Enum && !field.enumValues.contains(value.toString())) {
            return makeError(ErrorCode::ConfigInvalid,
                             QStringLiteral("'%1' is not one of: %2")
                                 .arg(value.toString(), field.enumValues.join(QStringLiteral(", "))),
                             field.key);
        }

        if (isNumeric(field.type)) {
            bool ok = false;
            const double numeric = value.toDouble(&ok);
            if (!ok) {
                return makeError(ErrorCode::ConfigInvalid,
                                 QStringLiteral("field '%1' expects a number").arg(field.label),
                                 field.key);
            }
            if (field.minimum && numeric < *field.minimum) {
                return makeError(ErrorCode::ConfigInvalid,
                                 QStringLiteral("field '%1' must be at least %2")
                                     .arg(field.label).arg(*field.minimum),
                                 field.key);
            }
            if (field.maximum && numeric > *field.maximum) {
                return makeError(ErrorCode::ConfigInvalid,
                                 QStringLiteral("field '%1' must not exceed %2")
                                     .arg(field.label).arg(*field.maximum),
                                 field.key);
            }
        }
    }
    return success();
}

QVariantMap ConfigSchema::normalise(QVariantMap values) const
{
    values = withDefaults(std::move(values));

    for (const ConfigField& field : fields_) {
        QVariant value = values.value(field.key);
        if (!value.isValid()) {
            continue;
        }

        if (field.type == FieldType::Bool) {
            value = value.toBool();
        } else if (field.type == FieldType::Integer || field.type == FieldType::Port
                   || field.type == FieldType::Duration) {
            qint64 numeric = value.toLongLong();
            if (field.minimum) numeric = qMax(numeric, static_cast<qint64>(*field.minimum));
            if (field.maximum) numeric = qMin(numeric, static_cast<qint64>(*field.maximum));
            value = QVariant::fromValue(numeric);
        } else if (field.type == FieldType::Double) {
            double numeric = value.toDouble();
            if (field.minimum) numeric = qMax(numeric, *field.minimum);
            if (field.maximum) numeric = qMin(numeric, *field.maximum);
            value = numeric;
        } else if (field.type == FieldType::Enum) {
            if (!field.enumValues.contains(value.toString())) {
                value = field.defaultValue;
            }
        }

        values.insert(field.key, value);
    }
    return values;
}

QJsonObject ConfigSchema::toJson() const
{
    QJsonArray fieldArray;
    for (const ConfigField& field : fields_) {
        QJsonObject item;
        item.insert(QStringLiteral("key"), field.key);
        item.insert(QStringLiteral("label"), field.label);
        item.insert(QStringLiteral("type"), fieldTypeName(field.type));
        item.insert(QStringLiteral("default"), QJsonValue::fromVariant(field.defaultValue));
        if (!field.description.isEmpty()) item.insert(QStringLiteral("description"), field.description);
        if (!field.group.isEmpty()) item.insert(QStringLiteral("group"), field.group);
        if (!field.unit.isEmpty()) item.insert(QStringLiteral("unit"), field.unit);
        if (field.minimum) item.insert(QStringLiteral("minimum"), *field.minimum);
        if (field.maximum) item.insert(QStringLiteral("maximum"), *field.maximum);
        if (!field.enumValues.isEmpty()) {
            item.insert(QStringLiteral("values"), QJsonArray::fromStringList(field.enumValues));
        }
        if (!field.visibleWhen.isEmpty()) item.insert(QStringLiteral("visibleWhen"), field.visibleWhen);
        if (!field.required) item.insert(QStringLiteral("required"), false);
        if (field.advanced) item.insert(QStringLiteral("advanced"), true);
        fieldArray.append(item);
    }

    QJsonObject root;
    root.insert(QStringLiteral("title"), title_);
    root.insert(QStringLiteral("fields"), fieldArray);
    return root;
}

QString fieldTypeName(FieldType type)
{
    static const QHash<FieldType, QString> kNames{
        {FieldType::Bool, QStringLiteral("bool")},
        {FieldType::Integer, QStringLiteral("integer")},
        {FieldType::Double, QStringLiteral("double")},
        {FieldType::Text, QStringLiteral("text")},
        {FieldType::MultilineText, QStringLiteral("multiline")},
        {FieldType::Enum, QStringLiteral("enum")},
        {FieldType::Host, QStringLiteral("host")},
        {FieldType::Port, QStringLiteral("port")},
        {FieldType::FilePath, QStringLiteral("file")},
        {FieldType::DirectoryPath, QStringLiteral("directory")},
        {FieldType::HexBytes, QStringLiteral("hex")},
        {FieldType::Duration, QStringLiteral("duration")},
    };
    return kNames.value(type, QStringLiteral("text"));
}

} // namespace hwsim::core
