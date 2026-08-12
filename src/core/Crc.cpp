#include "Crc.h"

#include <unordered_map>

namespace hwsim::core::crc {
namespace {

struct AlgorithmTraits {
    std::uint32_t (*compute)(std::span<const std::byte>);
    std::size_t width;
};

const std::unordered_map<Algorithm, AlgorithmTraits>& traitsTable()
{
    static const std::unordered_map<Algorithm, AlgorithmTraits> table{
        {Algorithm::None,
         {[](std::span<const std::byte>) -> std::uint32_t { return 0; }, 0}},
        {Algorithm::Modbus16,
         {[](std::span<const std::byte> d) -> std::uint32_t { return modbus(d); }, 2}},
        {Algorithm::CcittFalse16,
         {[](std::span<const std::byte> d) -> std::uint32_t { return ccittFalse(d); }, 2}},
        {Algorithm::Crc32,
         {[](std::span<const std::byte> d) -> std::uint32_t { return crc32(d); }, 4}},
        {Algorithm::Sum8,
         {[](std::span<const std::byte> d) -> std::uint32_t { return sum8(d); }, 1}},
        {Algorithm::Xor8,
         {[](std::span<const std::byte> d) -> std::uint32_t { return xor8(d); }, 1}},
        {Algorithm::Lrc8,
         {[](std::span<const std::byte> d) -> std::uint32_t { return lrc(d); }, 1}},
        {Algorithm::Umts16,
         {[](std::span<const std::byte> d) -> std::uint32_t { return umts(d); }, 2}},
    };
    return table;
}

} // namespace

std::uint32_t compute(Algorithm algorithm, std::span<const std::byte> data)
{
    const auto& table = traitsTable();
    const auto it = table.find(algorithm);
    return it == table.end() ? 0u : it->second.compute(data);
}

std::size_t widthBytes(Algorithm algorithm) noexcept
{
    const auto& table = traitsTable();
    const auto it = table.find(algorithm);
    return it == table.end() ? 0u : it->second.width;
}

} // namespace hwsim::core::crc
