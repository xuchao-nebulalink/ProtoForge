#pragma once

#include "IDeviceController.h"
#include "UiGlobal.h"

#include <core/EventBus.h>

#include <QAbstractTableModel>
#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QSortFilterProxyModel;
class QTableView;
class QTimer;

namespace hwsim::ui {

/// Table model over one device's parameters.
///
/// The value column is editable, and edits go through IDeviceController rather
/// than straight into the store. That matters because the operator writing a
/// value is a different origin from the protocol writing one: a read-only
/// sensor register must stay writable here while remaining read-only on the wire.
class HWSIM_UI_API ParameterTableModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column {
        NameColumn = 0,
        AddressColumn,
        TypeColumn,
        AccessColumn,
        ValueColumn,
        UnitColumn,
        GroupColumn,
        ColumnCount,
    };

    explicit ParameterTableModel(QObject* parent = nullptr);

    void setController(IDeviceController* controller);
    void setDevice(const QString& deviceId);
    [[nodiscard]] QString deviceId() const { return deviceId_; }

    /// Pulls fresh values without rebuilding the table, so the selection and
    /// any in-progress edit survive.
    void refreshValues();
    void reload();

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] bool setData(const QModelIndex& index, const QVariant& value, int role) override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                      int role) const override;

signals:
    void writeFailed(const QString& key, const QString& reason);

private:
    IDeviceController* controller_{nullptr};
    QString deviceId_;
    QVector<simulator::ParameterDefinition> definitions_;
    QVariantMap values_;
};

/// Parameter configuration panel: a searchable, live-updating register table
/// with inline editing, plus the device's state controls.
class HWSIM_UI_API ParameterPanel : public QWidget {
    Q_OBJECT

public:
    explicit ParameterPanel(QWidget* parent = nullptr);

    void setController(IDeviceController* controller);
    void setDevice(const QString& deviceId);
    [[nodiscard]] QString deviceId() const;

    /// Refreshes values on a timer while visible.
    void setRefreshIntervalMs(int milliseconds);

signals:
    void statusMessage(const QString& message);

private:
    void rebuildStateControls();
    void onRefreshTick();

    IDeviceController* controller_{nullptr};
    ParameterTableModel* model_{nullptr};
    QSortFilterProxyModel* proxy_{nullptr};
    QTableView* table_{nullptr};
    QLineEdit* filterEdit_{nullptr};
    QCheckBox* liveBox_{nullptr};
    QLabel* stateLabel_{nullptr};
    QWidget* stateControls_{nullptr};
    QTimer* refreshTimer_{nullptr};
};

} // namespace hwsim::ui
