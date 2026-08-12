#include "LogSinks.h"

#include <QDir>
#include <QFileInfo>

#include <cstdio>

namespace hwsim::core {
namespace {

const char* ansiColourFor(LogLevel level)
{
    // Matches the severity colours used by the log dock so console and UI agree.
    switch (level) {
    case LogLevel::Trace:    return "\033[90m";
    case LogLevel::Debug:    return "\033[36m";
    case LogLevel::Info:     return "\033[0m";
    case LogLevel::Warning:  return "\033[33m";
    case LogLevel::Error:    return "\033[31m";
    case LogLevel::Critical: return "\033[1;31m";
    case LogLevel::Off:      return "\033[0m";
    }
    return "\033[0m";
}

} // namespace

ConsoleLogSink::ConsoleLogSink(bool withLocation, bool colourise)
    : withLocation_(withLocation), colourise_(colourise)
{
}

void ConsoleLogSink::write(const LogRecord& record)
{
    const QString line = record.formatted(withLocation_);
    const QByteArray utf8 = line.toUtf8();

    std::lock_guard lock(mutex_);
    FILE* target = record.level >= LogLevel::Warning ? stderr : stdout;
    if (colourise_) {
        std::fprintf(target, "%s%s\033[0m\n", ansiColourFor(record.level), utf8.constData());
    } else {
        std::fprintf(target, "%s\n", utf8.constData());
    }
}

void ConsoleLogSink::flush()
{
    std::lock_guard lock(mutex_);
    std::fflush(stdout);
    std::fflush(stderr);
}

// ---------------------------------------------------------------------------

FileLogSink::FileLogSink(QString path, qint64 maxBytes, int keepFiles)
    : path_(std::move(path)), maxBytes_(maxBytes), keepFiles_(keepFiles), file_(path_)
{
    QDir().mkpath(QFileInfo(path_).absolutePath());
    if (file_.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        stream_.setDevice(&file_);
    }
}

FileLogSink::~FileLogSink()
{
    std::lock_guard lock(mutex_);
    stream_.flush();
    file_.close();
}

void FileLogSink::write(const LogRecord& record)
{
    std::lock_guard lock(mutex_);
    if (!file_.isOpen()) {
        return;
    }
    stream_ << record.formatted(true) << '\n';
    if (file_.size() > maxBytes_) {
        rotate();
    }
}

void FileLogSink::flush()
{
    std::lock_guard lock(mutex_);
    stream_.flush();
}

bool FileLogSink::isOpen() const
{
    return file_.isOpen();
}

void FileLogSink::rotate()
{
    stream_.flush();
    file_.close();

    // hwsim.log -> hwsim.log.1 -> hwsim.log.2 ...
    for (int index = keepFiles_ - 1; index >= 1; --index) {
        const QString from = QStringLiteral("%1.%2").arg(path_).arg(index);
        const QString to = QStringLiteral("%1.%2").arg(path_).arg(index + 1);
        if (QFile::exists(from)) {
            QFile::remove(to);
            QFile::rename(from, to);
        }
    }
    QFile::remove(QStringLiteral("%1.1").arg(path_));
    QFile::rename(path_, QStringLiteral("%1.1").arg(path_));

    file_.setFileName(path_);
    if (file_.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        stream_.setDevice(&file_);
    }
}

// ---------------------------------------------------------------------------

RingBufferLogSink::RingBufferLogSink(std::size_t capacity) : capacity_(capacity) {}

void RingBufferLogSink::write(const LogRecord& record)
{
    std::lock_guard lock(mutex_);
    records_.push_back(record);
    while (records_.size() > capacity_) {
        records_.pop_front();
    }
}

std::vector<LogRecord> RingBufferLogSink::snapshot() const
{
    std::lock_guard lock(mutex_);
    return std::vector<LogRecord>(records_.begin(), records_.end());
}

std::size_t RingBufferLogSink::size() const
{
    std::lock_guard lock(mutex_);
    return records_.size();
}

void RingBufferLogSink::clear()
{
    std::lock_guard lock(mutex_);
    records_.clear();
}

void RingBufferLogSink::setCapacity(std::size_t capacity)
{
    std::lock_guard lock(mutex_);
    capacity_ = capacity;
    while (records_.size() > capacity_) {
        records_.pop_front();
    }
}

} // namespace hwsim::core
