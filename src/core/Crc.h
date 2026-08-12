#pragma once

#include "CoreGlobal.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace hwsim::core::crc {
namespace detail {

constexpr std::array<std::uint16_t, 256> makeReflected16Table(std::uint16_t polynomial)
{
    std::array<std::uint16_t, 256> table{};
    for (std::size_t index = 0; index < table.size(); ++index) {
        auto value = static_cast<std::uint16_t>(index);
        for (int bit = 0; bit < 8; ++bit) {
            value = (value & 0x0001u)
                        ? static_cast<std::uint16_t>(static_cast<std::uint16_t>(value >> 1) ^ polynomial)
                        : static_cast<std::uint16_t>(value >> 1);
        }
        table[index] = value;
    }
    return table;
}

constexpr std::array<std::uint16_t, 256> makeForward16Table(std::uint16_t polynomial)
{
    std::array<std::uint16_t, 256> table{};
    for (std::size_t index = 0; index < table.size(); ++index) {
        auto value = static_cast<std::uint16_t>(index << 8);
        for (int bit = 0; bit < 8; ++bit) {
            value = (value & 0x8000u)
                        ? static_cast<std::uint16_t>(static_cast<std::uint16_t>(value << 1) ^ polynomial)
                        : static_cast<std::uint16_t>(value << 1);
        }
        table[index] = value;
    }
    return table;
}

constexpr std::array<std::uint32_t, 256> makeReflected32Table(std::uint32_t polynomial)
{
    std::array<std::uint32_t, 256> table{};
    for (std::size_t index = 0; index < table.size(); ++index) {
        auto value = static_cast<std::uint32_t>(index);
        for (int bit = 0; bit < 8; ++bit) {
            value = (value & 1u) ? ((value >> 1) ^ polynomial) : (value >> 1);
        }
        table[index] = value;
    }
    return table;
}

inline constexpr auto kModbusTable = makeReflected16Table(0xA001u);
inline constexpr auto kCcittTable = makeForward16Table(0x1021u);
inline constexpr auto kCrc32Table = makeReflected32Table(0xEDB88320u);

} // namespace detail

/// CRC-16/MODBUS: reflected, polynomial 0xA001, init 0xFFFF, no final XOR.
/// The `seed` parameter allows incremental computation over split buffers,
/// which the fault injector relies on when it rewrites part of a frame.
[[nodiscard]] constexpr std::uint16_t modbus(std::span<const std::byte> data,
                                             std::uint16_t seed = 0xFFFFu) noexcept
{
    std::uint16_t value = seed;
    for (const std::byte byte : data) {
        const auto index = static_cast<std::uint8_t>(
            static_cast<std::uint8_t>(value & 0xFFu) ^ std::to_integer<std::uint8_t>(byte));
        value = static_cast<std::uint16_t>(static_cast<std::uint16_t>(value >> 8)
                                           ^ detail::kModbusTable[index]);
    }
    return value;
}

/// CRC-16/CCITT-FALSE: forward, polynomial 0x1021, init 0xFFFF, no final XOR.
[[nodiscard]] constexpr std::uint16_t ccittFalse(std::span<const std::byte> data,
                                                 std::uint16_t seed = 0xFFFFu) noexcept
{
    std::uint16_t value = seed;
    for (const std::byte byte : data) {
        const auto index = static_cast<std::uint8_t>(
            static_cast<std::uint8_t>(value >> 8) ^ std::to_integer<std::uint8_t>(byte));
        value = static_cast<std::uint16_t>(static_cast<std::uint16_t>(value << 8)
                                           ^ detail::kCcittTable[index]);
    }
    return value;
}

/// Running CRC-32 state, without the final inversion.
///
/// Split from crc32() because the final XOR makes the result unusable as the
/// seed of the next chunk: unlike the CRC-16 variants above, which have no
/// final XOR and so chain naturally, crc32(b, crc32(a)) would not equal
/// crc32(a + b). Incremental callers accumulate with this and invert once at
/// the end with crc32Finalise().
[[nodiscard]] constexpr std::uint32_t crc32Update(std::span<const std::byte> data,
                                                  std::uint32_t seed = 0xFFFFFFFFu) noexcept
{
    std::uint32_t value = seed;
    for (const std::byte byte : data) {
        const auto index = static_cast<std::uint8_t>((value & 0xFFu)
                                                     ^ std::to_integer<std::uint8_t>(byte));
        value = (value >> 8) ^ detail::kCrc32Table[index];
    }
    return value;
}

[[nodiscard]] constexpr std::uint32_t crc32Finalise(std::uint32_t running) noexcept
{
    return running ^ 0xFFFFFFFFu;
}

/// CRC-32 as used by zlib: reflected, polynomial 0xEDB88320, init and final XOR 0xFFFFFFFF.
[[nodiscard]] constexpr std::uint32_t crc32(std::span<const std::byte> data) noexcept
{
    return crc32Finalise(crc32Update(data));
}

[[nodiscard]] constexpr std::uint8_t sum8(std::span<const std::byte> data) noexcept
{
    std::uint8_t value = 0;
    for (const std::byte byte : data) {
        value = static_cast<std::uint8_t>(value + std::to_integer<std::uint8_t>(byte));
    }
    return value;
}

[[nodiscard]] constexpr std::uint8_t xor8(std::span<const std::byte> data) noexcept
{
    std::uint8_t value = 0;
    for (const std::byte byte : data) {
        value = static_cast<std::uint8_t>(value ^ std::to_integer<std::uint8_t>(byte));
    }
    return value;
}

/// Longitudinal redundancy check, used by Modbus ASCII.
[[nodiscard]] constexpr std::uint8_t lrc(std::span<const std::byte> data) noexcept
{
    return static_cast<std::uint8_t>(static_cast<std::uint8_t>(~sum8(data)) + 1u);
}

/// Algorithm selector so a codec can expose the checksum kind as configuration
/// instead of hard-coding one.
enum class Algorithm {
    None,
    Modbus16,
    CcittFalse16,
    Crc32,
    Sum8,
    Xor8,
    Lrc8,
};

[[nodiscard]] HWSIM_CORE_API std::uint32_t compute(Algorithm algorithm, std::span<const std::byte> data);
[[nodiscard]] HWSIM_CORE_API std::size_t widthBytes(Algorithm algorithm) noexcept;

} // namespace hwsim::core::crc
