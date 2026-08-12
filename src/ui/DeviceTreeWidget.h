#pragma once

#include "UiGlobal.h"

#include <QStandardItemModel>
#include <QVector>
#include <QWidget>

class QLineEdit;
class QSortFilterProxyModel;
class QTreeView;

namespace hwsim::ui {

/// What a tree row refers to. Stored in a custom item role so context menus and
/// selection handling do not have to infer meaning from the row's depth.
enum class DeviceTreeNodeType {
    Workspace,
    Device,
    Transport,
    Protocol,
    ParameterGroup,
    Parameter,
    SignalBinding,
    FaultRule,
    Link,
};

/// Everything the tree shows about one device. The app layer assembles these
/// from its runtime objects, so the tree never reaches into a device that lives
/// on another thread.
struct HWSIM_UI_API DeviceNodeInfo {
    QString deviceId;
    QString name;
    QString protocolDisplayName;
    QString transportDescription;
    QString state;
    bool running{false};
    bool online{true};
    int linkCount{0};

    QStringList parameterGroups;
    QStringList signalBindings;
    QStringList faultRules;
    QStringList linkDescriptions;
};

class HWSIM_UI_API DeviceTreeModel : public QStandardItemModel {
    Q_OBJECT

public:
    enum Roles {
        NodeTypeRole = Qt::UserRole + 1,
        DeviceIdRole,
        PayloadRole,
    };

    explicit DeviceTreeModel(QObject* parent = nullptr);

    void setDevices(const QVector<DeviceNodeInfo>& devices);

    /// Refreshes one device in place, keeping the rest of the tree and the
    /// user's expansion state untouched.
    void updateDevice(const DeviceNodeInfo& device);

    void removeDevice(const QString& deviceId);

    [[nodiscard]] QString deviceIdAt(const QModelIndex& index) const;
    [[nodiscard]] DeviceTreeNodeType nodeTypeAt(const QModelIndex& index) const;
    [[nodiscard]] QVariant payloadAt(const QModelIndex& index) const;

private:
    [[nodiscard]] QStandardItem* findDeviceItem(const QString& deviceId) const;
    void populateDeviceItem(QStandardItem* item, const DeviceNodeInfo& device);
};

/// The project tree: devices, their endpoints, protocol bindings, parameter
/// groups, live signals and armed fault rules, with a filter box on top.
class HWSIM_UI_API DeviceTreeWidget : public QWidget {
    Q_OBJECT

public:
    explicit DeviceTreeWidget(QWidget* parent = nullptr);

    void setDevices(const QVector<DeviceNodeInfo>& devices);
    void updateDevice(const DeviceNodeInfo& device);
    void removeDevice(const QString& deviceId);

    [[nodiscard]] QString selectedDeviceId() const;

signals:
    void deviceSelected(const QString& deviceId);
    void deviceActivated(const QString& deviceId);
    void nodeActivated(const QString& deviceId, hwsim::ui::DeviceTreeNodeType type,
                       const QVariant& payload);
    void contextMenuRequested(const QString& deviceId, hwsim::ui::DeviceTreeNodeType type,
                              const QPoint& globalPosition);

private:
    void onSelectionChanged();
    void onActivated(const QModelIndex& index);
    void onContextMenu(const QPoint& position);

    DeviceTreeModel* model_{nullptr};
    QSortFilterProxyModel* proxy_{nullptr};
    QTreeView* view_{nullptr};
    QLineEdit* filterEdit_{nullptr};
};

} // namespace hwsim::ui

Q_DECLARE_METATYPE(hwsim::ui::DeviceTreeNodeType)
