#include "ParameterPanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

namespace hwsim::ui {
namespace {

constexpr int kDefaultRefreshMs = 250;

QString formatValue(const simulator::ParameterDefinition& definition, const QVariant& value)
{
    if (definition.type == simulator::ParameterType::Bool) {
        return value.toBool() ? QStringLiteral("1") : QStringLiteral("0");
    }
    if (definition.type == simulator::ParameterType::Double
        || definition.type == simulator::ParameterType::Float) {
        return QString::number(value.toDouble(), 'f', 3);
    }
    return value.toString();
}

} // namespace

// --- ParameterTableModel ---------------------------------------------------

ParameterTableModel::ParameterTableModel(QObject* parent) : QAbstractTableModel(parent) {}

void ParameterTableModel::setController(IDeviceController* controller)
{
    controller_ = controller;
    reload();
}

void ParameterTableModel::setDevice(const QString& deviceId)
{
    deviceId_ = deviceId;
    reload();
}

void ParameterTableModel::reload()
{
    beginResetModel();
    if (controller_ != nullptr && !deviceId_.isEmpty()) {
        definitions_ = controller_->parameterDefinitions(deviceId_);
        values_ = controller_->parameterValues(deviceId_);
    } else {
        definitions_.clear();
        values_.clear();
    }
    endResetModel();
}

void ParameterTableModel::refreshValues()
{
    if (controller_ == nullptr || deviceId_.isEmpty() || definitions_.isEmpty()) {
        return;
    }

    values_ = controller_->parameterValues(deviceId_);
    emit dataChanged(index(0, ValueColumn), index(rowCount() - 1, ValueColumn),
                     {Qt::DisplayRole, Qt::EditRole});
}

int ParameterTableModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(definitions_.size());
}

int ParameterTableModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant ParameterTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= definitions_.size()) {
        return {};
    }

    const simulator::ParameterDefinition& definition = definitions_.at(index.row());
    const QVariant value = values_.value(definition.key);

    if (role == Qt::ToolTipRole) {
        return definition.description.isEmpty() ? definition.key : definition.description;
    }

    if (role == Qt::EditRole && index.column() == ValueColumn) {
        return value;
    }

    if (role != Qt::DisplayRole) {
        return {};
    }

    if (index.column() == NameColumn) {
        return definition.displayName.isEmpty() ? definition.key : definition.displayName;
    }
    if (index.column() == AddressColumn) {
        return definition.hasAddress ? QString::number(definition.address) : QStringLiteral("-");
    }
    if (index.column() == TypeColumn) return simulator::parameterTypeName(definition.type);
    if (index.column() == AccessColumn) return simulator::accessModeName(definition.access);
    if (index.column() == ValueColumn) return formatValue(definition, value);
    if (index.column() == UnitColumn) return definition.unit;
    if (index.column() == GroupColumn) return definition.group;

    return {};
}

bool ParameterTableModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
    if (role != Qt::EditRole || index.column() != ValueColumn || controller_ == nullptr) {
        return false;
    }
    if (index.row() >= definitions_.size()) {
        return false;
    }

    const simulator::ParameterDefinition& definition = definitions_.at(index.row());
    const auto written = controller_->setParameter(deviceId_, definition.key, value);
    if (written.hasError()) {
        emit writeFailed(definition.key, written.error().toString());
        return false;
    }

    values_.insert(definition.key, value);
    emit dataChanged(index, index, {Qt::DisplayRole, Qt::EditRole});
    return true;
}

Qt::ItemFlags ParameterTableModel::flags(const QModelIndex& index) const
{
    Qt::ItemFlags result = QAbstractTableModel::flags(index);
    // Every parameter is editable from here regardless of its protocol access
    // mode: the operator is driving the simulated device, not talking to it.
    if (index.column() == ValueColumn) {
        result |= Qt::ItemIsEditable;
    }
    return result;
}

QVariant ParameterTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }

    static const QStringList headers{
        QStringLiteral("名称"), QStringLiteral("地址"), QStringLiteral("类型"),
        QStringLiteral("权限"), QStringLiteral("当前值"), QStringLiteral("单位"),
        QStringLiteral("分组"),
    };
    return section < headers.size() ? headers.at(section) : QVariant{};
}

// --- ParameterPanel --------------------------------------------------------

