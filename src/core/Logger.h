#pragma once

#include "CoreGlobal.h"

#include <QString>
#include <QTextStream>

#include <memory>

namespace hwsim::core {

enum class LogLevel {
    Trace = 0,
    Debug,
    Info,
    Warning,
    Error,
    Critical,
    Off,
};

[[nodiscard]] HWSIM_CORE_API QString logLevelName(LogLevel level);
[[nodiscard]] HWSIM_CORE_API LogLevel logLevelFromName(const QString& name, LogLevel fallback = LogLevel::Info);

struct HWSIM_CORE_API LogRecord {
    qint64 wallClockMs{0};
    qint64 monotonicMs{0};
    LogLevel level{LogLevel::Info};
    QString category;
    QString message;
    QString file;
    int line{0};
    quintptr threadId{0};

    [[nodiscard]] QString formatted(bool withLocation = false) const;
};

class HWSIM_CORE_API ILogSink {
public:
    virtual ~ILogSink() = default;
    virtual void write(const LogRecord& record) = 0;
    virtual void flush() {}
};

/// Process-wide log hub.
///
/// Lives in the core shared library so that the application, the protocol
/// plugins and the test binaries all publish into the same instance. Records
/// are handed to a worker thread by default, which keeps the protocol hot path
/// free of file and console I/O; tests turn that off with setAsynchronous().
class HWSIM_CORE_API Logger {
public:
    using SinkId = quint64;

    [[nodiscard]] static Logger& instance();

    SinkId addSink(std::shared_ptr<ILogSink> sink);

    /// Removes a sink and waits for any delivery already in progress.
    ///
    /// Unlike EventBus, the logger holds its lock across the whole fan-out, so
    /// once this returns the sink is guaranteed not to be called again. A sink
    /// that captures a pointer to its owner can rely on that.
    void removeSink(SinkId id);

    void clearSinks();

    void setLevel(LogLevel level) noexcept;
    [[nodiscard]] LogLevel level() const noexcept;

    /// Overrides the global threshold for one category, e.g. "transport.tcp".
    void setCategoryLevel(const QString& category, LogLevel level);
    void clearCategoryLevel(const QString& category);
    void clearAllCategoryLevels();

    [[nodiscard]] bool isEnabled(LogLevel level, const char* category) const noexcept;
    [[nodiscard]] bool isEnabled(LogLevel level, const QString& category) const;

    void submit(LogRecord record);

    /// Blocks until the queue has drained. Called before the process exits and
    /// by tests that assert on sink contents.
    void flush();
    void shutdown();

    void setAsynchronous(bool enabled);
    [[nodiscard]] bool isAsynchronous() const noexcept;

    /// Records dropped because the queue hit its bound.
    [[nodiscard]] quint64 droppedRecordCount() const noexcept;

    void setQueueLimit(std::size_t records);

private:
    Logger();
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    class Impl;
    std::unique_ptr<Impl> impl_;
};

/// Temporary produced by the HWSIM_LOG macros; submits on destruction.
class HWSIM_CORE_API LogStream {
public:
    /// Both overloads exist so the macros accept either a string literal (the
    /// common case, no allocation when the level is disabled) or a QString
    /// category held by the caller.
    LogStream(LogLevel level, const char* category, const char* file, int line);
    LogStream(LogLevel level, QString category, const char* file, int line);
    ~LogStream();

    LogStream(const LogStream&) = delete;
    LogStream& operator=(const LogStream&) = delete;

    template <typename T>
    LogStream& operator<<(const T& value)
    {
        stream_ << value;
        return *this;
    }

private:
    LogRecord record_;
    QString buffer_;
    QTextStream stream_;
};

} // namespace hwsim::core

/// The for-loop wrapper keeps the stream from being constructed when the level
/// is disabled, and unlike an if/else macro it cannot swallow a trailing else.
///
/// `categoryValue` may be a string literal or a QString, and is evaluated twice
/// (once for the level check, once for the record), so pass a literal or a
/// variable rather than a call with side effects.
#define HWSIM_LOG(levelValue, categoryValue)                                             \
    for (bool hwsimLogProceed_ =                                                         \
             ::hwsim::core::Logger::instance().isEnabled((levelValue), (categoryValue)); \
         hwsimLogProceed_; hwsimLogProceed_ = false)                                     \
    ::hwsim::core::LogStream((levelValue), (categoryValue), __FILE__, __LINE__)

#define HWSIM_LOG_TRACE(category)    HWSIM_LOG(::hwsim::core::LogLevel::Trace, category)
#define HWSIM_LOG_DEBUG(category)    HWSIM_LOG(::hwsim::core::LogLevel::Debug, category)
#define HWSIM_LOG_INFO(category)     HWSIM_LOG(::hwsim::core::LogLevel::Info, category)
#define HWSIM_LOG_WARNING(category)  HWSIM_LOG(::hwsim::core::LogLevel::Warning, category)
#define HWSIM_LOG_ERROR(category)    HWSIM_LOG(::hwsim::core::LogLevel::Error, category)
#define HWSIM_LOG_CRITICAL(category) HWSIM_LOG(::hwsim::core::LogLevel::Critical, category)
