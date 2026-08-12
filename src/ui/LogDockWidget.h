#pragma once

#include "UiGlobal.h"

#include <core/LogSinks.h>
#include <core/Logger.h>

#include <QVector>
#include <QWidget>

#include <memory>
#include <mutex>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QTimer;

namespace hwsim::ui {

/// Log window: severity and category filters, text search, pause, export.
///
/// Records arrive on the logger's worker thread, so they are queued into a
/// staging buffer and flushed into the widget on a timer. Without that, a burst
/// of protocol tracing would post one cross-thread event per line and stall the
/// UI; batching keeps the window responsive at thousands of lines per second.
class HWSIM_UI_API LogDockWidget : public QWidget {
    Q_OBJECT

public:
    explicit LogDockWidget(QWidget* parent = nullptr);
    ~LogDockWidget() override;

    /// Attaches to the process logger. Also replays whatever the shared history
    /// sink already holds, so a window opened late is not empty.
    void attachToLogger(std::shared_ptr<core::RingBufferLogSink> history = {});
    void detachFromLogger();

    void setMaximumLines(int lines);
    [[nodiscard]] int maximumLines() const noexcept { return maximumLines_; }

public slots:
    void clearLog();
    void exportToFile();
    void setPaused(bool paused);

private:
    void flushPending();
    void appendRecord(const core::LogRecord& record);
    [[nodiscard]] bool passesFilter(const core::LogRecord& record) const;
    void rebuildView();

    QComboBox* levelFilter_{nullptr};
    QLineEdit* categoryFilter_{nullptr};
    QLineEdit* searchFilter_{nullptr};
    QCheckBox* pauseBox_{nullptr};
    QCheckBox* followBox_{nullptr};
    QPlainTextEdit* output_{nullptr};
    QTimer* flushTimer_{nullptr};

    std::mutex pendingMutex_;
    std::vector<core::LogRecord> pending_;

    /// Kept so the view can be rebuilt when the filter changes.
    QVector<core::LogRecord> records_;

    core::Logger::SinkId sinkId_{0};
    int maximumLines_{5000};
    bool paused_{false};
};

} // namespace hwsim::ui
