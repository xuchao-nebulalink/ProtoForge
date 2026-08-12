#include "Sw6Types.h"

#include <core/Crc.h>
#include <core/HexUtils.h>

namespace hwsim::plugins::sw6 {
namespace {

quint32 parseHexToken(QStringView token, Sw6Value& value)
{
    const QStringView digits = token.sliced(2);
    if (digits.isEmpty() || digits.size() > 8) {
        return err::kArgumentTypeMismatch;
    }

    bool ok = false;
    const auto parsed = digits.toUInt(&ok, 16);
    if (!ok) {
        return err::kArgumentTypeMismatch;
    }

    value = Sw6Value::ofHex(parsed, static_cast<quint8>(digits.size()));
    return err::kSuccess;
}

} // namespace

OpCode commandOpcodeOf(QStringView name)
{
    std::uint32_t hash = 2166136261u;
    for (const QChar character : name) {
        hash ^= static_cast<std::uint8_t>(character.toLatin1());
        hash *= 16777619u;
    }
    return static_cast<OpCode>(hash & 0x7FFFFFFFu);
}

int axisIndex(QStringView name)
{
    if (name.size() != 1) {
        return -1;
    }
    const QChar upper = name.front().toUpper();
    for (int index = 0; index < kAxisCount; ++index) {
        if (upper == QLatin1Char(kAxisNames[static_cast<std::size_t>(index)].front())) {
            return index;
        }
    }
    return -1;
}

QString axisName(int index)
{
    if (index < 0 || index >= kAxisCount) {
        return {};
    }
    return QString::fromLatin1(kAxisNames[static_cast<std::size_t>(index)].data(), 1);
}

// --- Sw6Value --------------------------------------------------------------

Sw6Value Sw6Value::ofFloat(double value)
{
    Sw6Value token;
    token.kind = Kind::Float;
    token.real = value;
    token.whole = static_cast<qint64>(value);
    return token;
}

Sw6Value Sw6Value::ofInt(qint64 value)
{
    Sw6Value token;
    token.kind = Kind::Integer;
    token.whole = value;
    token.real = static_cast<double>(value);
    return token;
}

Sw6Value Sw6Value::ofText(QString value)
{
    Sw6Value token;
    token.kind = Kind::Text;
    token.label = std::move(value);
    return token;
}

Sw6Value Sw6Value::ofHex(quint32 value, quint8 digits)
{
    Sw6Value token;
    token.kind = Kind::Hex;
    token.whole = value;
    token.real = static_cast<double>(value);
    token.hexDigits = digits;
    return token;
}

double Sw6Value::asDouble() const
{
    return kind == Kind::Text ? 0.0 : real;
}

qint64 Sw6Value::asInt() const
{
    return kind == Kind::Float ? static_cast<qint64>(real) : whole;
}

QString Sw6Value::asText() const
{
    return kind == Kind::Text ? label : toToken();
}

QString Sw6Value::toToken() const
{
    switch (kind) {
    case Kind::Float:
        return formatFloat(real) + QLatin1Char('f');
    case Kind::Integer:
        return QString::number(whole) + QLatin1Char('d');
    case Kind::Text:
        return label + QLatin1Char('s');
    case Kind::Hex:
        break;
    }
    return QLatin1StringView("0x")
           + QString::number(static_cast<quint32>(whole), 16).toUpper().rightJustified(
               hexDigits, QLatin1Char('0'));
}

quint32 Sw6Value::parse(QStringView token, Sw6Value& value)
{
    if (token.isEmpty()) {
        return err::kMissingArgument;
    }

    if (token.startsWith(QLatin1StringView("0x"), Qt::CaseInsensitive)) {
        return parseHexToken(token, value);
    }

    const QChar suffix = token.back();
    const QStringView body = token.first(token.size() - 1);
    bool ok = false;

    switch (suffix.toLatin1()) {
    case 'f': {
        const double parsed = body.toDouble(&ok);
        if (!ok) {
            return err::kArgumentTypeMismatch;
        }
        value = Sw6Value::ofFloat(parsed);
        return err::kSuccess;
    }
    case 'd': {
        const qint64 parsed = body.toLongLong(&ok);
        if (!ok) {
            return err::kArgumentTypeMismatch;
        }
        value = Sw6Value::ofInt(parsed);
        return err::kSuccess;
    }
    case 's':
        value = Sw6Value::ofText(body.toString());
        return err::kSuccess;
    default:
        break;
    }

    return err::kMissingArgumentType;
}

QString formatFloat(double value)
{
    // 'g' with nine significant digits keeps a float round-trippable while
    // still writing 10.0 as "10", which is the notation the protocol uses.
    return QString::number(value, 'g', 9);
}

quint32 parseValues(const QByteArray& arguments, Sw6Values& values)
{
    values.clear();
    if (arguments.isEmpty()) {
        return err::kSuccess;
    }

    const QString text = QString::fromLatin1(arguments);
    const QList<QStringView> tokens = QStringView(text).split(QLatin1Char(','));
    values.reserve(static_cast<qsizetype>(tokens.size()));

    for (const QStringView token : tokens) {
        Sw6Value value;
        if (const quint32 code = Sw6Value::parse(token, value); code != err::kSuccess) {
            return code;
        }
        values.append(std::move(value));
    }
    return err::kSuccess;
}

QByteArray joinValues(const Sw6Values& values)
{
    QByteArray joined;
    for (const Sw6Value& value : values) {
        if (!joined.isEmpty()) {
            joined.append(',');
        }
        joined.append(value.toToken().toLatin1());
    }
    return joined;
}

// --- Checksums -------------------------------------------------------------

quint8 asciiChecksum(const QByteArray& frameThroughTerminator)
{
    return core::crc::sum8(core::hex::asBytes(frameThroughTerminator));
}

QByteArray buildAsciiFrame(const QString& command, const QByteArray& arguments)
{
    QByteArray frame;
    frame.reserve(command.size() + arguments.size() + 4);
    frame.append(kAsciiStart);
    frame.append(command.toLatin1());
    if (!arguments.isEmpty()) {
        frame.append(',');
        frame.append(arguments);
    }
    frame.append(kAsciiTerminator);

    const quint8 checksum = asciiChecksum(frame);
    frame.append(QByteArray::number(checksum, 16).rightJustified(2, '0').toUpper());
    return frame;
}

} // namespace hwsim::plugins::sw6
