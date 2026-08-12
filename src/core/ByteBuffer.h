#pragma once

#include "CoreGlobal.h"

#include <QByteArray>

#include <cstddef>
#include <span>
#include <vector>

namespace hwsim::core {

/// Accumulation buffer sitting between a transport and a frame codec.
///
/// Stream transports deliver arbitrary fragments, so a codec needs to inspect
/// what has arrived so far without consuming it, then consume exactly one frame
/// once it is complete. That is the peek/consume pair below. Consumed bytes are
/// not moved immediately; the read cursor advances and the buffer compacts only
/// once the dead prefix grows past a threshold, which keeps the common
/// one-frame-per-read case allocation free.
class HWSIM_CORE_API ByteBuffer {
public:
    static constexpr std::size_t kDefaultCompactThreshold = 4096;

    ByteBuffer() = default;
    explicit ByteBuffer(std::size_t reserveBytes);

    void append(std::span<const std::byte> data);
    void append(const QByteArray& data);
    void append(const char* data, std::size_t size);

    /// All unconsumed bytes. The span is invalidated by any append or consume.
    [[nodiscard]] std::span<const std::byte> readable() const noexcept;

    /// First `count` unconsumed bytes, clamped to what is available.
    [[nodiscard]] std::span<const std::byte> peek(std::size_t count) const noexcept;

    /// Drops `count` bytes from the front, clamped to what is available.
    void consume(std::size_t count) noexcept;

    /// Consumes and returns the first `count` bytes.
    [[nodiscard]] QByteArray take(std::size_t count);

    [[nodiscard]] std::size_t size() const noexcept { return storage_.size() - readPos_; }
    [[nodiscard]] bool isEmpty() const noexcept { return size() == 0; }
    [[nodiscard]] std::size_t capacity() const noexcept { return storage_.capacity(); }

    [[nodiscard]] QByteArray toByteArray() const;

    void clear() noexcept;
    void reserve(std::size_t bytes);

    /// Advisory ceiling on buffered bytes, for callers to test with
    /// isOverflowing(). append() does not enforce it: the session decides what
    /// to do about a peer that never sends a frame delimiter, and dropping
    /// bytes silently inside append() would hide that.
    void setCapacityLimit(std::size_t bytes) noexcept { capacityLimit_ = bytes; }
    [[nodiscard]] std::size_t capacityLimit() const noexcept { return capacityLimit_; }
    [[nodiscard]] bool isOverflowing() const noexcept { return size() > capacityLimit_; }

    void setCompactThreshold(std::size_t bytes) noexcept { compactThreshold_ = bytes; }

private:
    void compactIfNeeded();

    std::vector<std::byte> storage_;
    std::size_t readPos_{0};
    std::size_t compactThreshold_{kDefaultCompactThreshold};
    std::size_t capacityLimit_{1u << 20};
};

} // namespace hwsim::core
