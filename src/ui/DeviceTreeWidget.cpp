#include "DeviceTreeWidget.h"

#include <QHeaderView>
#include <QLineEdit>
#include <QSortFilterProxyModel>
#include <QTreeView>
#include <QVBoxLayout>

namespace hwsim::ui {
namespace {

QStandardItem* makeItem(const QString& text, DeviceTreeNodeType type, const QString& deviceId,
                        const QVariant& payload = {})
{
    auto* item = new QStandardItem(text);
    item->setEditable(false);
    item->setData(QVariant::fromValue(type), DeviceTreeModel::NodeTypeRole);
    item->setData(deviceId, DeviceTreeModel::DeviceIdRole);
    item->setData(payload, DeviceTreeModel::PayloadRole);
    return item;
}

/// Adds a category row with one child per entry, skipping empty categories so
/// the tree does not fill up with placeholders.
void appendCategory(QStandardItem* parent, const QString& title, const QStringList& entries,
                    DeviceTreeNodeType childType, const QString& deviceId)
{
    if (entries.isEmpty()) {
        return;
    }

    QStandardItem* category =
        makeItem(QStringLiteral("%1 (%2)").arg(title).arg(entries.size()),
                 DeviceTreeNodeType::ParameterGroup, deviceId);

    for (const QString& entry : entries) {
        category->appendRow(makeItem(entry, childType, deviceId, entry));
    }
    parent->appendRow(category);
}

QString describeDevice(const DeviceNodeInfo& device)
{
    QStringList parts;
    if (!device.online) {
        parts.append(QStringLiteral("离线"));
    } else if (!device.running) {
        parts.append(QStringLiteral("已停止"));
    } else if (!device.state.isEmpty()) {
        parts.append(device.state);
    }
    if (device.linkCount > 0) {
        parts.append(QStringLiteral("%1 条链路").arg(device.linkCount));
    }

    return parts.isEmpty() ? device.name
                           : QStringLiteral("%1  ·  %2").arg(device.name, parts.join(QStringLiteral(" / ")));
}

} // namespace

// --- DeviceTreeModel -------------------------------------------------------

DeviceTreeModel::DeviceTreeModel(QObject* parent) : QStandardItemModel(parent)
{
    setHorizontalHeaderLabels({QStringLiteral("工程")});
}

void DeviceTreeModel::setDevices(const QVector<DeviceNodeInfo>& devices)
{
    clear();
    setHorizontalHeaderLabels({QStringLiteral("工程")});

    for (const DeviceNodeInfo& device : devices) {
        QStandardItem* item = makeItem(describeDevice(device), DeviceTreeNodeType::Device,
                                       device.deviceId);
        populateDeviceItem(item, device);
        invisibleRootItem()->appendRow(item);
    }
}

void DeviceTreeModel::updateDevice(const DeviceNodeInfo& device)
{
    QStandardItem* item = findDeviceItem(device.deviceId);
    if (item == nullptr) {
        item = makeItem(describeDevice(device), DeviceTreeNodeType::Device, device.deviceId);
        populateDeviceItem(item, device);
        invisibleRootItem()->appendRow(item);
        return;
    }

    item->setText(describeDevice(device));
    item->removeRows(0, item->rowCount());
    populateDeviceItem(item, device);
}

void DeviceTreeModel::removeDevice(const QString& deviceId)
{
    if (QStandardItem* item = findDeviceItem(deviceId); item != nullptr) {
        invisibleRootItem()->removeRow(item->row());
    }
}

QStandardItem* DeviceTreeModel::findDeviceItem(const QString& deviceId) const
{
    for (int row = 0; row < invisibleRootItem()->rowCount(); ++row) {
        QStandardItem* item = invisibleRootItem()->child(row);
        if (item != nullptr && item->data(DeviceIdRole).toString() == deviceId) {
            return item;
        }
    }
    return nullptr;
}

void DeviceTreeModel::populateDeviceItem(QStandardItem* item, const DeviceNodeInfo& device)
{
    if (!device.transportDescription.isEmpty()) {
        item->appendRow(makeItem(QStringLiteral("通讯: %1").arg(device.transportDescription),
                                 DeviceTreeNodeType::Transport, device.deviceId));
    }
    if (!device.protocolDisplayName.isEmpty()) {
        item->appendRow(makeItem(QStringLiteral("协议: %1").arg(device.protocolDisplayName),
                                 DeviceTreeNodeType::Protocol, device.deviceId));
    }

    appendCategory(item, QStringLiteral("链路"), device.linkDescriptions, DeviceTreeNodeType::Link,
                   device.deviceId);
    appendCategory(item, QStringLiteral("参数组"), device.parameterGroups,
                   DeviceTreeNodeType::ParameterGroup, device.deviceId);
    appendCategory(item, QStringLiteral("信号源"), device.signalBindings,
                   DeviceTreeNodeType::SignalBinding, device.deviceId);
    appendCategory(item, QStringLiteral("故障注入"), device.faultRules,
                   DeviceTreeNodeType::FaultRule, device.deviceId);
}

QString DeviceTreeModel::deviceIdAt(const QModelIndex& index) const
{
    return index.data(DeviceIdRole).toString();
}

DeviceTreeNodeType DeviceTreeModel::nodeTypeAt(const QModelIndex& index) const
{
    const QVariant value = index.data(NodeTypeRole);
    return value.isValid() ? value.value<DeviceTreeNodeType>() : DeviceTreeNodeType::Workspace;
}

QVariant DeviceTreeModel::payloadAt(const QModelIndex& index) const
{
    return index.data(PayloadRole);
}

// --- DeviceTreeWidget ------------------------------------------------------

DeviceTreeWidget::DeviceTreeWidget(QWidget* parent) : QWidget(parent)
{
    model_ = new DeviceTreeModel(this);

    proxy_ = new QSortFilterProxyModel(this);
    proxy_->setSourceModel(model_);
    proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    proxy_->setRecursiveFilteringEnabled(true);

    filterEdit_ = new QLineEdit(this);
    filterEdit_->setPlaceholderText(QStringLiteral("过滤设备 / 参数..."));
    filterEdit_->setClearButtonEnabled(true);

    view_ = new QTreeView(this);
    view_->setModel(proxy_);
    view_->setHeaderHidden(false);
    view_->setUniformRowHeights(true);
    view_->setAlternatingRowColors(true);
    view_->setContextMenuPolicy(Qt::CustomContextMenu);
    view_->header()->setSectionResizeMode(QHeaderView::Stretch);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);
    layout->addWidget(filterEdit_);
    layout->addWidget(view_, 1);

