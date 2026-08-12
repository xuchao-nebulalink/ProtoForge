#include "Logger.h"

#include "Clock.h"

#include <QHash>
#include <QThread>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <iterator>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>

namespace hwsim::core {
namespace {

constexpr const char* kLevelNames[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL", "OFF"};

} // namespace

QString logLevelName(LogLevel level)
{
    const auto index = static_cast<std::size_t>(level);
    return index < std::size(kLevelNames) ? QString::fromLatin1(kLevelNames[index])
                                          : QStringLiteral("?");
}

LogLevel logLevelFromName(const QString& name, LogLevel fallback)
{
    const QString upper = name.trimmed().toUpper();
    for (std::size_t i = 0; i < std::size(kLevelNames); ++i) {
        if (upper == QLatin1String(kLevelNames[i])) {
            return static_cast<LogLevel>(i);
        }
    }
    return fallback;
}

QString LogRecord::formatted(bool withLocation) const
{
    QString result = QStringLiteral("%1 [%2] %3")
                         .arg(formatWallClock(wallClockMs),
                              logLevelName(level).leftJustified(5),
                              category.isEmpty() ? QStringLiteral("-") : category);
    result += QStringLiteral(" | ") + message;
    if (withLocation && !file.isEmpty()) {
        const auto slash = file.lastIndexOf(QLatin1Char('/'));
        const auto backslash = file.lastIndexOf(QLatin1Char('\\'));
        const auto cut = qMax(slash, backslash);
        const QString shortFile = cut >= 0 ? file.mid(cut + 1) : file;
        result += QStringLiteral("  (%1:%2)").arg(shortFile).arg(line);
    }
    return result;
}

// ---------------------------------------------------------------------------

class Logger::Impl {
public:
    Impl() { startWorker(); }

    ~Impl() { stopWorker(); }

    void startWorker()
    {
        if (worker_.joinable()) {
            return;
        }
        running_ = true;
        worker_ = std::thread([this] { workerLoop(); });
    }

    void stopWorker()
    {
        {
            std::lock_guard lock(queueMutex_);
            running_ = false;
        }
        queueCondition_.notify_all();
        // A thread parked in flush() is waiting on a different condition
        // variable and would otherwise sleep until a spurious wakeup.
        flushCondition_.notify_all();

        if (worker_.joinable()) {
            worker_.join();
        }
        drainQueue();
    }

    void enqueue(LogRecord&& record)
    {
        if (!asynchronous_.load(std::memory_order_relaxed) || !running_) {
            deliver(record);
            return;
        }

        {
            std::lock_guard lock(queueMutex_);
            if (queue_.size() >= queueLimit_) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            queue_.push_back(std::move(record));
        }
        queueCondition_.notify_one();
    }

    void deliver(const LogRecord& record)
    {
        std::shared_lock lock(sinkMutex_);
        for (const auto& entry : sinks_) {
            entry.sink->write(record);
        }
    }

    void flush()
    {
        {
            std::unique_lock lock(queueMutex_);
            // Waiting on an empty queue alone is not enough: the worker empties
            // the queue first and only then writes the batch to the sinks, so
            // flush() could return before a single record had been delivered.
            // delivering_ closes that window, which is what makes a test that
            // asserts on sink contents deterministic.
            flushCondition_.wait(lock,
                                 [this] { return (queue_.empty() && !delivering_) || !running_; });
        }
        std::shared_lock lock(sinkMutex_);
        for (const auto& entry : sinks_) {
            entry.sink->flush();
        }
    }

    struct SinkEntry {
        Logger::SinkId id;
        std::shared_ptr<ILogSink> sink;
    };

    std::atomic<LogLevel> level_{LogLevel::Info};
    std::atomic<bool> asynchronous_{true};
    std::atomic<bool> hasCategoryOverrides_{false};
    std::atomic<quint64> dropped_{0};

    mutable std::shared_mutex sinkMutex_;
    std::vector<SinkEntry> sinks_;
    Logger::SinkId nextSinkId_{1};

    mutable std::shared_mutex categoryMutex_;
    QHash<QString, LogLevel> categoryLevels_;

    std::mutex queueMutex_;
    std::condition_variable queueCondition_;
    std::condition_variable flushCondition_;
    std::deque<LogRecord> queue_;
    std::size_t queueLimit_{16384};

    /// Read by enqueue() outside the queue lock, so it has to be atomic.
    std::atomic<bool> running_{false};

    /// True between the worker taking a batch and finishing its delivery.
    /// Guarded by queueMutex_.
    bool delivering_{false};

    std::thread worker_;

private:
    void workerLoop()
    {
        for (;;) {
            std::deque<LogRecord> batch;
            {
                std::unique_lock lock(queueMutex_);
                queueCondition_.wait(lock, [this] { return !queue_.empty() || !running_; });
                if (queue_.empty() && !running_) {
                    return;
                }
                batch.swap(queue_);
                delivering_ = true;
            }

            for (const auto& record : batch) {
                deliver(record);
            }

            {
                std::lock_guard lock(queueMutex_);
                delivering_ = false;
            }
            flushCondition_.notify_all();
        }
    }

