#include "LogDockWidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>

namespace hwsim::ui {
namespace {

constexpr int kFlushIntervalMs = 120;

QColor colourFor(core::LogLevel level)
{
    static const QHash<core::LogLevel, QColor> colours{
        {core::LogLevel::Trace, QColor(0x88, 0x88, 0x88)},
        {core::LogLevel::Debug, QColor(0x4e, 0x9a, 0xd0)},
        {core::LogLevel::Info, QColor(0xd0, 0xd0, 0xd0)},
        {core::LogLevel::Warning, QColor(0xe0, 0xa0, 0x30)},
        {core::LogLevel::Error, QColor(0xe0, 0x50, 0x50)},
        {core::LogLevel::Critical, QColor(0xff, 0x40, 0x80)},
    };
    return colours.value(level, QColor(0xd0, 0xd0, 0xd0));
}

} // namespace

LogDockWidget::LogDockWidget(QWidget* parent) : QWidget(parent)
{
    levelFilter_ = new QComboBox(this);
    for (int level = static_cast<int>(core::LogLevel::Trace);
         level <= static_cast<int>(core::LogLevel::Critical); ++level) {
        levelFilter_->addItem(core::logLevelName(static_cast<core::LogLevel>(level)), level);
    }
    levelFilter_->setCurrentIndex(static_cast<int>(core::LogLevel::Debug));

    categoryFilter_ = new QLineEdit(this);
    categoryFilter_->setPlaceholderText(QStringLiteral("分类，如 transport.tcp"));
    categoryFilter_->setClearButtonEnabled(true);

    searchFilter_ = new QLineEdit(this);
    searchFilter_->setPlaceholderText(QStringLiteral("搜索内容..."));
    searchFilter_->setClearButtonEnabled(true);

    pauseBox_ = new QCheckBox(QStringLiteral("暂停"), this);
    followBox_ = new QCheckBox(QStringLiteral("自动滚动"), this);
    followBox_->setChecked(true);

    auto* clearButton = new QPushButton(QStringLiteral("清空"), this);
    auto* exportButton = new QPushButton(QStringLiteral("导出"), this);

    auto* toolbar = new QHBoxLayout;
    toolbar->setContentsMargins(0, 0, 0, 0);
    toolbar->addWidget(new QLabel(QStringLiteral("级别"), this));
    toolbar->addWidget(levelFilter_);
    toolbar->addWidget(categoryFilter_, 1);
    toolbar->addWidget(searchFilter_, 1);
    toolbar->addWidget(pauseBox_);
    toolbar->addWidget(followBox_);
    toolbar->addWidget(clearButton);
    toolbar->addWidget(exportButton);

    output_ = new QPlainTextEdit(this);
    output_->setReadOnly(true);
    output_->setMaximumBlockCount(maximumLines_);
    output_->setLineWrapMode(QPlainTextEdit::NoWrap);
    output_->setFont(QFont(QStringLiteral("Consolas"), 9));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);
    layout->addLayout(toolbar);
    layout->addWidget(output_, 1);

    flushTimer_ = new QTimer(this);
    flushTimer_->setInterval(kFlushIntervalMs);
    connect(flushTimer_, &QTimer::timeout, this, &LogDockWidget::flushPending);
    flushTimer_->start();

    connect(clearButton, &QPushButton::clicked, this, &LogDockWidget::clearLog);
    connect(exportButton, &QPushButton::clicked, this, &LogDockWidget::exportToFile);
    connect(pauseBox_, &QCheckBox::toggled, this, &LogDockWidget::setPaused);
    connect(levelFilter_, &QComboBox::currentIndexChanged, this, &LogDockWidget::rebuildView);
    connect(categoryFilter_, &QLineEdit::textChanged, this, &LogDockWidget::rebuildView);
    connect(searchFilter_, &QLineEdit::textChanged, this, &LogDockWidget::rebuildView);
}

LogDockWidget::~LogDockWidget()
{
    detachFromLogger();
}

