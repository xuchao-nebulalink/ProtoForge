#include "HexUtils.h"

#include <cstdint>

namespace hwsim::core::hex {
namespace {

constexpr char kDigits[] = "0123456789ABCDEF";

[[nodiscard]] int hexValue(QChar ch)
{
    const ushort code = ch.unicode();
    if (code >= '0' && code <= '9') return code - '0';
    if (code >= 'a' && code <= 'f') return code - 'a' + 10;
    if (code >= 'A' && code <= 'F') return code - 'A' + 10;
    return -1;
}

} // namespace

QString toHex(std::span<const std::byte> data, QChar separator)
{
    if (data.empty()) {
        return {};
    }

    const bool useSeparator = !separator.isNull();
    QString result;
    result.reserve(static_cast<qsizetype>(data.size()) * (useSeparator ? 3 : 2));

    for (std::size_t i = 0; i < data.size(); ++i) {
        if (useSeparator && i != 0) {
            result.append(separator);
        }
        const auto value = std::to_integer<std::uint8_t>(data[i]);
        result.append(QLatin1Char(kDigits[value >> 4]));
        result.append(QLatin1Char(kDigits[value & 0x0F]));
    }
    return result;
}

QString toHex(const QByteArray& data, QChar separator)
{
    return toHex(asBytes(data), separator);
}

QByteArray fromHex(const QString& text, bool* ok)
{
    QByteArray result;
    result.reserve(text.size() / 2 + 1);

    int high = -1;
    for (int i = 0; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        if (ch.isSpace() || ch == QLatin1Char(',') || ch == QLatin1Char('-') || ch == QLatin1Char(':')) {
            continue;
        }
        // Skip an "0x" / "0X" prefix without treating the zero as a digit.
        if (ch == QLatin1Char('0') && i + 1 < text.size()
            && (text.at(i + 1) == QLatin1Char('x') || text.at(i + 1) == QLatin1Char('X'))) {
            ++i;
            continue;
        }

        const int value = hexValue(ch);
        if (value < 0) {
            if (ok != nullptr) *ok = false;
            return {};
        }
        if (high < 0) {
            high = value;
        } else {
            result.append(static_cast<char>((high << 4) | value));
            high = -1;
        }
    }

    if (high >= 0) {
        // Odd digit count: treat the trailing nibble as a low nibble.
        result.append(static_cast<char>(high));
    }
    if (ok != nullptr) *ok = true;
    return result;
}

QString toPrintableAscii(std::span<const std::byte> data, QChar placeholder)
{
    QString result;
    result.reserve(static_cast<qsizetype>(data.size()));
    for (const std::byte byte : data) {
        const auto value = std::to_integer<std::uint8_t>(byte);
        result.append(value >= 0x20 && value < 0x7F ? QLatin1Char(static_cast<char>(value))
                                                    : placeholder);
    }
    return result;
}

QString dump(std::span<const std::byte> data, int bytesPerLine)
{
    if (bytesPerLine <= 0) {
        bytesPerLine = 16;
    }
    const auto perLine = static_cast<std::size_t>(bytesPerLine);

    QString result;
    for (std::size_t offset = 0; offset < data.size(); offset += perLine) {
        const auto chunk = data.subspan(offset, (std::min)(perLine, data.size() - offset));

        result += QStringLiteral("%1  ").arg(static_cast<qulonglong>(offset), 8, 16, QLatin1Char('0'));

        const QString hexPart = toHex(chunk);
        result += hexPart;
        result += QString(static_cast<qsizetype>((perLine - chunk.size()) * 3), QLatin1Char(' '));

        result += QStringLiteral("  |") + toPrintableAscii(chunk) + QLatin1Char('|');
        result += QLatin1Char('\n');
    }
    return result;
}

QString dump(const QByteArray& data, int bytesPerLine)
{
    return dump(asBytes(data), bytesPerLine);
}

std::span<const std::byte> asBytes(const QByteArray& data) noexcept
{
    return std::span<const std::byte>{reinterpret_cast<const std::byte*>(data.constData()),
                                      static_cast<std::size_t>(data.size())};
}

QByteArray toByteArray(std::span<const std::byte> data)
{
    return QByteArray(reinterpret_cast<const char*>(data.data()),
                      static_cast<qsizetype>(data.size()));
}

} // namespace hwsim::core::hex
