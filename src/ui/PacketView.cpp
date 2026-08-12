#include "PacketView.h"

#include <core/Clock.h>
#include <core/HexUtils.h>

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSplitter>
#include <QTabWidget>
#include <QTableView>
#include <QTextStream>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace hwsim::ui {
namespace {

constexpr int kFlushIntervalMs = 100;

const QColor& inboundColour()
{
    static const QColor colour(0x1e, 0x3a, 0x28);
    return colour;
}

const QColor& outboundColour()
{
    static const QColor colour(0x1e, 0x2c, 0x3a);
    return colour;
}

const QColor& faultColour()
{
    static const QColor colour(0x3a, 0x24, 0x1e);
    return colour;
}

} // namespace

// --- PacketModel -----------------------------------------------------------

PacketModel::PacketModel(QObject* parent) : QAbstractTableModel(parent) {}

int PacketModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(records_.size());
}

int PacketModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant PacketModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= static_cast<int>(records_.size())) {
        return {};
    }

    const PacketRecord& record = records_.at(static_cast<std::size_t>(index.row()));

    if (role == Qt::BackgroundRole) {
        if (!record.annotation.isEmpty() || !record.decoded) {
            return faultColour();
        }
        return record.direction == transport::Direction::Inbound ? inboundColour()
                                                                 : outboundColour();
    }

    if (role == Qt::ToolTipRole) {
        return core::hex::dump(record.raw);
    }

    if (role != Qt::DisplayRole) {
        return {};
    }

    if (index.column() == SequenceColumn) return record.sequence;
    if (index.column() == TimeColumn) return core::formatWallClock(record.timestampMs);
    if (index.column() == DirectionColumn) return transport::directionName(record.direction);
    if (index.column() == DeviceColumn) {
        return record.device.isEmpty() ? record.session : record.device;
    }
    if (index.column() == LinkColumn) return record.linkId;
    if (index.column() == OpcodeColumn) {
        return QStringLiteral("0x%1").arg(record.opcode, 2, 16, QLatin1Char('0'));
    }
    if (index.column() == LengthColumn) return record.raw.size();
    if (index.column() == DescriptionColumn) {
        return record.decoded ? record.description : QStringLiteral("<解析失败>");
    }
    if (index.column() == AnnotationColumn) return record.annotation;

    return {};
}

QVariant PacketModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }

    static const QStringList headers{
        QStringLiteral("#"),       QStringLiteral("时间"),   QStringLiteral("方向"),
        QStringLiteral("设备"),    QStringLiteral("链路"),   QStringLiteral("功能码"),
        QStringLiteral("长度"),    QStringLiteral("解析"),   QStringLiteral("备注"),
    };
    return section < headers.size() ? headers.at(section) : QVariant{};
}

void PacketModel::append(const QVector<PacketRecord>& records)
{
    if (records.isEmpty()) {
        return;
    }

    const int first = static_cast<int>(records_.size());
    beginInsertRows({}, first, first + static_cast<int>(records.size()) - 1);
    for (const PacketRecord& record : records) {
        records_.push_back(record);
    }
    endInsertRows();

    trim();
}

void PacketModel::trim()
{
    if (static_cast<int>(records_.size()) <= capacity_) {
        return;
    }

    const int excess = static_cast<int>(records_.size()) - capacity_;
    beginRemoveRows({}, 0, excess - 1);
    records_.erase(records_.begin(), records_.begin() + excess);
    endRemoveRows();
}

void PacketModel::clear()
{
    beginResetModel();
    records_.clear();
    endResetModel();
}

const PacketRecord* PacketModel::recordAt(int row) const
{
    if (row < 0 || row >= static_cast<int>(records_.size())) {
        return nullptr;
    }
    return &records_.at(static_cast<std::size_t>(row));
}

void PacketModel::setCapacity(int rows)
{
    capacity_ = qMax(100, rows);
    trim();
}

// --- PacketFilterProxy -----------------------------------------------------

PacketFilterProxy::PacketFilterProxy(QObject* parent) : QSortFilterProxyModel(parent) {}

void PacketFilterProxy::setDirectionFilter(int direction)
{
    direction_ = direction;
    invalidateFilter();
}

void PacketFilterProxy::setDeviceFilter(const QString& text)
{
    device_ = text.trimmed();
    invalidateFilter();
}

void PacketFilterProxy::setTextFilter(const QString& text)
{
    text_ = text.trimmed();
    invalidateFilter();
}

