#include "ScenarioPanel.h"

#include "SchemaFormWidget.h"

#include <core/Clock.h>
#include <simulator/ISignalSource.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace hwsim::ui {
namespace {

constexpr int kRefreshIntervalMs = 500;

/// Column carrying the enable checkbox in each table.
constexpr int kEnabledColumn = 4;
constexpr int kFaultEnabledColumn = 3;

QTableWidgetItem* readOnlyItem(const QString& text, const QString& id = {})
{
    auto* item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    if (!id.isEmpty()) {
        item->setData(Qt::UserRole, id);
    }
    return item;
}

/// Modal editor for one signal source or fault rule.
///
/// The kind combo drives a SchemaFormWidget: choosing a different kind swaps
/// the whole form for the one that kind's schema describes.
class KindConfigDialog : public QDialog {
public:
    KindConfigDialog(const QString& title, const QStringList& kinds,
                     std::function<core::ConfigSchema(const QString&)> schemaFor, QWidget* parent)
        : QDialog(parent), schemaFor_(std::move(schemaFor))
    {
        setWindowTitle(title);
        resize(460, 520);

        kindCombo_ = new QComboBox(this);
        kindCombo_->addItems(kinds);

        form_ = new SchemaFormWidget(this);

        auto* scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setWidget(form_);
        scroll->setFrameShape(QFrame::NoFrame);

        extraLayout_ = new QFormLayout;

        auto* buttons =
            new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

        auto* layout = new QVBoxLayout(this);
        auto* topForm = new QFormLayout;
        topForm->addRow(QStringLiteral("类型"), kindCombo_);
        layout->addLayout(topForm);
        layout->addLayout(extraLayout_);
        layout->addWidget(scroll, 1);
        layout->addWidget(buttons);

        connect(kindCombo_, &QComboBox::currentTextChanged, this, [this](const QString& kind) {
            form_->setSchema(schemaFor_(kind));
        });
        connect(buttons, &QDialogButtonBox::accepted, this, [this] {
            if (const auto valid = form_->validate(); valid.hasError()) {
                QMessageBox::warning(this, QStringLiteral("配置无效"), valid.error().toString());
                return;
            }
            accept();
        });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

        if (!kinds.isEmpty()) {
            form_->setSchema(schemaFor_(kinds.first()));
        }
    }

    [[nodiscard]] QString selectedKind() const { return kindCombo_->currentText(); }
    [[nodiscard]] QVariantMap values() const { return form_->values(); }
    [[nodiscard]] QFormLayout* extraLayout() const { return extraLayout_; }

private:
    std::function<core::ConfigSchema(const QString&)> schemaFor_;
    QComboBox* kindCombo_{nullptr};
    SchemaFormWidget* form_{nullptr};
    QFormLayout* extraLayout_{nullptr};
};

} // namespace