    connect(filterEdit_, &QLineEdit::textChanged, proxy_,
            &QSortFilterProxyModel::setFilterFixedString);
    connect(view_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            &DeviceTreeWidget::onSelectionChanged);
    connect(view_, &QTreeView::activated, this, &DeviceTreeWidget::onActivated);
    connect(view_, &QTreeView::customContextMenuRequested, this, &DeviceTreeWidget::onContextMenu);
}

void DeviceTreeWidget::setDevices(const QVector<DeviceNodeInfo>& devices)
{
    model_->setDevices(devices);
    view_->expandToDepth(0);
}

void DeviceTreeWidget::updateDevice(const DeviceNodeInfo& device)
{
    model_->updateDevice(device);
}

void DeviceTreeWidget::removeDevice(const QString& deviceId)
{
    model_->removeDevice(deviceId);
}

QString DeviceTreeWidget::selectedDeviceId() const
{
    const QModelIndexList selected = view_->selectionModel()->selectedIndexes();
    if (selected.isEmpty()) {
        return {};
    }
    return model_->deviceIdAt(proxy_->mapToSource(selected.first()));
}

void DeviceTreeWidget::onSelectionChanged()
{
    const QString deviceId = selectedDeviceId();
    if (!deviceId.isEmpty()) {
        emit deviceSelected(deviceId);
    }
}

void DeviceTreeWidget::onActivated(const QModelIndex& index)
{
    const QModelIndex source = proxy_->mapToSource(index);
    const QString deviceId = model_->deviceIdAt(source);
    const DeviceTreeNodeType type = model_->nodeTypeAt(source);

    emit nodeActivated(deviceId, type, model_->payloadAt(source));
    if (type == DeviceTreeNodeType::Device) {
        emit deviceActivated(deviceId);
    }
}

void DeviceTreeWidget::onContextMenu(const QPoint& position)
{
    const QModelIndex index = view_->indexAt(position);
    if (!index.isValid()) {
        emit contextMenuRequested({}, DeviceTreeNodeType::Workspace,
                                  view_->viewport()->mapToGlobal(position));
        return;
    }

    const QModelIndex source = proxy_->mapToSource(index);
    emit contextMenuRequested(model_->deviceIdAt(source), model_->nodeTypeAt(source),
                              view_->viewport()->mapToGlobal(position));
}

} // namespace hwsim::ui