bool PacketFilterProxy::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const
{
    const auto* source = qobject_cast<const PacketModel*>(sourceModel());
    if (source == nullptr) {
        return true;
    }
    Q_UNUSED(sourceParent)

    const PacketRecord* record = source->recordAt(sourceRow);
    if (record == nullptr) {
        return false;
    }

    if (direction_ >= 0 && static_cast<int>(record->direction) != direction_) {
        return false;
    }

    if (!device_.isEmpty() && !record->device.contains(device_, Qt::CaseInsensitive)
        && !record->session.contains(device_, Qt::CaseInsensitive)) {
        return false;
    }

    if (!text_.isEmpty()) {
        const bool inDescription = record->description.contains(text_, Qt::CaseInsensitive);
        const bool inAnnotation = record->annotation.contains(text_, Qt::CaseInsensitive);
        const bool inHex = core::hex::toHex(record->raw).contains(text_, Qt::CaseInsensitive);
        if (!inDescription && !inAnnotation && !inHex) {
            return false;
        }
    }

    return true;
}

// --- PacketView ------------------------------------------------------------

PacketView::PacketView(QWidget* parent) : QWidget(parent)
{
    model_ = new PacketModel(this);

    proxy_ = new PacketFilterProxy(this);
    proxy_->setSourceModel(model_);

    table_ = new QTableView(this);
    table_->setModel(proxy_);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setShowGrid(false);
    table_->setFont(QFont(QStringLiteral("Consolas"), 9));

    hexView_ = new QPlainTextEdit(this);
    hexView_->setReadOnly(true);
    hexView_->setFont(QFont(QStringLiteral("Consolas"), 9));

    asciiView_ = new QPlainTextEdit(this);
    asciiView_->setReadOnly(true);
    asciiView_->setFont(QFont(QStringLiteral("Consolas"), 9));

    parseView_ = new QTreeWidget(this);
    parseView_->setColumnCount(2);
    parseView_->setHeaderLabels({QStringLiteral("字段"), QStringLiteral("值")});

    auto* detailTabs = new QTabWidget(this);
    detailTabs->addTab(hexView_, QStringLiteral("十六进制"));
    detailTabs->addTab(asciiView_, QStringLiteral("文本"));
    detailTabs->addTab(parseView_, QStringLiteral("解析"));

    directionFilter_ = new QComboBox(this);
    directionFilter_->addItem(QStringLiteral("全部方向"), -1);
    directionFilter_->addItem(QStringLiteral("仅接收"),
                              static_cast<int>(transport::Direction::Inbound));
    directionFilter_->addItem(QStringLiteral("仅发送"),
                              static_cast<int>(transport::Direction::Outbound));

    deviceFilter_ = new QLineEdit(this);
    deviceFilter_->setPlaceholderText(QStringLiteral("设备名过滤"));
    deviceFilter_->setClearButtonEnabled(true);

    textFilter_ = new QLineEdit(this);
    textFilter_->setPlaceholderText(QStringLiteral("解析内容 / 十六进制搜索"));
    textFilter_->setClearButtonEnabled(true);

    pauseBox_ = new QCheckBox(QStringLiteral("暂停"), this);
    followBox_ = new QCheckBox(QStringLiteral("自动滚动"), this);
    followBox_->setChecked(true);

    auto* clearButton = new QPushButton(QStringLiteral("清空"), this);
    auto* exportButton = new QPushButton(QStringLiteral("导出"), this);

    auto* toolbar = new QHBoxLayout;
    toolbar->setContentsMargins(0, 0, 0, 0);
    toolbar->addWidget(directionFilter_);
    toolbar->addWidget(deviceFilter_, 1);
    toolbar->addWidget(textFilter_, 1);
    toolbar->addWidget(pauseBox_);
    toolbar->addWidget(followBox_);
    toolbar->addWidget(clearButton);
    toolbar->addWidget(exportButton);

    // Manual send: the escape hatch for poking a device with a hand-built frame.
    sendDeviceEdit_ = new QLineEdit(this);
    sendDeviceEdit_->setPlaceholderText(QStringLiteral("设备"));
    sendDeviceEdit_->setMaximumWidth(140);

    sendHexEdit_ = new QLineEdit(this);
    sendHexEdit_->setPlaceholderText(QStringLiteral("01 03 00 00 00 0A  (会自动补齐组帧与校验之外的原始字节)"));

    auto* sendButton = new QPushButton(QStringLiteral("发送"), this);

    auto* sendBox = new QGroupBox(QStringLiteral("手动发送"), this);
    auto* sendLayout = new QHBoxLayout(sendBox);
    sendLayout->addWidget(sendDeviceEdit_);
    sendLayout->addWidget(sendHexEdit_, 1);
    sendLayout->addWidget(sendButton);

    auto* splitter = new QSplitter(Qt::Vertical, this);
    splitter->addWidget(table_);
    splitter->addWidget(detailTabs);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);
    layout->addLayout(toolbar);
    layout->addWidget(splitter, 1);
    layout->addWidget(sendBox);

    flushTimer_ = new QTimer(this);
    flushTimer_->setInterval(kFlushIntervalMs);
    connect(flushTimer_, &QTimer::timeout, this, &PacketView::flushPending);
    flushTimer_->start();

    connect(clearButton, &QPushButton::clicked, this, &PacketView::clearPackets);
    connect(exportButton, &QPushButton::clicked, this, &PacketView::exportToFile);
    connect(pauseBox_, &QCheckBox::toggled, this, &PacketView::setPaused);
    connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            &PacketView::onSelectionChanged);

    connect(directionFilter_, &QComboBox::currentIndexChanged, this, [this] {
        proxy_->setDirectionFilter(directionFilter_->currentData().toInt());
    });
    connect(deviceFilter_, &QLineEdit::textChanged, proxy_, &PacketFilterProxy::setDeviceFilter);
    connect(textFilter_, &QLineEdit::textChanged, proxy_, &PacketFilterProxy::setTextFilter);

    connect(sendButton, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const QByteArray bytes = core::hex::fromHex(sendHexEdit_->text(), &ok);
        if (!ok || bytes.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("无法发送"),
                                 QStringLiteral("请输入合法的十六进制字节序列。"));
            return;
        }
        emit manualSendRequested(sendDeviceEdit_->text().trimmed(), bytes);
    });
}

