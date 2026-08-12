#include "Sw6Messages.h"

#include <core/Endian.h>
#include <core/HexUtils.h>

using hwsim::core::ErrorCode;
using hwsim::core::makeError;

namespace hwsim::plugins::sw6 {
namespace {

/// Walks a flat argument list into `<element>[,<value>...]` groups. `resolve`
/// turns the leading token of a group into an element index and returns -1 when
/// the token is not one.
template <typename Resolver>
quint32 parseElements(const Sw6Values& values, int valuesPerElement, Resolver resolve,
                      Sw6Elements& elements)
{
    elements.clear();

    qsizetype cursor = 0;
    while (cursor < values.size()) {
        const int index = resolve(values.at(cursor));
        if (index < 0) {
            return err::kBadArgumentValue;
        }
        ++cursor;

        Sw6Element element;
        element.index = index;
        for (int position = 0; position < valuesPerElement; ++position) {
            if (cursor >= values.size()) {
                return err::kMissingArgument;
            }
            if (values.at(cursor).isText()) {
                return err::kArgumentTypeMismatch;
            }
            element.values.append(values.at(cursor));
            ++cursor;
        }
        elements.append(std::move(element));
    }
    return err::kSuccess;
}

int resolveAxis(const Sw6Value& value)
{
    return value.isText() ? axisIndex(value.label) : -1;
}

int resolveLeg(const Sw6Value& value)
{
    if (value.isText()) {
        return -1;
    }
    const qint64 leg = value.asInt();
    return (leg >= 1 && leg <= kLegCount) ? static_cast<int>(leg) : -1;
}

QByteArray encodeElements(const Sw6Elements& elements, bool axisShape)
{
    Sw6Values flat;
    for (const Sw6Element& element : elements) {
        flat.append(axisShape ? Sw6Value::ofText(axisName(element.index))
                              : Sw6Value::ofInt(element.index));
        flat.append(element.values);
    }
    return joinValues(flat);
}

QString describeCommand(const QString& command, const QByteArray& arguments)
{
    return arguments.isEmpty()
               ? command
               : QStringLiteral("%1 %2").arg(command, QString::fromLatin1(arguments));
}

/// Shared body of the positional command types: parse the arguments, then
/// check their count against the spec. An argument the device cannot read is
/// not a decode failure - it travels on so the handler can answer with the
/// error code section 4.6 requires.
template <typename Message, typename Spec>
Result<Message> decodePositional(const Frame& frame, const Spec* spec, QLatin1StringView shape)
{
    if (spec == nullptr) {
        return makeError(ErrorCode::UnknownCommand,
                         QStringLiteral("opcode 0x%1 is not a %2 command")
                             .arg(frame.opcode, 8, 16, QLatin1Char('0'))
                             .arg(shape));
    }

    Message command;
    command.command = commandNameFor(frame.opcode);

    if (const quint32 code = parseValues(frame.payload, command.values); code != err::kSuccess) {
        command.argumentError = code;
        return command;
    }

    const auto count = command.values.size();
    if (count < spec->minValues) {
        command.argumentError = err::kMissingArgument;
    } else if (count > spec->maxValues) {
        command.argumentError = err::kTooManyArguments;
    }
    return command;
}

/// Reply data is a flat list of `<element>`, `<value>` pairs (section 4.6).
/// Some replies put a label in front of the pairs - `$KSTq` echoes the frame
/// name, `$GEOq` the hinge row - so both alignments are tried rather than
/// scanning every position, which could mistake a value for an element id.
template <typename Matcher>
std::optional<double> pairedValue(const Sw6Values& values, Matcher matches)
{
    for (const qsizetype start : {qsizetype{0}, qsizetype{1}}) {
        for (qsizetype index = start; index + 1 < values.size(); index += 2) {
            if (matches(values.at(index))) {
                return values.at(index + 1).asDouble();
            }
        }
    }
    return std::nullopt;
}

} // namespace

// --- AxisCommand -----------------------------------------------------------

std::optional<OpCode> AxisCommand::dynamicOpcode() const
{
    return command.isEmpty() ? std::nullopt : std::optional<OpCode>(commandOpcodeOf(command));
}

Result<AxisCommand> AxisCommand::decode(const Frame& frame)
{
    const AxisCommandSpec* spec = findAxisCommand(frame.opcode);
    if (spec == nullptr) {
        return makeError(ErrorCode::UnknownCommand,
                         QStringLiteral("opcode 0x%1 is not an axis command")
                             .arg(frame.opcode, 8, 16, QLatin1Char('0')));
    }

    AxisCommand command;
    command.command = commandNameFor(frame.opcode);

    Sw6Values values;
    if (const quint32 code = parseValues(frame.payload, values); code != err::kSuccess) {
        command.argumentError = code;
        return command;
    }

    // A query carries bare axis names; only a set carries values after them.
    const int perElement = spec->op == AxisOp::Query ? 0 : spec->valuesPerElement;
    command.argumentError = parseElements(values, perElement, resolveAxis, command.elements);
    return command;
}

Result<QByteArray> AxisCommand::encodeBody(const EncodeContext&) const
{
    return encodeElements(elements, true);
}

QString AxisCommand::describe() const
{
    return describeCommand(command, encodeElements(elements, true));
}

AxisCommand AxisCommand::make(QString name, std::initializer_list<Sw6Element> elements)
{
    AxisCommand command;
    command.command = std::move(name);
    command.elements = Sw6Elements(elements);
    return command;
}

// --- LegCommand ------------------------------------------------------------

std::optional<OpCode> LegCommand::dynamicOpcode() const
{
    return command.isEmpty() ? std::nullopt : std::optional<OpCode>(commandOpcodeOf(command));
}

Result<LegCommand> LegCommand::decode(const Frame& frame)
{
    const LegCommandSpec* spec = findLegCommand(frame.opcode);
    if (spec == nullptr) {
        return makeError(ErrorCode::UnknownCommand,
                         QStringLiteral("opcode 0x%1 is not a leg command")
                             .arg(frame.opcode, 8, 16, QLatin1Char('0')));
    }

    LegCommand command;
    command.command = commandNameFor(frame.opcode);

    Sw6Values values;
    if (const quint32 code = parseValues(frame.payload, values); code != err::kSuccess) {
        command.argumentError = code;
        return command;
    }

    const int perElement = spec->op == LegOp::Set ? spec->valuesPerElement : 0;
    command.argumentError = parseElements(values, perElement, resolveLeg, command.elements);
    return command;
}

Result<QByteArray> LegCommand::encodeBody(const EncodeContext&) const
{
    return encodeElements(elements, false);
}

QString LegCommand::describe() const
{
    return describeCommand(command, encodeElements(elements, false));
}

LegCommand LegCommand::make(QString name, std::initializer_list<Sw6Element> elements)
{
    LegCommand command;
    command.command = std::move(name);
    command.elements = Sw6Elements(elements);
    return command;
}

// --- ChannelCommand --------------------------------------------------------

std::optional<OpCode> ChannelCommand::dynamicOpcode() const
{
    return command.isEmpty() ? std::nullopt : std::optional<OpCode>(commandOpcodeOf(command));
}

Result<ChannelCommand> ChannelCommand::decode(const Frame& frame)
{
    const ChannelCommandSpec* spec = findChannelCommand(frame.opcode);
    if (spec == nullptr) {
        return makeError(ErrorCode::UnknownCommand,
                         QStringLiteral("opcode 0x%1 is not a channel command")
                             .arg(frame.opcode, 8, 16, QLatin1Char('0')));
    }

    ChannelCommand command;
    command.command = commandNameFor(frame.opcode);

    Sw6Values values;
    if (const quint32 code = parseValues(frame.payload, values); code != err::kSuccess) {
        command.argumentError = code;
        return command;
    }

    const ChannelRange range = channelRangeOf(spec->field);
    const int perElement =
        (spec->op == ChannelOp::Set || spec->op == ChannelOp::SetConfig) ? spec->valuesPerElement
                                                                        : 0;
    command.argumentError = parseElements(
        values, perElement,
        [range](const Sw6Value& value) {
            return !value.isText() && range.contains(value.asInt())
                       ? static_cast<int>(value.asInt())
                       : -1;
        },
        command.elements);
    return command;
}

Result<QByteArray> ChannelCommand::encodeBody(const EncodeContext&) const
{
    return encodeElements(elements, false);
}

QString ChannelCommand::describe() const
{
    return describeCommand(command, encodeElements(elements, false));
}

ChannelCommand ChannelCommand::make(QString name, std::initializer_list<Sw6Element> elements)
{
    ChannelCommand command;
    command.command = std::move(name);
    command.elements = Sw6Elements(elements);
    return command;
}

// --- SystemCommand ---------------------------------------------------------

std::optional<OpCode> SystemCommand::dynamicOpcode() const
{
    return command.isEmpty() ? std::nullopt : std::optional<OpCode>(commandOpcodeOf(command));
}

Result<SystemCommand> SystemCommand::decode(const Frame& frame)
{
    return decodePositional<SystemCommand>(frame, findSystemCommand(frame.opcode),
                                           QLatin1StringView("system"));
}

Result<QByteArray> SystemCommand::encodeBody(const EncodeContext&) const
{
    return joinValues(values);
}

QString SystemCommand::describe() const
{
    return describeCommand(command, joinValues(values));
}

SystemCommand SystemCommand::make(QString name, Sw6Values values)
{
    SystemCommand command;
    command.command = std::move(name);
    command.values = std::move(values);
    return command;
}

// --- CoordinateCommand -----------------------------------------------------

std::optional<OpCode> CoordinateCommand::dynamicOpcode() const
{
    return command.isEmpty() ? std::nullopt : std::optional<OpCode>(commandOpcodeOf(command));
}

Result<CoordinateCommand> CoordinateCommand::decode(const Frame& frame)
{
    return decodePositional<CoordinateCommand>(frame, findCoordinateCommand(frame.opcode),
                                               QLatin1StringView("coordinate"));
}

Result<QByteArray> CoordinateCommand::encodeBody(const EncodeContext&) const
{
    return joinValues(values);
}

QString CoordinateCommand::describe() const
{
    return describeCommand(command, joinValues(values));
}

CoordinateCommand CoordinateCommand::make(QString name, Sw6Values values)
{
    CoordinateCommand command;
    command.command = std::move(name);
    command.values = std::move(values);
    return command;
}

// --- NamedCommand ----------------------------------------------------------

std::optional<OpCode> NamedCommand::dynamicOpcode() const
{
    return command.isEmpty() ? std::nullopt : std::optional<OpCode>(commandOpcodeOf(command));
}

Result<NamedCommand> NamedCommand::decode(const Frame& frame)
{
    return decodePositional<NamedCommand>(frame, findNamedCommand(frame.opcode),
                                          QLatin1StringView("named-parameter"));
}

Result<QByteArray> NamedCommand::encodeBody(const EncodeContext&) const
{
    return joinValues(values);
}

QString NamedCommand::describe() const
{
    return describeCommand(command, joinValues(values));
}

NamedCommand NamedCommand::make(QString name, Sw6Values values)
{
    NamedCommand command;
    command.command = std::move(name);
    command.values = std::move(values);
    return command;
}

// --- TrajectoryCommand -----------------------------------------------------

std::optional<OpCode> TrajectoryCommand::dynamicOpcode() const
{
    return command.isEmpty() ? std::nullopt : std::optional<OpCode>(commandOpcodeOf(command));
}

Result<TrajectoryCommand> TrajectoryCommand::decode(const Frame& frame)
{
    return decodePositional<TrajectoryCommand>(frame, findTrajectoryCommand(frame.opcode),
                                               QLatin1StringView("trajectory"));
}

Result<QByteArray> TrajectoryCommand::encodeBody(const EncodeContext&) const
{
    return joinValues(values);
}

QString TrajectoryCommand::describe() const
{
    return describeCommand(command, joinValues(values));
}

TrajectoryCommand TrajectoryCommand::make(QString name, Sw6Values values)
{
    TrajectoryCommand command;
    command.command = std::move(name);
    command.values = std::move(values);
    return command;
}

// --- AlignmentCommand ------------------------------------------------------

std::optional<OpCode> AlignmentCommand::dynamicOpcode() const
{
    return command.isEmpty() ? std::nullopt : std::optional<OpCode>(commandOpcodeOf(command));
}

Result<AlignmentCommand> AlignmentCommand::decode(const Frame& frame)
{
    return decodePositional<AlignmentCommand>(frame, findAlignmentCommand(frame.opcode),
                                              QLatin1StringView("alignment"));
}

Result<QByteArray> AlignmentCommand::encodeBody(const EncodeContext&) const
{
    return joinValues(values);
}

QString AlignmentCommand::describe() const
{
    return describeCommand(command, joinValues(values));
}

AlignmentCommand AlignmentCommand::make(QString name, Sw6Values values)
{
    AlignmentCommand command;
    command.command = std::move(name);
    command.values = std::move(values);
    return command;
}

// --- UnknownCommand --------------------------------------------------------

Result<UnknownCommand> UnknownCommand::decode(const Frame& frame)
{
    UnknownCommand command;
    command.command = frame.attribute(QString::fromLatin1(kCommandAttribute)).toString();
    return command;
}

QString UnknownCommand::describe() const
{
    return QStringLiteral("Unknown command %1").arg(command);
}

// --- Sw6Reply --------------------------------------------------------------

std::optional<OpCode> Sw6Reply::dynamicOpcode() const
{
    return command.isEmpty() ? std::nullopt : std::optional<OpCode>(commandOpcodeOf(command));
}

Result<Sw6Reply> Sw6Reply::decode(const Frame& frame)
{
    Sw6Reply reply;
    reply.command = commandNameFor(frame.opcode);
    if (reply.command.isEmpty()) {
        reply.command = frame.attribute(QString::fromLatin1(kCommandAttribute)).toString();
    }

    Sw6Values values;
    if (const quint32 code = parseValues(frame.payload, values); code != err::kSuccess) {
        return makeError(ErrorCode::FrameMalformed,
                         QStringLiteral("reply to %1 has an unreadable argument (0x%2)")
                             .arg(reply.command)
                             .arg(code, 8, 16, QLatin1Char('0')));
    }

    if (values.isEmpty() || values.first().kind != Sw6Value::Kind::Hex) {
        return makeError(ErrorCode::FrameMalformed,
                         QStringLiteral("reply to %1 carries no error code").arg(reply.command));
    }

    reply.errorCode = static_cast<quint32>(values.first().whole);
    reply.values = values.mid(1);
    return reply;
}

Result<QByteArray> Sw6Reply::encodeBody(const EncodeContext&) const
{
    Sw6Values body;
    body.reserve(values.size() + 1);
    body.append(Sw6Value::ofHex(errorCode));
    body.append(values);
    return joinValues(body);
}

QString Sw6Reply::describe() const
{
    const QString code = QStringLiteral("0x%1").arg(errorCode, 8, 16, QLatin1Char('0')).toUpper();
    const QByteArray data = joinValues(values);
    return data.isEmpty()
               ? QStringLiteral("%1 → %2").arg(command, code)
               : QStringLiteral("%1 → %2 %3").arg(command, code, QString::fromLatin1(data));
}

std::optional<double> Sw6Reply::valueForAxis(int axis) const
{
    const QString name = axisName(axis);
    return pairedValue(values, [&name](const Sw6Value& value) {
        return value.isText() && value.label.compare(name, Qt::CaseInsensitive) == 0;
    });
}

std::optional<double> Sw6Reply::valueForLeg(int leg) const
{
    return pairedValue(values, [leg](const Sw6Value& value) {
        return !value.isText() && value.asInt() == leg;
    });
}

MessagePtr Sw6Reply::ack(QString command)
{
    auto reply = std::make_shared<Sw6Reply>();
    reply->command = std::move(command);
    return reply;
}

MessagePtr Sw6Reply::failure(QString command, quint32 code)
{
    auto reply = std::make_shared<Sw6Reply>();
    reply->command = std::move(command);
    reply->errorCode = code;
    return reply;
}

MessagePtr Sw6Reply::data(QString command, Sw6Values values)
{
    auto reply = std::make_shared<Sw6Reply>();
    reply->command = std::move(command);
    reply->values = std::move(values);
    return reply;
}

// --- Realtime stream -------------------------------------------------------

namespace stream {

int expectedRecordCount(quint8 mask)
{
    int count = kAxisCount;
    if ((mask & kMaskTheoreticalPose) != 0) {
        count += kAxisCount;
    }
    if ((mask & kMaskActualLength) != 0) {
        count += kLegCount;
    }
    if ((mask & kMaskTheoreticalLength) != 0) {
        count += kLegCount;
    }
    if ((mask & kMaskLegSpeed) != 0) {
        count += kLegCount;
    }
    if ((mask & kMaskAnalog) != 0) {
        count += kAnalogChannels;
    }
    return count;
}

int expectedFrameBytes(quint8 mask)
{
    return static_cast<int>(kStreamOverheadBytes
                            + static_cast<std::size_t>(expectedRecordCount(mask))
                                  * kStreamRecordBytes);
}

} // namespace stream

namespace {

/// Appends one block of consecutive type codes, scaling engineering units to
/// the integer counts of section 6.2.
template <std::size_t N>
void appendBlock(QVector<Sw6StreamRecord>& records, quint8 baseType,
                 const std::array<double, N>& block)
{
    for (std::size_t index = 0; index < N; ++index) {
        Sw6StreamRecord record;
        record.type = static_cast<quint8>(baseType + index);
        record.value = static_cast<qint32>(block[index] * stream::kScale);
        records.append(record);
    }
}

/// Reverse of appendBlock: returns true when `type` belongs to the block.
template <std::size_t N>
bool readBlock(quint8 baseType, const Sw6StreamRecord& record, std::array<double, N>& block)
{
    const auto base = static_cast<std::size_t>(baseType);
    const auto type = static_cast<std::size_t>(record.type);
    if (type < base || type >= base + N) {
        return false;
    }
    block[type - base] = static_cast<double>(record.value) / stream::kScale;
    return true;
}

} // namespace

Result<RealtimeFrame> RealtimeFrame::decode(const Frame& frame)
{
    if (frame.payload.size() % static_cast<qsizetype>(kStreamRecordBytes) != 0) {
        return makeError(ErrorCode::FrameMalformed,
                         QStringLiteral("realtime payload of %1 bytes is not a multiple of 5")
                             .arg(frame.payload.size()));
    }

    const std::span<const std::byte> bytes = core::hex::asBytes(frame.payload);

    RealtimeFrame decoded;
    decoded.records.reserve(frame.payload.size() / static_cast<qsizetype>(kStreamRecordBytes));

    std::array<quint64, 4> seen{};
    for (std::size_t offset = 0; offset < bytes.size(); offset += kStreamRecordBytes) {
        Sw6StreamRecord record;
        record.type = std::to_integer<quint8>(bytes[offset]);
        record.value = core::endian::readLittle<qint32>(bytes.subspan(offset + 1, 4));

        // Section 6.2: a type may appear at most once per frame, so a repeat
        // means the sender and the mask disagree and the frame is unusable.
        const quint64 bit = quint64{1} << (record.type % 64);
        quint64& word = seen[record.type / 64];
        if ((word & bit) != 0) {
            return makeError(ErrorCode::FrameMalformed,
                             QStringLiteral("realtime type 0x%1 appears twice")
                                 .arg(record.type, 2, 16, QLatin1Char('0')));
        }
        word |= bit;

        decoded.records.append(record);
    }
    return decoded;
}

Result<QByteArray> RealtimeFrame::encodeBody(const EncodeContext&) const
{
    QByteArray body;
    body.resize(static_cast<qsizetype>(records.size() * static_cast<qsizetype>(kStreamRecordBytes)));

    auto* cursor = reinterpret_cast<std::byte*>(body.data());
    for (const Sw6StreamRecord& record : records) {
        cursor[0] = static_cast<std::byte>(record.type);
        core::endian::writeLittle<qint32>(std::span<std::byte>(cursor + 1, 4), record.value);
        cursor += kStreamRecordBytes;
    }
    return body;
}

QString RealtimeFrame::describe() const
{
    return QStringLiteral("Realtime stream, %1 record(s)").arg(records.size());
}

RealtimeFrame RealtimeFrame::fromSample(const Sw6StreamSample& sample)
{
    RealtimeFrame frame;
    frame.records.reserve(stream::expectedRecordCount(sample.mask));

    appendBlock(frame.records, stream::kActualPoseBase, sample.actualPose);
    if ((sample.mask & stream::kMaskTheoreticalPose) != 0) {
        appendBlock(frame.records, stream::kTheoreticalPoseBase, sample.theoreticalPose);
    }
    if ((sample.mask & stream::kMaskActualLength) != 0) {
        appendBlock(frame.records, stream::kActualLengthBase, sample.actualLength);
    }
    if ((sample.mask & stream::kMaskTheoreticalLength) != 0) {
        appendBlock(frame.records, stream::kTheoreticalLengthBase, sample.theoreticalLength);
    }
    if ((sample.mask & stream::kMaskLegSpeed) != 0) {
        appendBlock(frame.records, stream::kLegSpeedBase, sample.legSpeed);
    }
    if ((sample.mask & stream::kMaskAnalog) != 0) {
        appendBlock(frame.records, stream::kAnalogBase, sample.analog);
    }
    return frame;
}

Sw6StreamSample RealtimeFrame::toSample() const
{
    Sw6StreamSample sample;
    for (const Sw6StreamRecord& record : records) {
        if (readBlock(stream::kActualPoseBase, record, sample.actualPose)) {
            continue;
        }
        if (readBlock(stream::kTheoreticalPoseBase, record, sample.theoreticalPose)) {
            sample.mask |= stream::kMaskTheoreticalPose;
            continue;
        }
        if (readBlock(stream::kActualLengthBase, record, sample.actualLength)) {
            sample.mask |= stream::kMaskActualLength;
            continue;
        }
        if (readBlock(stream::kTheoreticalLengthBase, record, sample.theoreticalLength)) {
            sample.mask |= stream::kMaskTheoreticalLength;
            continue;
        }
        if (readBlock(stream::kLegSpeedBase, record, sample.legSpeed)) {
            sample.mask |= stream::kMaskLegSpeed;
            continue;
        }
        if (readBlock(stream::kAnalogBase, record, sample.analog)) {
            sample.mask |= stream::kMaskAnalog;
        }
    }
    return sample;
}

} // namespace hwsim::plugins::sw6
