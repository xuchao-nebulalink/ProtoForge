#pragma once

#include "UiGlobal.h"

#include <core/ConfigSchema.h>

#include <QHash>
#include <QVariantMap>
#include <QWidget>

class QFormLayout;
class QVBoxLayout;

namespace hwsim::ui {

/// Builds an editor from a core::ConfigSchema.
///
/// This is why every configurable thing in the framework publishes a schema:
/// transports, codecs, signal sources, fault rules and protocol plugins all get
/// a working settings form without a single line of UI code, and a new protocol
/// plugin gets one without touching this module at all.
///
/// Field types map to widgets, groups map to boxes, and `visibleWhen`
/// expressions are re-evaluated whenever a value changes, so dependent fields
/// appear and disappear as the user edits.
class HWSIM_UI_API SchemaFormWidget : public QWidget {
    Q_OBJECT

public:
    explicit SchemaFormWidget(QWidget* parent = nullptr);

    void setSchema(const core::ConfigSchema& schema, const QVariantMap& values = {});
    void clearSchema();

    [[nodiscard]] QVariantMap values() const;
    void setValues(const QVariantMap& values);

    [[nodiscard]] core::Result<void> validate() const;

    /// Hides fields marked advanced. Off by default so nothing is hidden by
    /// surprise; the owning panel usually offers a checkbox.
    void setAdvancedVisible(bool visible);
    [[nodiscard]] bool isAdvancedVisible() const noexcept { return advancedVisible_; }

    void setReadOnly(bool readOnly);

signals:
    void valueChanged(const QString& key, const QVariant& value);
    void valuesChanged();

private:
    struct FieldWidgets {
        core::ConfigField field;
        QWidget* editor{nullptr};
        QWidget* label{nullptr};
        QWidget* container{nullptr};
    };

    [[nodiscard]] QWidget* createEditor(const core::ConfigField& field, const QVariant& value);
    [[nodiscard]] QVariant readEditor(const FieldWidgets& entry) const;
    void writeEditor(const FieldWidgets& entry, const QVariant& value);
    void onEditorChanged(const QString& key);
    void updateVisibility();

    core::ConfigSchema schema_;
    QHash<QString, FieldWidgets> fields_;
    QVBoxLayout* rootLayout_{nullptr};
    bool advancedVisible_{true};
    bool readOnly_{false};
    bool updating_{false};
};

} // namespace hwsim::ui
