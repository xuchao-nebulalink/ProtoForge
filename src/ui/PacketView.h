#pragma once

#include "UiGlobal.h"

#include <core/EventBus.h>
#include <protocol/ProtocolEvents.h>

#include <QAbstractTableModel>
#include <QSortFilterProxyModel>
#include <QVector>
#include <QWidget>

#include <deque>
#include <memory>
#include <mutex>
#include <vector>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QTableView;
class QTimer;
class QTreeWidget;

namespace hwsim::ui {

/// One row of the packet table.
struct HWSIM_UI_API PacketRecord {
    quint64 sequence{0};
    qint64 timestampMs{0};
    QString device;
    QString session;
    quint64 linkId{0};
    transport::Direction direction{transport::Direction::Inbound};
    protocol::OpCode opcode{0};
    QByteArray raw;
    QString description;
    QString annotation;
    bool decoded{false};

    /// False for an outbound frame a fault rule discarded before transmission.
    bool delivered{true};
};

class HWSIM_UI_API PacketModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column {
        SequenceColumn = 0,
        TimeColumn,
        DirectionColumn,
        DeviceColumn,
        LinkColumn,
        OpcodeColumn,
        LengthColumn,
        DescriptionColumn,
        AnnotationColumn,
        ColumnCount,
    };

    explicit PacketModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                      int role) const override;

    void append(const QVector<PacketRecord>& records);
    void clear();

    [[nodiscard]] const PacketRecord* recordAt(int row) const;

    void setCapacity(int rows);
    [[nodiscard]] int capacity() const noexcept { return capacity_; }

private:
    void trim();

    std::deque<PacketRecord> records_;
    int capacity_{20000};
};

/// Applies the toolbar filters to the whole capture, not just to newly arriving
/// frames, so tightening a filter never hides evidence that was already there.
class HWSIM_UI_API PacketFilterProxy : public QSortFilterProxyModel {
    Q_OBJECT

public:
    explicit PacketFilterProxy(QObject* parent = nullptr);

    void setDirectionFilter(int direction);
    void setDeviceFilter(const QString& text);
    void setTextFilter(const QString& text);

protected:
    [[nodiscard]] bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
    int direction_{-1};
    QString device_;
    QString text_;
};

/// Traffic view: a scrolling table of frames plus a detail pane showing the
/// same frame as hex, as text, and as its decoded fields.
///
/// The rows come from ProtocolFrameEvent, which the session publishes after the
/// outbound fault filters have run, so what is displayed is what actually went
/// on the wire, corruption included.
class HWSIM_UI_API PacketView : public QWidget {
    Q_OBJECT

public:
    explicit PacketView(QWidget* parent = nullptr);
    ~PacketView() override;

    /// Subscribes to frame and error events on `bus`.
    void attachToEventBus(core::EventBus* bus);
    void detachFromEventBus();

public slots:
    void clearPackets();
    void exportToFile();
    void setPaused(bool paused);

signals:
    /// Emitted by the manual send box, for the app layer to route to a session.
    void manualSendRequested(const QString& deviceId, const QByteArray& bytes);

private:
    void flushPending();
    void onSelectionChanged();
    void updateDetail(const PacketRecord& record);

    PacketModel* model_{nullptr};
    PacketFilterProxy* proxy_{nullptr};
    QTableView* table_{nullptr};
    QPlainTextEdit* hexView_{nullptr};
    QPlainTextEdit* asciiView_{nullptr};
    QTreeWidget* parseView_{nullptr};

    QComboBox* directionFilter_{nullptr};
    QLineEdit* deviceFilter_{nullptr};
    QLineEdit* textFilter_{nullptr};
    QCheckBox* pauseBox_{nullptr};
    QCheckBox* followBox_{nullptr};

    QLineEdit* sendDeviceEdit_{nullptr};
    QLineEdit* sendHexEdit_{nullptr};

    /// Staging buffer shared with the subscription lambdas.
    ///
    /// EventBus releases its lock before invoking handlers, so a handler can
    /// still be running when unsubscribe() returns. Capturing a shared_ptr to
    /// this instead of `this` means a late delivery writes into an object that
    /// is guaranteed to outlive it, rather than into a destroyed widget.
    struct Staging {
        std::mutex mutex;
        std::vector<PacketRecord> records;
    };

    QTimer* flushTimer_{nullptr};
    std::shared_ptr<Staging> staging_{std::make_shared<Staging>()};

    core::EventBus* bus_{nullptr};
    core::SubscriptionId frameSubscription_{0};
    core::SubscriptionId errorSubscription_{0};

    quint64 nextSequence_{1};
    bool paused_{false};
};

} // namespace hwsim::ui
