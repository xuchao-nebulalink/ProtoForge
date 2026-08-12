#include "ByteBuffer.h"

#include <algorithm>
#include <functional>

namespace hwsim::core {

ByteBuffer::ByteBuffer(std::size_t reserveBytes)
{
    storage_.reserve(reserveBytes);
}

void ByteBuffer::append(std::span<const std::byte> data)
{
    if (data.empty()) {
        return;
    }

    // Guard against appending a view of our own storage, which callers do when
    // they re-queue unconsumed bytes. Both compaction and reallocation would
    // invalidate the span before it is read.
    //
    // std::less rather than the built-in operators: relational comparison of
    // pointers into different objects is unspecified, and this deliberately
    // compares an outside pointer against our buffer to find out whether it is
    // one of ours.
    const std::less<const std::byte*> before;
    const bool aliasesStorage = !storage_.empty() && !before(data.data(), storage_.data())
                                && before(data.data(), storage_.data() + storage_.size());
    if (aliasesStorage) {
        const std::vector<std::byte> copy(data.begin(), data.end());
        compactIfNeeded();
        storage_.insert(storage_.end(), copy.begin(), copy.end());
        return;
    }

    compactIfNeeded();
    storage_.insert(storage_.end(), data.begin(), data.end());
}

void ByteBuffer::append(const QByteArray& data)
{
    append(reinterpret_cast<const char*>(data.constData()), static_cast<std::size_t>(data.size()));
}

void ByteBuffer::append(const char* data, std::size_t size)
{
    if (data == nullptr || size == 0) {
        return;
    }
    append(std::span<const std::byte>{reinterpret_cast<const std::byte*>(data), size});
}

std::span<const std::byte> ByteBuffer::readable() const noexcept
{
    return std::span<const std::byte>{storage_.data() + readPos_, size()};
}

std::span<const std::byte> ByteBuffer::peek(std::size_t count) const noexcept
{
    return readable().first((std::min)(count, size()));
}

void ByteBuffer::consume(std::size_t count) noexcept
{
    readPos_ += (std::min)(count, size());
    if (readPos_ == storage_.size()) {
        storage_.clear();
        readPos_ = 0;
    }
}

QByteArray ByteBuffer::take(std::size_t count)
{
    const auto view = peek(count);
    QByteArray result(reinterpret_cast<const char*>(view.data()),
                      static_cast<qsizetype>(view.size()));
    consume(view.size());
    return result;
}

QByteArray ByteBuffer::toByteArray() const
{
    const auto view = readable();
    return QByteArray(reinterpret_cast<const char*>(view.data()),
                      static_cast<qsizetype>(view.size()));
}

void ByteBuffer::clear() noexcept
{
    storage_.clear();
    readPos_ = 0;
}

void ByteBuffer::reserve(std::size_t bytes)
{
    storage_.reserve(bytes);
}

void ByteBuffer::compactIfNeeded()
{
    if (readPos_ == 0 || readPos_ < compactThreshold_) {
        return;
    }
    storage_.erase(storage_.begin(), storage_.begin() + static_cast<std::ptrdiff_t>(readPos_));
    readPos_ = 0;
}

} // namespace hwsim::core
