#include "SchemaFormWidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace hwsim::ui {
namespace {

constexpr int kIntegerSpinMinimum = -1000000000;
constexpr int kIntegerSpinMaximum = 1000000000;

QString labelTextFor(const core::ConfigField& field)
{
    return field.unit.isEmpty() ? field.label
                                : QStringLiteral("%1 (%2)").arg(field.label, field.unit);
}

} // namespace

SchemaFormWidget::SchemaFormWidget(QWidget* parent) : QWidget(parent)
{
    rootLayout_ = new QVBoxLayout(this);
    rootLayout_->setContentsMargins(0, 0, 0, 0);
    rootLayout_->setSpacing(8);
}

void SchemaFormWidget::clearSchema()
{
    fields_.clear();

    while (QLayoutItem* item = rootLayout_->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            // deleteLater alone would leave the widget parented and visible at
            // its old geometry until the event loop turns, so the outgoing form
            // would briefly paint on top of the incoming one.
            widget->hide();
            widget->setParent(nullptr);
            widget->deleteLater();
        }
        delete item;
    }
}

void SchemaFormWidget::setSchema(const core::ConfigSchema& schema, const QVariantMap& values)
{
    clearSchema();
    schema_ = schema;

    const QVariantMap effective = schema.withDefaults(values);

    // One form per group, in the order the groups first appear.
    for (const QString& group : schema.groups()) {
        auto* form = new QFormLayout;
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

        int fieldsInGroup = 0;
        for (const core::ConfigField& field : schema.fields()) {
            if (field.group != group) {
                continue;
            }

            QWidget* editor = createEditor(field, effective.value(field.key));
            if (editor == nullptr) {
                continue;
            }

            auto* label = new QLabel(labelTextFor(field), this);
            if (!field.description.isEmpty()) {
                label->setToolTip(field.description);
                editor->setToolTip(field.description);
            }

            form->addRow(label, editor);

            FieldWidgets entry;
            entry.field = field;
            entry.editor = editor;
            entry.label = label;
            fields_.insert(field.key, entry);
            ++fieldsInGroup;
        }

        if (fieldsInGroup == 0) {
            delete form;
            continue;
        }

        if (group.isEmpty()) {
            auto* plain = new QWidget(this);
            plain->setLayout(form);
            rootLayout_->addWidget(plain);
        } else {
            auto* box = new QGroupBox(group, this);
            box->setLayout(form);
            rootLayout_->addWidget(box);
        }
    }

    rootLayout_->addStretch(1);
    updateVisibility();
}

QWidget* SchemaFormWidget::createEditor(const core::ConfigField& field, const QVariant& value)
{
    using core::FieldType;

    const auto notify = [this, key = field.key] { onEditorChanged(key); };

    if (field.type == FieldType::Bool) {
        auto* box = new QCheckBox(this);
        box->setChecked(value.toBool());
        connect(box, &QCheckBox::toggled, this, notify);
        return box;
    }

    if (field.type == FieldType::Enum) {
        auto* combo = new QComboBox(this);
        for (int index = 0; index < field.enumValues.size(); ++index) {
            const QString text = index < field.enumLabels.size() ? field.enumLabels.at(index)
                                                                 : field.enumValues.at(index);
            combo->addItem(text, field.enumValues.at(index));
        }
        combo->setCurrentIndex(qMax(0, combo->findData(value.toString())));
        connect(combo, &QComboBox::currentIndexChanged, this, notify);
        return combo;
    }

    if (field.type == FieldType::Integer || field.type == FieldType::Port
        || field.type == FieldType::Duration) {
        auto* spin = new QSpinBox(this);
        spin->setRange(field.minimum ? static_cast<int>(*field.minimum) : kIntegerSpinMinimum,
                       field.maximum ? static_cast<int>(*field.maximum) : kIntegerSpinMaximum);
        spin->setValue(value.toInt());
        if (!field.unit.isEmpty()) {
            spin->setSuffix(QStringLiteral(" ") + field.unit);
        }
        spin->setGroupSeparatorShown(true);
        connect(spin, &QSpinBox::valueChanged, this, notify);
        return spin;
    }

    if (field.type == FieldType::Double) {
        auto* spin = new QDoubleSpinBox(this);
        spin->setDecimals(4);
        spin->setRange(field.minimum ? *field.minimum : -1e12,
                       field.maximum ? *field.maximum : 1e12);
        spin->setValue(value.toDouble());
        if (!field.unit.isEmpty()) {
            spin->setSuffix(QStringLiteral(" ") + field.unit);
        }
        connect(spin, &QDoubleSpinBox::valueChanged, this, notify);
        return spin;
    }

    if (field.type == FieldType::MultilineText) {
        auto* edit = new QPlainTextEdit(value.toString(), this);
        edit->setMinimumHeight(80);
        connect(edit, &QPlainTextEdit::textChanged, this, notify);
        return edit;
    }

    if (field.type == FieldType::FilePath || field.type == FieldType::DirectoryPath) {
        auto* container = new QWidget(this);
        auto* layout = new QHBoxLayout(container);
        layout->setContentsMargins(0, 0, 0, 0);

        auto* edit = new QLineEdit(value.toString(), container);
        edit->setObjectName(QStringLiteral("pathEdit"));
        auto* browse = new QPushButton(QStringLiteral("浏览..."), container);
        browse->setMaximumWidth(72);

        layout->addWidget(edit, 1);
        layout->addWidget(browse);

        const bool isDirectory = field.type == FieldType::DirectoryPath;
        connect(browse, &QPushButton::clicked, this, [this, edit, isDirectory, label = field.label] {
            const QString chosen = isDirectory
                                       ? QFileDialog::getExistingDirectory(this, label, edit->text())
                                       : QFileDialog::getOpenFileName(this, label, edit->text());
            if (!chosen.isEmpty()) {
                edit->setText(chosen);
            }
        });
        connect(edit, &QLineEdit::textChanged, this, notify);
        return container;
    }

    // Text, Host and HexBytes all edit as a single line; only the placeholder differs.
    auto* edit = new QLineEdit(value.toString(), this);
    if (!field.placeholder.isEmpty()) {
        edit->setPlaceholderText(field.placeholder);
    } else if (field.type == FieldType::HexBytes) {
        edit->setPlaceholderText(QStringLiteral("01 A2 FF"));
    }
    connect(edit, &QLineEdit::textChanged, this, notify);
    return edit;
}