PacketView::~PacketView()
{
    detachFromEventBus();
}

void PacketView::attachToEventBus(core::EventBus* bus)
{
    detachFromEventBus();
    bus_ = bus;
    if (bus_ == nullptr) {
        return;
    }

    // No QObject context on purpose: events arrive on device I/O threads and
    // are staged into a buffer that the UI timer drains, which is far cheaper
    // than one queued invocation per frame under load.
    //
    // The lambdas capture the staging buffer by shared_ptr rather than
    // capturing `this`, because EventBus drops its lock before dispatching and
    // a handler may therefore still be running after detachFromEventBus().
    frameSubscription_ = bus_->subscribe<protocol::ProtocolFrameEvent>(
        [staging = staging_](const protocol::ProtocolFrameEvent& event) {
            PacketRecord record;
            record.timestampMs = event.timestampMs;
            record.device = event.deviceName;
            record.session = event.sessionName;
            record.linkId = event.linkId;
            record.direction = event.direction;
            record.opcode = event.opcode;
            record.raw = event.raw;
            record.description = event.messageDescription;
            record.annotation = event.annotation;
            record.decoded = event.decoded;

            std::lock_guard lock(staging->mutex);
            staging->records.push_back(std::move(record));
        });

    errorSubscription_ = bus_->subscribe<protocol::ProtocolErrorEvent>(
        [staging = staging_](const protocol::ProtocolErrorEvent& event) {
            PacketRecord record;
            record.timestampMs = event.timestampMs;
            record.session = event.sessionName;
            record.linkId = event.linkId;
            record.raw = event.raw;
            record.annotation = event.error.toString();
            record.decoded = false;

            std::lock_guard lock(staging->mutex);
            staging->records.push_back(std::move(record));
        });
}

void PacketView::detachFromEventBus()
{
    if (bus_ == nullptr) {
        return;
    }
    if (frameSubscription_ != 0) {
        bus_->unsubscribe(frameSubscription_);
        frameSubscription_ = 0;
    }
    if (errorSubscription_ != 0) {
        bus_->unsubscribe(errorSubscription_);
        errorSubscription_ = 0;
    }
    bus_ = nullptr;
}

void PacketView::setPaused(bool paused)
{
    paused_ = paused;
    if (pauseBox_->isChecked() != paused) {
        pauseBox_->setChecked(paused);
    }
}