ParameterPanel::ParameterPanel(QWidget* parent) : QWidget(parent)
{
    model_ = new ParameterTableModel(this);

    proxy_ = new QSortFilterProxyModel(this);
    proxy_->setSourceModel(model_);
    proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    proxy_->setFilterKeyColumn(-1);

    table_ = new QTableView(this);
    table_->setModel(proxy_);
    table_->setSortingEnabled(true);
    table_->setAlternatingRowColors(true);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setStretchLastSection(true);

    filterEdit_ = new QLineEdit(this);
    filterEdit_->setPlaceholderText(QStringLiteral("按名称 / 地址 / 分组过滤"));
    filterEdit_->setClearButtonEnabled(true);

    liveBox_ = new QCheckBox(QStringLiteral("实时刷新"), this);
    liveBox_->setChecked(true);

    auto* resetButton = new QPushButton(QStringLiteral("恢复默认值"), this);

    stateLabel_ = new QLabel(QStringLiteral("状态: -"), this);
    stateControls_ = new QWidget(this);
    auto* stateLayout = new QHBoxLayout(stateControls_);
    stateLayout->setContentsMargins(0, 0, 0, 0);

    auto* toolbar = new QHBoxLayout;
    toolbar->setContentsMargins(0, 0, 0, 0);
    toolbar->addWidget(filterEdit_, 1);
    toolbar->addWidget(liveBox_);
    toolbar->addWidget(resetButton);

    auto* statusRow = new QHBoxLayout;
    statusRow->setContentsMargins(0, 0, 0, 0);
    statusRow->addWidget(stateLabel_);
    statusRow->addWidget(stateControls_, 1);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);
    layout->addLayout(toolbar);
    layout->addWidget(table_, 1);
    layout->addLayout(statusRow);

    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(kDefaultRefreshMs);
    connect(refreshTimer_, &QTimer::timeout, this, &ParameterPanel::onRefreshTick);
    refreshTimer_->start();

    connect(filterEdit_, &QLineEdit::textChanged, proxy_,
            &QSortFilterProxyModel::setFilterFixedString);
    connect(resetButton, &QPushButton::clicked, this, [this] {
        if (controller_ != nullptr && !model_->deviceId().isEmpty()) {
            controller_->resetParameters(model_->deviceId());
            model_->reload();
            emit statusMessage(QStringLiteral("已恢复默认值"));
        }
    });
    connect(model_, &ParameterTableModel::writeFailed, this,
            [this](const QString& key, const QString& reason) {
                emit statusMessage(QStringLiteral("写入 %1 失败: %2").arg(key, reason));
            });
}

void ParameterPanel::setController(IDeviceController* controller)
{
    controller_ = controller;
    model_->setController(controller);
    rebuildStateControls();
}

void ParameterPanel::setDevice(const QString& deviceId)
{
    model_->setDevice(deviceId);
    table_->resizeColumnsToContents();
    rebuildStateControls();
}

QString ParameterPanel::deviceId() const
{
    return model_->deviceId();
}

void ParameterPanel::setRefreshIntervalMs(int milliseconds)
{
    refreshTimer_->setInterval(qMax(50, milliseconds));
}

void ParameterPanel::onRefreshTick()
{
    if (!liveBox_->isChecked() || !isVisible()) {
        return;
    }

    model_->refreshValues();

    if (controller_ != nullptr && !model_->deviceId().isEmpty()) {
        stateLabel_->setText(
            QStringLiteral("状态: %1").arg(controller_->currentState(model_->deviceId())));
    }
}

void ParameterPanel::rebuildStateControls()
{
    auto* layout = qobject_cast<QHBoxLayout*>(stateControls_->layout());
    if (layout == nullptr) {
        return;
    }

    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->hide();
            widget->setParent(nullptr);
            widget->deleteLater();
        }
        delete item;
    }

    const QString device = model_->deviceId();
    if (controller_ == nullptr || device.isEmpty()) {
        return;
    }

    const QStringList states = controller_->stateNames(device);
    if (!states.isEmpty()) {
        auto* combo = new QComboBox(stateControls_);
        combo->addItems(states);
        combo->setCurrentText(controller_->currentState(device));

        auto* apply = new QPushButton(QStringLiteral("强制切换"), stateControls_);
        connect(apply, &QPushButton::clicked, this, [this, combo, device] {
            const auto forced = controller_->forceState(device, combo->currentText());
            emit statusMessage(forced.hasValue()
                                   ? QStringLiteral("已切换到 %1").arg(combo->currentText())
                                   : forced.error().toString());
        });

        layout->addWidget(new QLabel(QStringLiteral("目标状态"), stateControls_));
        layout->addWidget(combo);
        layout->addWidget(apply);
    }

    const QStringList events = controller_->eventNames(device);
    if (!events.isEmpty()) {
        auto* combo = new QComboBox(stateControls_);
        combo->addItems(events);

        auto* post = new QPushButton(QStringLiteral("触发事件"), stateControls_);
        connect(post, &QPushButton::clicked, this, [this, combo, device] {
            const auto posted = controller_->postEvent(device, combo->currentText());
            emit statusMessage(posted.hasValue()
                                   ? QStringLiteral("已触发 %1").arg(combo->currentText())
                                   : posted.error().toString());
        });

        layout->addWidget(new QLabel(QStringLiteral("事件"), stateControls_));
        layout->addWidget(combo);
        layout->addWidget(post);
    }

    auto* onlineBox = new QCheckBox(QStringLiteral("在线"), stateControls_);
    onlineBox->setChecked(controller_->isDeviceOnline(device));
    connect(onlineBox, &QCheckBox::toggled, this, [this, device](bool checked) {
        controller_->setDeviceOnline(device, checked);
        emit statusMessage(checked ? QStringLiteral("设备已上线") : QStringLiteral("设备已离线"));
    });
    layout->addWidget(onlineBox);

    layout->addStretch(1);
}

} // namespace hwsim::ui
