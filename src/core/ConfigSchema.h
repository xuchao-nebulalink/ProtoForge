#pragma once

#include "CoreGlobal.h"
#include "Result.h"

#include <QJsonObject>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

#include <optional>

namespace hwsim::core {

enum class FieldType {
    Bool,
    Integer,
    Double,
    Text,
    MultilineText,
    Enum,
    Host,
    Port,
    FilePath,
    DirectoryPath,
    HexBytes,
    Duration,
};

/// One configurable value, described well enough that the UI can build an
/// editor for it and the loader can validate a saved profile.
///
/// Transports, codecs, signal sources and fault rules all publish a schema
/// instead of a hand-written settings dialog. A new protocol plugin therefore
/// gets a working configuration panel without touching the UI module at all.
struct HWSIM_CORE_API ConfigField {
    QString key;
    QString label;
    QString description;
    FieldType type{FieldType::Text};
    QVariant defaultValue;
    std::optional<double> minimum;
    std::optional<double> maximum;
    QStringList enumValues;
    QStringList enumLabels;
    QString group;
    QString unit;
    QString placeholder;
    bool required{true};
    bool advanced{false};

    /// Simple dependency expression, "key==value" or "key!=value". Lets a
    /// schema hide the listen port when the transport is in client mode.
    QString visibleWhen;

    static ConfigField boolean(QString key, QString label, bool defaultValue = false);
    static ConfigField integer(QString key, QString label, qint64 defaultValue = 0);
    static ConfigField number(QString key, QString label, double defaultValue = 0.0);
    static ConfigField text(QString key, QString label, QString defaultValue = {});
    static ConfigField enumeration(QString key, QString label, QStringList values,
                                   QString defaultValue = {});
    static ConfigField host(QString key, QString label, QString defaultValue = QStringLiteral("127.0.0.1"));
    static ConfigField port(QString key, QString label, quint16 defaultValue = 502);
    static ConfigField filePath(QString key, QString label, QString defaultValue = {});
    static ConfigField hexBytes(QString key, QString label, QString defaultValue = {});
    static ConfigField duration(QString key, QString label, qint64 defaultMs = 1000);

    ConfigField& range(double minimum, double maximum);
    ConfigField& describedAs(QString text);
    ConfigField& inGroup(QString name);
    ConfigField& withUnit(QString text);
    ConfigField& withLabels(QStringList labels);
    ConfigField& withPlaceholder(QString text);
    ConfigField& optional();
    ConfigField& asAdvanced();
    ConfigField& shownWhen(QString expression);
};

class HWSIM_CORE_API ConfigSchema {
public:
    ConfigSchema() = default;
    explicit ConfigSchema(QString title) : title_(std::move(title)) {}

    ConfigSchema& add(ConfigField field);
    ConfigSchema& merge(const ConfigSchema& other, const QString& groupOverride = {});

    [[nodiscard]] const QVector<ConfigField>& fields() const noexcept { return fields_; }
    [[nodiscard]] const ConfigField* field(const QString& key) const;
    [[nodiscard]] bool isEmpty() const noexcept { return fields_.isEmpty(); }

    [[nodiscard]] QString title() const { return title_; }
    void setTitle(QString title) { title_ = std::move(title); }

    /// Ordered list of distinct group names; fields without a group come first
    /// under an empty name.
    [[nodiscard]] QStringList groups() const;

    [[nodiscard]] QVariantMap defaults() const;

    /// Fills in any missing key with its default value.
    [[nodiscard]] QVariantMap withDefaults(QVariantMap values) const;

    /// Checks types, ranges, enum membership and required-ness. Only fields
    /// that are currently visible are enforced.
    [[nodiscard]] Result<void> validate(const QVariantMap& values) const;

    /// Coerces values to the declared types and clamps numbers into range.
    [[nodiscard]] QVariantMap normalise(QVariantMap values) const;

    [[nodiscard]] bool isVisible(const ConfigField& field, const QVariantMap& values) const;

    [[nodiscard]] QJsonObject toJson() const;

private:
    QString title_;
    QVector<ConfigField> fields_;
};

[[nodiscard]] HWSIM_CORE_API QString fieldTypeName(FieldType type);

} // namespace hwsim::core
