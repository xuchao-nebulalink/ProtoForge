#pragma once

#include "ErrorCode.h"

#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace hwsim::core {

/// Return type used throughout the framework instead of exceptions.
///
///     Result<int> readRegister(quint16 address) {
///         if (address >= kCount) return makeError(ErrorCode::OutOfRange, "address");
///         return values[address];              // implicit success
///     }
///
/// Both constructors are implicit so call sites stay free of ceremony.
template <typename T>
class Result {
    static_assert(!std::is_same_v<std::remove_cvref_t<T>, Error>,
                  "Result<Error> is ambiguous; use Result<void> or wrap the payload");

public:
    using value_type = T;

    Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
    Result(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}

    [[nodiscard]] bool hasValue() const noexcept { return storage_.index() == 0; }
    [[nodiscard]] bool hasError() const noexcept { return storage_.index() == 1; }
    explicit operator bool() const noexcept { return hasValue(); }

    [[nodiscard]] T& value() & { return std::get<0>(storage_); }
    [[nodiscard]] const T& value() const& { return std::get<0>(storage_); }
    [[nodiscard]] T&& value() && { return std::get<0>(std::move(storage_)); }

    [[nodiscard]] const Error& error() const& { return std::get<1>(storage_); }
    [[nodiscard]] Error&& error() && { return std::get<1>(std::move(storage_)); }

    [[nodiscard]] T valueOr(T fallback) const&
    {
        return hasValue() ? std::get<0>(storage_) : std::move(fallback);
    }

    /// Applies `fn` to the contained value, propagating any error unchanged.
    template <typename F>
    [[nodiscard]] auto map(F&& fn) const& -> Result<std::invoke_result_t<F, const T&>>
    {
        using U = std::invoke_result_t<F, const T&>;

        // A void-returning fn has to be handled separately: Result<void> has no
        // constructor that could take the result of calling it.
        if constexpr (std::is_void_v<U>) {
            if (hasError()) return Result<void>(error());
            std::forward<F>(fn)(value());
            return Result<void>{};
        } else {
            if (hasError()) return Result<U>(error());
            return Result<U>(std::forward<F>(fn)(value()));
        }
    }

    /// Like map(), but `fn` itself returns a Result, so the results are flattened.
    template <typename F>
    [[nodiscard]] auto andThen(F&& fn) const& -> std::invoke_result_t<F, const T&>
    {
        using R = std::invoke_result_t<F, const T&>;
        if (hasError()) return R(error());
        return std::forward<F>(fn)(value());
    }

private:
    std::variant<T, Error> storage_;
};

/// Void specialisation: default-constructed means success.
template <>
class Result<void> {
public:
    using value_type = void;

    Result() = default;
    Result(Error error) : error_(std::move(error)) {}

    [[nodiscard]] bool hasValue() const noexcept { return !error_.has_value(); }
    [[nodiscard]] bool hasError() const noexcept { return error_.has_value(); }
    explicit operator bool() const noexcept { return hasValue(); }

    [[nodiscard]] const Error& error() const& { return *error_; }
    [[nodiscard]] Error&& error() && { return std::move(*error_); }

    template <typename F>
    [[nodiscard]] auto andThen(F&& fn) const -> std::invoke_result_t<F>
    {
        using R = std::invoke_result_t<F>;
        if (hasError()) return R(*error_);
        return std::forward<F>(fn)();
    }

private:
    std::optional<Error> error_;
};

/// Explicit success factory, useful where the value type cannot be deduced.
template <typename T>
[[nodiscard]] Result<T> success(T value)
{
    return Result<T>(std::move(value));
}

[[nodiscard]] inline Result<void> success()
{
    return Result<void>{};
}

template <typename T = void>
[[nodiscard]] Result<T> failure(ErrorCode code, QString message, QString context = {})
{
    return Result<T>(makeError(code, std::move(message), std::move(context)));
}

} // namespace hwsim::core