QVariant SchemaFormWidget::readEditor(const FieldWidgets& entry) const
{
    if (auto* box = qobject_cast<QCheckBox*>(entry.editor)) {
        return box->isChecked();
    }
    if (auto* combo = qobject_cast<QComboBox*>(entry.editor)) {
        return combo->currentData();
    }
    if (auto* spin = qobject_cast<QSpinBox*>(entry.editor)) {
        return spin->value();
    }
    if (auto* spin = qobject_cast<QDoubleSpinBox*>(entry.editor)) {
        return spin->value();
    }
    if (auto* edit = qobject_cast<QPlainTextEdit*>(entry.editor)) {
        return edit->toPlainText();
    }
    if (auto* edit = qobject_cast<QLineEdit*>(entry.editor)) {
        return edit->text();
    }
    if (auto* edit = entry.editor->findChild<QLineEdit*>(QStringLiteral("pathEdit"))) {
        return edit->text();
    }
    return {};
}

void SchemaFormWidget::writeEditor(const FieldWidgets& entry, const QVariant& value)
{
    if (auto* box = qobject_cast<QCheckBox*>(entry.editor)) {
        box->setChecked(value.toBool());
    } else if (auto* combo = qobject_cast<QComboBox*>(entry.editor)) {
        combo->setCurrentIndex(qMax(0, combo->findData(value.toString())));
    } else if (auto* spin = qobject_cast<QSpinBox*>(entry.editor)) {
        spin->setValue(value.toInt());
    } else if (auto* spin = qobject_cast<QDoubleSpinBox*>(entry.editor)) {
        spin->setValue(value.toDouble());
    } else if (auto* edit = qobject_cast<QPlainTextEdit*>(entry.editor)) {
        edit->setPlainText(value.toString());
    } else if (auto* edit = qobject_cast<QLineEdit*>(entry.editor)) {
        edit->setText(value.toString());
    } else if (auto* edit = entry.editor->findChild<QLineEdit*>(QStringLiteral("pathEdit"))) {
        edit->setText(value.toString());
    }
}

QVariantMap SchemaFormWidget::values() const
{
    QVariantMap result;
    for (auto it = fields_.constBegin(); it != fields_.constEnd(); ++it) {
        result.insert(it.key(), readEditor(it.value()));
    }
    return schema_.normalise(result);
}

void SchemaFormWidget::setValues(const QVariantMap& values)
{
    updating_ = true;
    for (auto it = fields_.constBegin(); it != fields_.constEnd(); ++it) {
        if (values.contains(it.key())) {
            writeEditor(it.value(), values.value(it.key()));
        }
    }
    updating_ = false;
    updateVisibility();
}

core::Result<void> SchemaFormWidget::validate() const
{
    return schema_.validate(values());
}

void SchemaFormWidget::setAdvancedVisible(bool visible)
{
    advancedVisible_ = visible;
    updateVisibility();
}

void SchemaFormWidget::setReadOnly(bool readOnly)
{
    readOnly_ = readOnly;
    for (auto it = fields_.constBegin(); it != fields_.constEnd(); ++it) {
        it.value().editor->setEnabled(!readOnly);
    }
}

void SchemaFormWidget::onEditorChanged(const QString& key)
{
    if (updating_) {
        return;
    }

    const auto it = fields_.constFind(key);
    if (it == fields_.constEnd()) {
        return;
    }

    // A change may reveal or hide dependent fields, so re-run the expressions.
    updateVisibility();

    emit valueChanged(key, readEditor(it.value()));
    emit valuesChanged();
}

void SchemaFormWidget::updateVisibility()
{
    // Read raw editor state rather than values(), which would normalise and
    // could fight with what the user is currently typing.
    QVariantMap current;
    for (auto it = fields_.constBegin(); it != fields_.constEnd(); ++it) {
        current.insert(it.key(), readEditor(it.value()));
    }

    for (auto it = fields_.constBegin(); it != fields_.constEnd(); ++it) {
        const core::ConfigField& field = it.value().field;
        const bool visible = schema_.isVisible(field, current)
                             && (advancedVisible_ || !field.advanced);

        it.value().editor->setVisible(visible);
        if (it.value().label != nullptr) {
            it.value().label->setVisible(visible);
        }
    }
}

} // namespace hwsim::ui
