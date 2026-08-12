#pragma once

#include "CoreGlobal.h"

#include <cstdint>
#include <functional>
#include <string_view>
#include <type_traits>

namespace hwsim::core {
namespace detail {

/// The compiler-provided function signature contains the template argument,
/// which is the only portable source of a type name in C++20.
template <typename T>
constexpr std::string_view rawSignature() noexcept
{
#if defined(_MSC_VER)
    return std::string_view{__FUNCSIG__};
#else
    return std::string_view{__PRETTY_FUNCTION__};
#endif
}

constexpr std::string_view stripElaboration(std::string_view name) noexcept
{
    constexpr std::string_view kClass{"class "};
    constexpr std::string_view kStruct{"struct "};
    constexpr std::string_view kEnum{"enum "};
    constexpr std::string_view kUnion{"union "};

    if (name.substr(0, kClass.size()) == kClass) return name.substr(kClass.size());
    if (name.substr(0, kStruct.size()) == kStruct) return name.substr(kStruct.size());
    if (name.substr(0, kEnum.size()) == kEnum) return name.substr(kEnum.size());
    if (name.substr(0, kUnion.size()) == kUnion) return name.substr(kUnion.size());
    return name;
}

template <typename T>
constexpr std::string_view typeName() noexcept
{
    constexpr std::string_view signature = rawSignature<T>();

#if defined(_MSC_VER)
    constexpr std::string_view marker{"rawSignature<"};
    constexpr auto begin = signature.find(marker);
    if constexpr (begin == std::string_view::npos) {
        return signature;
    } else {
        constexpr auto start = begin + marker.size();
        constexpr auto stop = signature.rfind(">(");
        if constexpr (stop == std::string_view::npos || stop <= start) {
            return signature;
        } else {
            return stripElaboration(signature.substr(start, stop - start));
        }
    }
#else
    constexpr std::string_view marker{"T = "};
    constexpr auto begin = signature.find(marker);
    if constexpr (begin == std::string_view::npos) {
        return signature;
    } else {
        constexpr auto start = begin + marker.size();
        auto stop = signature.find_first_of(";]", start);
        if (stop == std::string_view::npos) stop = signature.size();
        return signature.substr(start, stop - start);
    }
#endif
}

constexpr std::uint64_t fnv1a64(std::string_view text) noexcept
{
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char ch : text) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
        hash *= 1099511628211ULL;
    }
    return hash;
}

} // namespace detail

/// A cheap, RTTI-free type token.
///
/// Identity is a hash of the compiler-generated type name rather than the
/// address of a static member. That distinction matters here: command types are
/// defined inside protocol plugin DLLs, and address-based identity is not
/// guaranteed to be unique across module boundaries on Windows, whereas the
/// name hash is.
///
/// This is what lets CommandRegistry dispatch through a hash table instead of a
/// switch over opcodes.
class TypeId {
public:
    constexpr TypeId() noexcept = default;

    template <typename T>
    [[nodiscard]] static constexpr TypeId of() noexcept
    {
        using Clean = std::remove_cvref_t<T>;
        return TypeId{detail::typeName<Clean>(), detail::fnv1a64(detail::typeName<Clean>())};
    }

    [[nodiscard]] constexpr std::uint64_t hash() const noexcept { return hash_; }
    [[nodiscard]] constexpr std::string_view name() const noexcept { return name_; }
    [[nodiscard]] constexpr bool isValid() const noexcept { return hash_ != 0; }

    [[nodiscard]] friend constexpr bool operator==(TypeId lhs, TypeId rhs) noexcept
    {
        return lhs.hash_ == rhs.hash_;
    }
    [[nodiscard]] friend constexpr bool operator!=(TypeId lhs, TypeId rhs) noexcept
    {
        return lhs.hash_ != rhs.hash_;
    }
    [[nodiscard]] friend constexpr bool operator<(TypeId lhs, TypeId rhs) noexcept
    {
        return lhs.hash_ < rhs.hash_;
    }

private:
    constexpr TypeId(std::string_view name, std::uint64_t hash) noexcept
        : name_(name), hash_(hash) {}

    std::string_view name_{};
    std::uint64_t hash_{0};
};

} // namespace hwsim::core

template <>
struct std::hash<hwsim::core::TypeId> {
    [[nodiscard]] std::size_t operator()(hwsim::core::TypeId id) const noexcept
    {
        return static_cast<std::size_t>(id.hash());
    }
};
