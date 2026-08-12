#pragma once

#include "CoreGlobal.h"

#include <algorithm>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

#if !defined(HWSIM_HAS_STD_BYTESWAP)
#  define HWSIM_HAS_STD_BYTESWAP 0
#endif

namespace hwsim::core::endian {

template <std::integral T>
[[nodiscard]] constexpr T byteswap(T value) noexcept
{
#if HWSIM_HAS_STD_BYTESWAP
    return std::byteswap(value);
#else
    using U = std::make_unsigned_t<T>;
    auto raw = static_cast<U>(value);
    U result{};
    for (std::size_t i = 0; i < sizeof(U); ++i) {
        result = static_cast<U>(static_cast<U>(result << 8) | static_cast<U>(raw & U{0xFF}));
        raw = static_cast<U>(raw >> 8);
    }
    return static_cast<T>(result);
#endif
}

/// Reads up to sizeof(T) bytes, most significant byte first. Short spans are
/// zero-extended rather than treated as an error: framing code checks length
/// before it decodes, and this keeps the accessors noexcept.
template <std::integral T>
[[nodiscard]] constexpr T readBig(std::span<const std::byte> bytes) noexcept
{
    using U = std::make_unsigned_t<T>;
    U value{};
    const std::size_t count = (std::min)(sizeof(T), bytes.size());
    for (std::size_t i = 0; i < count; ++i) {
        value = static_cast<U>(static_cast<U>(value << 8)
                               | static_cast<U>(std::to_integer<unsigned char>(bytes[i])));
    }
    return static_cast<T>(value);
}

template <std::integral T>
[[nodiscard]] constexpr T readLittle(std::span<const std::byte> bytes) noexcept
{
    using U = std::make_unsigned_t<T>;
    U value{};
    const std::size_t count = (std::min)(sizeof(T), bytes.size());
    for (std::size_t i = count; i > 0; --i) {
        value = static_cast<U>(static_cast<U>(value << 8)
                               | static_cast<U>(std::to_integer<unsigned char>(bytes[i - 1])));
    }
    return static_cast<T>(value);
}

template <std::integral T>
constexpr void writeBig(std::span<std::byte> bytes, T value) noexcept
{
    using U = std::make_unsigned_t<T>;
    const auto raw = static_cast<U>(value);
    const std::size_t count = (std::min)(sizeof(T), bytes.size());
    for (std::size_t i = 0; i < count; ++i) {
        const auto shift = static_cast<unsigned>(8 * (count - 1 - i));
        bytes[i] = static_cast<std::byte>((raw >> shift) & U{0xFF});
    }
}

template <std::integral T>
constexpr void writeLittle(std::span<std::byte> bytes, T value) noexcept
{
    using U = std::make_unsigned_t<T>;
    const auto raw = static_cast<U>(value);
    const std::size_t count = (std::min)(sizeof(T), bytes.size());
    for (std::size_t i = 0; i < count; ++i) {
        const auto shift = static_cast<unsigned>(8 * i);
        bytes[i] = static_cast<std::byte>((raw >> shift) & U{0xFF});
    }
}

[[nodiscard]] inline float readBigFloat(std::span<const std::byte> bytes) noexcept
{
    return std::bit_cast<float>(readBig<std::uint32_t>(bytes));
}

[[nodiscard]] inline float readLittleFloat(std::span<const std::byte> bytes) noexcept
{
    return std::bit_cast<float>(readLittle<std::uint32_t>(bytes));
}

[[nodiscard]] inline double readBigDouble(std::span<const std::byte> bytes) noexcept
{
    return std::bit_cast<double>(readBig<std::uint64_t>(bytes));
}

inline void writeBigFloat(std::span<std::byte> bytes, float value) noexcept
{
    writeBig<std::uint32_t>(bytes, std::bit_cast<std::uint32_t>(value));
}

inline void writeLittleFloat(std::span<std::byte> bytes, float value) noexcept
{
    writeLittle<std::uint32_t>(bytes, std::bit_cast<std::uint32_t>(value));
}

inline void writeBigDouble(std::span<std::byte> bytes, double value) noexcept
{
    writeBig<std::uint64_t>(bytes, std::bit_cast<std::uint64_t>(value));
}

/// Word order for 32-bit quantities carried as two 16-bit registers. Modbus
/// devices disagree about this constantly, so it is a configurable property of
/// the device profile rather than something baked into a codec.
enum class WordOrder {
    HighWordFirst,
    LowWordFirst,
};

} // namespace hwsim::core::endian