    void drainQueue()
    {
        std::deque<LogRecord> remaining;
        {
            std::lock_guard lock(queueMutex_);
            remaining.swap(queue_);
        }
        for (const auto& record : remaining) {
            deliver(record);
        }
    }
};

// ---------------------------------------------------------------------------

Logger::Logger() : impl_(std::make_unique<Impl>()) {}

Logger::~Logger() = default;

Logger& Logger::instance()
{
    static Logger logger;
    return logger;
}

Logger::SinkId Logger::addSink(std::shared_ptr<ILogSink> sink)
{
    if (!sink) {
        return 0;
    }
    std::unique_lock lock(impl_->sinkMutex_);
    const SinkId id = impl_->nextSinkId_++;
    impl_->sinks_.push_back(Impl::SinkEntry{id, std::move(sink)});
    return id;
}

void Logger::removeSink(SinkId id)
{
    std::unique_lock lock(impl_->sinkMutex_);
    std::erase_if(impl_->sinks_, [id](const Impl::SinkEntry& entry) { return entry.id == id; });
}

void Logger::clearSinks()
{
    std::unique_lock lock(impl_->sinkMutex_);
    impl_->sinks_.clear();
}

void Logger::setLevel(LogLevel level) noexcept
{
    impl_->level_.store(level, std::memory_order_relaxed);
}

LogLevel Logger::level() const noexcept
{
    return impl_->level_.load(std::memory_order_relaxed);
}

void Logger::setCategoryLevel(const QString& category, LogLevel level)
{
    std::unique_lock lock(impl_->categoryMutex_);
    impl_->categoryLevels_.insert(category, level);
    impl_->hasCategoryOverrides_.store(true, std::memory_order_relaxed);
}

void Logger::clearCategoryLevel(const QString& category)
{
    std::unique_lock lock(impl_->categoryMutex_);
    impl_->categoryLevels_.remove(category);
    impl_->hasCategoryOverrides_.store(!impl_->categoryLevels_.isEmpty(), std::memory_order_relaxed);
}

void Logger::clearAllCategoryLevels()
{
    std::unique_lock lock(impl_->categoryMutex_);
    impl_->categoryLevels_.clear();
    impl_->hasCategoryOverrides_.store(false, std::memory_order_relaxed);
}

bool Logger::isEnabled(LogLevel level, const char* category) const noexcept
{
    if (!impl_->hasCategoryOverrides_.load(std::memory_order_relaxed)) {
        return level >= impl_->level_.load(std::memory_order_relaxed);
    }
    return isEnabled(level, QString::fromUtf8(category));
}

bool Logger::isEnabled(LogLevel level, const QString& category) const
{
    if (impl_->hasCategoryOverrides_.load(std::memory_order_relaxed)) {
        std::shared_lock lock(impl_->categoryMutex_);
        const auto it = impl_->categoryLevels_.constFind(category);
        if (it != impl_->categoryLevels_.constEnd()) {
            return level >= it.value();
        }
    }
    return level >= impl_->level_.load(std::memory_order_relaxed);
}

void Logger::submit(LogRecord record)
{
    if (record.wallClockMs == 0) {
        record.wallClockMs = wallClockMs();
    }
    if (record.monotonicMs == 0) {
        record.monotonicMs = monotonicMs();
    }
    if (record.threadId == 0) {
        record.threadId = reinterpret_cast<quintptr>(QThread::currentThreadId());
    }
    impl_->enqueue(std::move(record));
}

void Logger::flush()
{
    impl_->flush();
}

void Logger::shutdown()
{
    impl_->stopWorker();
    clearSinks();
}

void Logger::setAsynchronous(bool enabled)
{
    impl_->asynchronous_.store(enabled, std::memory_order_relaxed);
    if (enabled) {
        impl_->startWorker();
    } else {
        impl_->flush();
    }
}

bool Logger::isAsynchronous() const noexcept
{
    return impl_->asynchronous_.load(std::memory_order_relaxed);
}

quint64 Logger::droppedRecordCount() const noexcept
{
    return impl_->dropped_.load(std::memory_order_relaxed);
}

void Logger::setQueueLimit(std::size_t records)
{
    std::lock_guard lock(impl_->queueMutex_);
    impl_->queueLimit_ = records;
}

// ---------------------------------------------------------------------------

LogStream::LogStream(LogLevel level, const char* category, const char* file, int line)
    : LogStream(level, QString::fromUtf8(category), file, line)
{
}

LogStream::LogStream(LogLevel level, QString category, const char* file, int line)
    : stream_(&buffer_)
{
    record_.level = level;
    record_.category = std::move(category);
    record_.file = QString::fromUtf8(file);
    record_.line = line;
}

LogStream::~LogStream()
{
    stream_.flush();
    record_.message = buffer_;
    Logger::instance().submit(std::move(record_));
}

} // namespace hwsim::core