ScenarioPanel::ScenarioPanel(QWidget* parent) : QWidget(parent)
{
    auto* tabs = new QTabWidget(this);
    tabs->addTab(buildSignalsTab(), QStringLiteral("实时数据"));
    tabs->addTab(buildFaultsTab(), QStringLiteral("故障注入"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(tabs);

    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(kRefreshIntervalMs);
    connect(refreshTimer_, &QTimer::timeout, this, [this] {
        if (isVisible()) {
            refresh();
        }
    });
    refreshTimer_->start();
}

QWidget* ScenarioPanel::buildSignalsTab()
{
    auto* page = new QWidget(this);

    signalTable_ = new QTableWidget(0, 6, page);
    signalTable_->setHorizontalHeaderLabels({QStringLiteral("参数"), QStringLiteral("波形"),
                                             QStringLiteral("叠加方式"), QStringLiteral("周期"),
                                             QStringLiteral("启用"), QStringLiteral("当前值")});
    signalTable_->horizontalHeader()->setStretchLastSection(true);
    signalTable_->verticalHeader()->setVisible(false);
    signalTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    signalTable_->setSelectionMode(QAbstractItemView::SingleSelection);

    auto* addButton = new QPushButton(QStringLiteral("添加信号源"), page);
    auto* removeButton = new QPushButton(QStringLiteral("移除"), page);

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(addButton);
    buttons->addWidget(removeButton);
    buttons->addStretch(1);

    auto* layout = new QVBoxLayout(page);
    layout->addWidget(signalTable_, 1);
    layout->addLayout(buttons);

    connect(addButton, &QPushButton::clicked, this, &ScenarioPanel::addSignalBinding);
    connect(removeButton, &QPushButton::clicked, this,
            &ScenarioPanel::removeSelectedSignalBinding);

    // Connected once here, never inside refreshSignals(): repopulating the
    // table emits itemChanged for every cell, which would otherwise write the
    // displayed state straight back into the model and clobber anything a
    // script changed since the last refresh.
    connect(signalTable_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (item->column() != kEnabledColumn || controller_ == nullptr) {
            return;
        }
        controller_->setSignalBindingEnabled(deviceId_, item->data(Qt::UserRole).toString(),
                                             item->checkState() == Qt::Checked);
    });

    return page;
}

QWidget* ScenarioPanel::buildFaultsTab()
{
    auto* page = new QWidget(this);

    faultMasterSwitch_ = new QCheckBox(QStringLiteral("启用故障注入"), page);
    faultMasterSwitch_->setChecked(true);

    faultTable_ = new QTableWidget(0, 6, page);
    faultTable_->setHorizontalHeaderLabels({QStringLiteral("规则"), QStringLiteral("类型"),
                                            QStringLiteral("方向"), QStringLiteral("启用"),
                                            QStringLiteral("命中/评估"),
                                            QStringLiteral("最近触发")});
    faultTable_->horizontalHeader()->setStretchLastSection(true);
    faultTable_->verticalHeader()->setVisible(false);
    faultTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    faultTable_->setSelectionMode(QAbstractItemView::SingleSelection);

    auto* addButton = new QPushButton(QStringLiteral("添加故障规则"), page);
    auto* removeButton = new QPushButton(QStringLiteral("移除"), page);
    auto* armButton = new QPushButton(QStringLiteral("手动触发一次"), page);

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(addButton);
    buttons->addWidget(removeButton);
    buttons->addWidget(armButton);
    buttons->addStretch(1);

    auto* layout = new QVBoxLayout(page);
    layout->addWidget(faultMasterSwitch_);
    layout->addWidget(faultTable_, 1);
    layout->addLayout(buttons);

    connect(addButton, &QPushButton::clicked, this, &ScenarioPanel::addFaultRule);
    connect(removeButton, &QPushButton::clicked, this, &ScenarioPanel::removeSelectedFaultRule);
    connect(armButton, &QPushButton::clicked, this, &ScenarioPanel::armSelectedFaultRule);
    connect(faultTable_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (item->column() != kFaultEnabledColumn || controller_ == nullptr) {
            return;
        }
        controller_->setFaultRuleEnabled(deviceId_, item->data(Qt::UserRole).toString(),
                                         item->checkState() == Qt::Checked);
    });
    connect(faultMasterSwitch_, &QCheckBox::toggled, this, [this](bool checked) {
        if (controller_ != nullptr && !deviceId_.isEmpty()) {
            controller_->setFaultInjectionEnabled(deviceId_, checked);
            emit statusMessage(checked ? QStringLiteral("故障注入已启用")
                                       : QStringLiteral("故障注入已关闭"));
        }
    });

    return page;
}

void ScenarioPanel::setController(IDeviceController* controller)
{
    controller_ = controller;
    refresh();
}

void ScenarioPanel::setDevice(const QString& deviceId)
{
    deviceId_ = deviceId;
    refresh();
}

void ScenarioPanel::refresh()
{
    refreshSignals();
    refreshFaults();
}

void ScenarioPanel::refreshSignals()
{
    // Repopulating emits itemChanged per cell; blocking keeps that from being
    // mistaken for the user toggling a checkbox.
    const QSignalBlocker blocker(signalTable_);

    signalTable_->setRowCount(0);
    if (controller_ == nullptr || deviceId_.isEmpty()) {
        return;
    }

    const auto bindings = controller_->signalBindings(deviceId_);
    signalTable_->setRowCount(static_cast<int>(bindings.size()));

    for (int row = 0; row < bindings.size(); ++row) {
        const simulator::SignalBindingInfo& binding = bindings.at(row);

        signalTable_->setItem(row, 0, readOnlyItem(binding.parameterKey, binding.id));
        signalTable_->setItem(row, 1, readOnlyItem(binding.sourceKind));
        signalTable_->setItem(row, 2, readOnlyItem(simulator::combineModeName(binding.combine)));
        signalTable_->setItem(row, 3,
                              readOnlyItem(QStringLiteral("%1 ms").arg(binding.intervalMs)));

        auto* enabled = new QTableWidgetItem;
        enabled->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
        enabled->setCheckState(binding.enabled ? Qt::Checked : Qt::Unchecked);
        enabled->setData(Qt::UserRole, binding.id);
        signalTable_->setItem(row, 4, enabled);

        signalTable_->setItem(row, 5,
                              readOnlyItem(QString::number(binding.lastValue, 'f', 3)));
    }
}

void ScenarioPanel::refreshFaults()
{
    const QSignalBlocker tableBlocker(faultTable_);

    faultTable_->setRowCount(0);
    if (controller_ == nullptr || deviceId_.isEmpty()) {
        return;
    }

    {
        const QSignalBlocker switchBlocker(faultMasterSwitch_);
        faultMasterSwitch_->setChecked(controller_->isFaultInjectionEnabled(deviceId_));
    }

    const auto rules = controller_->faultRules(deviceId_);
    faultTable_->setRowCount(static_cast<int>(rules.size()));

    for (int row = 0; row < rules.size(); ++row) {
        const simulator::FaultRuleInfo& rule = rules.at(row);

        faultTable_->setItem(row, 0, readOnlyItem(rule.id, rule.id));
        faultTable_->setItem(row, 1, readOnlyItem(rule.displayName));
        faultTable_->setItem(
            row, 2, readOnlyItem(rule.configuration.value(QStringLiteral("direction")).toString()));

        auto* enabled = new QTableWidgetItem;
        enabled->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
        enabled->setCheckState(rule.enabled ? Qt::Checked : Qt::Unchecked);
        enabled->setData(Qt::UserRole, rule.id);
        faultTable_->setItem(row, 3, enabled);

        faultTable_->setItem(row, 4,
                             readOnlyItem(QStringLiteral("%1 / %2")
                                              .arg(rule.statistics.activations)
                                              .arg(rule.statistics.evaluations)));
        faultTable_->setItem(
            row, 5,
            readOnlyItem(rule.statistics.lastActivationMs == 0
                             ? QStringLiteral("-")
                             : core::formatWallClock(rule.statistics.lastActivationMs)));
    }
}

