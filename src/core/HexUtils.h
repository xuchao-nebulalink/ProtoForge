#pragma once

#include "CoreGlobal.h"

#include <QByteArray>
#include <QString>

#include <cstddef>
#include <span>

namespace hwsim::core::hex {

/// "01 A2 FF" style rendering used by the packet view and the log.
[[nodiscard]] HWSIM_CORE_API QString toHex(std::span<const std::byte> data, QChar separator = QLatin1Char(' '));
[[nodiscard]] HWSIM_CORE_API QString toHex(const QByteArray& data, QChar separator = QLatin1Char(' '));

/// Accepts "01 A2 FF", "01A2FF", "0x01,0xA2" and similar. Returns an empty
/// array and sets `ok` to false when a non-hex character is encountered.
[[nodiscard]] HWSIM_CORE_API QByteArray fromHex(const QString& text, bool* ok = nullptr);

/// Non-printable bytes are replaced by `placeholder`.
[[nodiscard]] HWSIM_CORE_API QString toPrintableAscii(std::span<const std::byte> data,
                                                      QChar placeholder = QLatin1Char('.'));

/// Classic offset / hex / ascii three-column dump.
[[nodiscard]] HWSIM_CORE_API QString dump(std::span<const std::byte> data, int bytesPerLine = 16);
[[nodiscard]] HWSIM_CORE_API QString dump(const QByteArray& data, int bytesPerLine = 16);

[[nodiscard]] HWSIM_CORE_API std::span<const std::byte> asBytes(const QByteArray& data) noexcept;
[[nodiscard]] HWSIM_CORE_API QByteArray toByteArray(std::span<const std::byte> data);

} // namespace hwsim::core::hex