void LogDockWidget::attachToLogger(std::shared_ptr<core::RingBufferLogSink> history)
{
    detachFromLogger();

    if (history) {
        for (const core::LogRecord& record : history->snapshot()) {
            records_.append(record);
        }
        rebuildView();
    }

    // The sink runs on the logger's worker thread; it only stages records.
    sinkId_ = core::Logger::instance().addSink(
        std::make_shared<core::FunctionLogSink>([this](const core::LogRecord& record) {
            std::lock_guard lock(pendingMutex_);
            pending_.push_back(record);
        }));
}

void LogDockWidget::detachFromLogger()
{
    if (sinkId_ != 0) {
        core::Logger::instance().removeSink(sinkId_);
        sinkId_ = 0;
    }
}

void LogDockWidget::setMaximumLines(int lines)
{
    maximumLines_ = qMax(100, lines);
    output_->setMaximumBlockCount(maximumLines_);
}

void LogDockWidget::setPaused(bool paused)
{
    paused_ = paused;
    if (pauseBox_->isChecked() != paused) {
        pauseBox_->setChecked(paused);
    }
}

void LogDockWidget::clearLog()
{
    records_.clear();
    output_->clear();
}

void LogDockWidget::flushPending()
{
    std::vector<core::LogRecord> batch;
    {
        std::lock_guard lock(pendingMutex_);
        if (pending_.empty()) {
            return;
        }
        batch.swap(pending_);
    }

    // Records keep accumulating while paused so nothing is lost; only the
    // rendering stops.
    for (const core::LogRecord& record : batch) {
        records_.append(record);
    }
    while (records_.size() > maximumLines_ * 2) {
        records_.removeFirst();
    }

    if (paused_) {
        return;
    }

    const bool atBottom =
        output_->verticalScrollBar()->value() >= output_->verticalScrollBar()->maximum() - 4;

    for (const core::LogRecord& record : batch) {
        if (passesFilter(record)) {
            appendRecord(record);
        }
    }

    if (followBox_->isChecked() || atBottom) {
        output_->verticalScrollBar()->setValue(output_->verticalScrollBar()->maximum());
    }
}

void LogDockWidget::appendRecord(const core::LogRecord& record)
{
    QTextCharFormat format;
    format.setForeground(colourFor(record.level));

    QTextCursor cursor(output_->document());
    cursor.movePosition(QTextCursor::End);
    if (!output_->document()->isEmpty()) {
        cursor.insertBlock();
    }
    cursor.insertText(record.formatted(), format);
}

bool LogDockWidget::passesFilter(const core::LogRecord& record) const
{
    const auto minimumLevel = static_cast<core::LogLevel>(levelFilter_->currentData().toInt());
    if (record.level < minimumLevel) {
        return false;
    }

    const QString category = categoryFilter_->text().trimmed();
    if (!category.isEmpty() && !record.category.contains(category, Qt::CaseInsensitive)) {
        return false;
    }

    const QString needle = searchFilter_->text().trimmed();
    if (!needle.isEmpty() && !record.message.contains(needle, Qt::CaseInsensitive)) {
        return false;
    }

    return true;
}

void LogDockWidget::rebuildView()
{
    output_->clear();
    for (const core::LogRecord& record : records_) {
        if (passesFilter(record)) {
            appendRecord(record);
        }
    }
    output_->verticalScrollBar()->setValue(output_->verticalScrollBar()->maximum());
}

void LogDockWidget::exportToFile()
{
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出日志"), QStringLiteral("hwsim-log.txt"),
        QStringLiteral("文本文件 (*.txt);;所有文件 (*)"));
    if (path.isEmpty()) {
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), file.errorString());
        return;
    }

    // Export what the user is looking at, filters included.
    QTextStream stream(&file);
    for (const core::LogRecord& record : records_) {
        if (passesFilter(record)) {
            stream << record.formatted(true) << '\n';
        }
    }
}

} // namespace hwsim::ui