QString ScenarioPanel::selectedId(QTableWidget* table) const
{
    const QList<QTableWidgetItem*> selected = table->selectedItems();
    if (selected.isEmpty()) {
        return {};
    }
    return table->item(selected.first()->row(), 0)->data(Qt::UserRole).toString();
}

void ScenarioPanel::addSignalBinding()
{
    if (controller_ == nullptr || deviceId_.isEmpty()) {
        return;
    }

    simulator::registerBuiltinSignalSources();

    QStringList kinds;
    for (const QString& key : simulator::signalSourceRegistry().keys()) {
        kinds.append(key);
    }
    kinds.sort();

    KindConfigDialog dialog(QStringLiteral("添加信号源"), kinds,
                            [](const QString& kind) -> core::ConfigSchema {
                                auto created = simulator::signalSourceRegistry().create(kind);
                                return created.hasValue() ? created.value()->schema()
                                                          : core::ConfigSchema{};
                            },
                            this);

    // Binding-level settings that are not part of the source's own schema.
    auto* parameterCombo = new QComboBox(&dialog);
    for (const auto& definition : controller_->parameterDefinitions(deviceId_)) {
        parameterCombo->addItem(definition.key);
    }

    auto* intervalSpin = new QSpinBox(&dialog);
    intervalSpin->setRange(1, 600000);
    intervalSpin->setValue(100);
    intervalSpin->setSuffix(QStringLiteral(" ms"));

    auto* combineCombo = new QComboBox(&dialog);
    combineCombo->addItem(QStringLiteral("替换"), QStringLiteral("replace"));
    combineCombo->addItem(QStringLiteral("叠加"), QStringLiteral("add"));
    combineCombo->addItem(QStringLiteral("相乘"), QStringLiteral("multiply"));

    dialog.extraLayout()->addRow(QStringLiteral("目标参数"), parameterCombo);
    dialog.extraLayout()->addRow(QStringLiteral("刷新周期"), intervalSpin);
    dialog.extraLayout()->addRow(QStringLiteral("叠加方式"), combineCombo);

    if (dialog.exec() != QDialog::Accepted || parameterCombo->currentText().isEmpty()) {
        return;
    }

    const auto added = controller_->addSignalBinding(
        deviceId_, parameterCombo->currentText(), dialog.selectedKind(), dialog.values(),
        intervalSpin->value(),
        simulator::combineModeFromName(combineCombo->currentData().toString()));

    if (added.hasError()) {
        QMessageBox::warning(this, QStringLiteral("添加失败"), added.error().toString());
        return;
    }

    emit statusMessage(QStringLiteral("已添加信号源 %1").arg(added.value()));
    refreshSignals();
}

void ScenarioPanel::removeSelectedSignalBinding()
{
    const QString id = selectedId(signalTable_);
    if (id.isEmpty() || controller_ == nullptr) {
        return;
    }
    controller_->removeSignalBinding(deviceId_, id);
    refreshSignals();
}

void ScenarioPanel::addFaultRule()
{
    if (controller_ == nullptr || deviceId_.isEmpty()) {
        return;
    }

    KindConfigDialog dialog(QStringLiteral("添加故障规则"),
                            simulator::FaultInjector::availableKinds(),
                            [](const QString& kind) -> core::ConfigSchema {
                                const auto schema = simulator::FaultInjector::schemaFor(kind);
                                return schema.hasValue() ? schema.value() : core::ConfigSchema{};
                            },
                            this);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const auto added = controller_->addFaultRule(deviceId_, dialog.selectedKind(), dialog.values());
    if (added.hasError()) {
        QMessageBox::warning(this, QStringLiteral("添加失败"), added.error().toString());
        return;
    }

    emit statusMessage(QStringLiteral("已添加故障规则 %1").arg(added.value()));
    refreshFaults();
}

void ScenarioPanel::removeSelectedFaultRule()
{
    const QString id = selectedId(faultTable_);
    if (id.isEmpty() || controller_ == nullptr) {
        return;
    }
    controller_->removeFaultRule(deviceId_, id);
    refreshFaults();
}

void ScenarioPanel::armSelectedFaultRule()
{
    const QString id = selectedId(faultTable_);
    if (id.isEmpty() || controller_ == nullptr) {
        return;
    }
    if (controller_->armFaultRule(deviceId_, id)) {
        emit statusMessage(QStringLiteral("%1 已装填，下一帧生效").arg(id));
    }
}

} // namespace hwsim::ui