void PacketView::clearPackets()
{
    model_->clear();
    hexView_->clear();
    asciiView_->clear();
    parseView_->clear();
}

void PacketView::flushPending()
{
    if (paused_) {
        // Leave the frames in the staging buffer instead of draining and
        // discarding them, so unpausing shows what arrived during the pause.
        // Bounded so that a long pause under load cannot exhaust memory.
        constexpr std::size_t kPausedBufferLimit = 100000;

        std::lock_guard lock(staging_->mutex);
        if (staging_->records.size() > kPausedBufferLimit) {
            const auto excess = staging_->records.size() - kPausedBufferLimit;
            staging_->records.erase(staging_->records.begin(),
                                    staging_->records.begin() + static_cast<std::ptrdiff_t>(excess));
        }
        return;
    }

    std::vector<PacketRecord> batch;
    {
        std::lock_guard lock(staging_->mutex);
        if (staging_->records.empty()) {
            return;
        }
        batch.swap(staging_->records);
    }

    QVector<PacketRecord> accepted;
    accepted.reserve(static_cast<qsizetype>(batch.size()));
    for (PacketRecord& record : batch) {
        record.sequence = nextSequence_++;
        accepted.append(std::move(record));
    }

    model_->append(accepted);

    if (followBox_->isChecked()) {
        table_->scrollToBottom();
    }
}

void PacketView::onSelectionChanged()
{
    const QModelIndexList selected = table_->selectionModel()->selectedRows();
    if (selected.isEmpty()) {
        return;
    }

    const QModelIndex source = proxy_->mapToSource(selected.first());
    if (const PacketRecord* record = model_->recordAt(source.row()); record != nullptr) {
        updateDetail(*record);
    }
}

void PacketView::updateDetail(const PacketRecord& record)
{
    hexView_->setPlainText(core::hex::dump(record.raw));
    asciiView_->setPlainText(core::hex::toPrintableAscii(core::hex::asBytes(record.raw)));

    parseView_->clear();
    const auto addRow = [this](const QString& name, const QString& value) {
        auto* item = new QTreeWidgetItem(parseView_);
        item->setText(0, name);
        item->setText(1, value);
    };

    addRow(QStringLiteral("时间"), core::formatWallClock(record.timestampMs, true));
    addRow(QStringLiteral("方向"), transport::directionName(record.direction));
    addRow(QStringLiteral("设备"), record.device);
    addRow(QStringLiteral("会话"), record.session);
    addRow(QStringLiteral("链路"), QString::number(record.linkId));
    addRow(QStringLiteral("功能码"),
           QStringLiteral("0x%1").arg(record.opcode, 2, 16, QLatin1Char('0')));
    addRow(QStringLiteral("长度"), QStringLiteral("%1 字节").arg(record.raw.size()));
    addRow(QStringLiteral("解析"),
           record.decoded ? record.description : QStringLiteral("<解析失败>"));
    if (!record.annotation.isEmpty()) {
        addRow(QStringLiteral("备注"), record.annotation);
    }

    parseView_->resizeColumnToContents(0);
}

void PacketView::exportToFile()
{
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出报文"), QStringLiteral("hwsim-packets.csv"),
        QStringLiteral("CSV 文件 (*.csv);;所有文件 (*)"));
    if (path.isEmpty()) {
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), file.errorString());
        return;
    }

    QTextStream stream(&file);
    stream << "sequence,timestamp,direction,device,session,link,opcode,length,hex,description,annotation\n";

    for (int row = 0; row < model_->rowCount(); ++row) {
        const PacketRecord* record = model_->recordAt(row);
        if (record == nullptr) {
            continue;
        }
        stream << record->sequence << ','
               << core::formatWallClock(record->timestampMs, true) << ','
               << transport::directionName(record->direction) << ','
               << record->device << ',' << record->session << ',' << record->linkId << ','
               << QStringLiteral("0x%1").arg(record->opcode, 2, 16, QLatin1Char('0')) << ','
               << record->raw.size() << ','
               << core::hex::toHex(record->raw, QChar()) << ','
               << QStringLiteral("\"%1\"").arg(
                      QString(record->description).replace(QLatin1Char('"'), QLatin1Char('\'')))
               << ','
               << QStringLiteral("\"%1\"").arg(
                      QString(record->annotation).replace(QLatin1Char('"'), QLatin1Char('\'')))
               << '\n';
    }
}

} // namespace hwsim::ui
