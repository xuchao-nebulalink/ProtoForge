#pragma once

#include "Logger.h"

#include <QFile>
#include <QString>
#include <QTextStream>

#include <deque>
#include <functional>
#include <mutex>
#include <vector>

namespace hwsim::core {

class HWSIM_CORE_API ConsoleLogSink final : public ILogSink {
public:
    explicit ConsoleLogSink(bool withLocation = false, bool colourise = true);
    void write(const LogRecord& record) override;
    void flush() override;

private:
    bool withLocation_;
    bool colourise_;
    std::mutex mutex_;
};

/// Appends to a file, rotating once the configured size is exceeded.
class HWSIM_CORE_API FileLogSink final : public ILogSink {
public:
    explicit FileLogSink(QString path, qint64 maxBytes = 16 * 1024 * 1024, int keepFiles = 3);
    ~FileLogSink() override;

    void write(const LogRecord& record) override;
    void flush() override;

    [[nodiscard]] bool isOpen() const;
    [[nodiscard]] QString path() const { return path_; }

private:
    void rotate();

    QString path_;
    qint64 maxBytes_;
    int keepFiles_;
    QFile file_;
    QTextStream stream_;
    std::mutex mutex_;
};

/// Bounded in-memory history. The log dock attaches one of these so that a
/// window opened after startup still shows what already happened.
class HWSIM_CORE_API RingBufferLogSink final : public ILogSink {
public:
    explicit RingBufferLogSink(std::size_t capacity = 20000);

    void write(const LogRecord& record) override;

    [[nodiscard]] std::vector<LogRecord> snapshot() const;
    [[nodiscard]] std::size_t size() const;
    void clear();

    void setCapacity(std::size_t capacity);

private:
    mutable std::mutex mutex_;
    std::deque<LogRecord> records_;
    std::size_t capacity_;
};

/// Adapter for callers that would rather receive a callback than implement the
/// interface. The callback runs on the logger worker thread.
class HWSIM_CORE_API FunctionLogSink final : public ILogSink {
public:
    using Callback = std::function<void(const LogRecord&)>;

    explicit FunctionLogSink(Callback callback) : callback_(std::move(callback)) {}

    void write(const LogRecord& record) override
    {
        if (callback_) {
            callback_(record);
        }
    }

private:
    Callback callback_;
};

} // namespace hwsim::core
