#pragma once

// -----------------------------------------------------------------------------
// Message types.
//
// There are five of them for roughly two hundred commands, because SW6 commands
// differ in meaning rather than in shape. Each instance carries the command
// name it was decoded from and returns it through dynamicOpcode(), so one type
// can be bound to as many opcodes as there are command names of that shape.
//
// Arguments that do not parse do not fail decoding: section 4.6 says the device
// answers `$<Cmd>,0x<ERRCODE>;XX`, so the offending code travels inside the
// message and the handler turns it into that reply.
// -----------------------------------------------------------------------------

#include "Sw6Commands.h"
#include "Sw6Types.h"

#include <protocol/IMessage.h>

#include <array>
#include <optional>

namespace hwsim::plugins::sw6 {

using hwsim::protocol::EncodeContext;
using hwsim::protocol::Frame;
using hwsim::protocol::MessageBase;
using hwsim::protocol::MessagePtr;

/// One `<element>[,<value>...]` group: an axis with its value, or a leg number
/// with one or two values.
struct Sw6Element {
    int index{0};
    Sw6Values values;
};

using Sw6Elements = QVector<Sw6Element>;

// --- Requests --------------------------------------------------------------

/// Anything addressed by platform axis: `$MOV,Xs,1.5f`, `$VELq,Xs`, `$GOH`.
/// No elements means "all six axes", which is the protocol-wide default (4.5).
struct AxisCommand : MessageBase<AxisCommand> {
    QString command;
    Sw6Elements elements;
    quint32 argumentError{err::kSuccess};

    [[nodiscard]] static constexpr OpCode opcode() { return kAxisCommandOpcode; }
    [[nodiscard]] std::optional<OpCode> dynamicOpcode() const override;

    [[nodiscard]] static Result<AxisCommand> decode(const Frame& frame);
    [[nodiscard]] Result<QByteArray> encodeBody(const EncodeContext& context) const;
    [[nodiscard]] QString describe() const override;

    [[nodiscard]] static AxisCommand make(QString name, std::initializer_list<Sw6Element> elements);
};

/// Anything addressed by physical leg 1..6: `$LLC,1d,0.12f`, `$FRFq`.
struct LegCommand : MessageBase<LegCommand> {
    QString command;
    Sw6Elements elements;
    quint32 argumentError{err::kSuccess};

    [[nodiscard]] static constexpr OpCode opcode() { return kLegCommandOpcode; }
    [[nodiscard]] std::optional<OpCode> dynamicOpcode() const override;

    [[nodiscard]] static Result<LegCommand> decode(const Frame& frame);
    [[nodiscard]] Result<QByteArray> encodeBody(const EncodeContext& context) const;
    [[nodiscard]] QString describe() const override;

    [[nodiscard]] static LegCommand make(QString name, std::initializer_list<Sw6Element> elements);
};

/// Sections 5.9 and 5.10, addressed by analog channel or I/O line: `$TAVq,0d`,
/// `$DIO,1d,1d`, `$CTO,1d,3d,1f`. The element numbering a command accepts
/// comes from its field, so a line number the device does not have is rejected
/// while decoding.
struct ChannelCommand : MessageBase<ChannelCommand> {
    QString command;
    Sw6Elements elements;
    quint32 argumentError{err::kSuccess};

    [[nodiscard]] static constexpr OpCode opcode() { return kChannelCommandOpcode; }
    [[nodiscard]] std::optional<OpCode> dynamicOpcode() const override;

    [[nodiscard]] static Result<ChannelCommand> decode(const Frame& frame);
    [[nodiscard]] Result<QByteArray> encodeBody(const EncodeContext& context) const;
    [[nodiscard]] QString describe() const override;

    [[nodiscard]] static ChannelCommand make(QString name,
                                             std::initializer_list<Sw6Element> elements);
};

/// Commands with no element: identity, status, stops, the stream mask.
struct SystemCommand : MessageBase<SystemCommand> {
    QString command;
    Sw6Values values;
    quint32 argumentError{err::kSuccess};

    [[nodiscard]] static constexpr OpCode opcode() { return kSystemCommandOpcode; }
    [[nodiscard]] std::optional<OpCode> dynamicOpcode() const override;

    [[nodiscard]] static Result<SystemCommand> decode(const Frame& frame);
    [[nodiscard]] Result<QByteArray> encodeBody(const EncodeContext& context) const;
    [[nodiscard]] QString describe() const override;

    [[nodiscard]] static SystemCommand make(QString name, Sw6Values values = {});
};

/// Section 5.7, the named tool and work frames. Their arguments are positional
/// (`<类型>s,<name>s,...`) rather than element-wise, so the message keeps the
/// argument list as it arrived and the handler reads it by position.
struct CoordinateCommand : MessageBase<CoordinateCommand> {
    QString command;
    Sw6Values values;
    quint32 argumentError{err::kSuccess};

    [[nodiscard]] static constexpr OpCode opcode() { return kCoordinateCommandOpcode; }
    [[nodiscard]] std::optional<OpCode> dynamicOpcode() const override;

    [[nodiscard]] static Result<CoordinateCommand> decode(const Frame& frame);
    [[nodiscard]] Result<QByteArray> encodeBody(const EncodeContext& context) const;
    [[nodiscard]] QString describe() const override;

    [[nodiscard]] static CoordinateCommand make(QString name, Sw6Values values = {});
};

/// Sections 5.8 and 5.8.1: the named parameter table and the read-only
/// kinematic and geometry registries.
struct NamedCommand : MessageBase<NamedCommand> {
    QString command;
    Sw6Values values;
    quint32 argumentError{err::kSuccess};

    [[nodiscard]] static constexpr OpCode opcode() { return kNamedCommandOpcode; }
    [[nodiscard]] std::optional<OpCode> dynamicOpcode() const override;

    [[nodiscard]] static Result<NamedCommand> decode(const Frame& frame);
    [[nodiscard]] Result<QByteArray> encodeBody(const EncodeContext& context) const;
    [[nodiscard]] QString describe() const override;

    [[nodiscard]] static NamedCommand make(QString name, Sw6Values values = {});
};

/// Section 5.12. A trajectory command names its trajectory first and then
/// carries positional arguments whose meaning differs per command, so like the
/// coordinate systems it keeps the list as it arrived.
struct TrajectoryCommand : MessageBase<TrajectoryCommand> {
    QString command;
    Sw6Values values;
    quint32 argumentError{err::kSuccess};

    [[nodiscard]] static constexpr OpCode opcode() { return kTrajectoryCommandOpcode; }
    [[nodiscard]] std::optional<OpCode> dynamicOpcode() const override;

    [[nodiscard]] static Result<TrajectoryCommand> decode(const Frame& frame);
    [[nodiscard]] Result<QByteArray> encodeBody(const EncodeContext& context) const;
    [[nodiscard]] QString describe() const override;

    [[nodiscard]] static TrajectoryCommand make(QString name, Sw6Values values = {});
};

/// Section 5.15. Named searches, axis scans and identification sweeps: the
/// leading arguments differ per command and the trailing ones are
/// firmware-defined, so the handler reads the list by position.
struct AlignmentCommand : MessageBase<AlignmentCommand> {
    QString command;
    Sw6Values values;
    quint32 argumentError{err::kSuccess};

    [[nodiscard]] static constexpr OpCode opcode() { return kAlignmentCommandOpcode; }
    [[nodiscard]] std::optional<OpCode> dynamicOpcode() const override;

    [[nodiscard]] static Result<AlignmentCommand> decode(const Frame& frame);
    [[nodiscard]] Result<QByteArray> encodeBody(const EncodeContext& context) const;
    [[nodiscard]] QString describe() const override;

    [[nodiscard]] static AlignmentCommand make(QString name, Sw6Values values = {});
};

/// A well-formed frame whose command name is not in the table. Decoded so the
/// device can answer `0x03000001` instead of staying silent.
struct UnknownCommand : MessageBase<UnknownCommand> {
    QString command;

    [[nodiscard]] static constexpr OpCode opcode() { return kUnknownCommandOpcode; }
    [[nodiscard]] static Result<UnknownCommand> decode(const Frame& frame);
    [[nodiscard]] QString describe() const override;
};

// --- Reply -----------------------------------------------------------------

/// Section 4.6: `$<Cmd>,0x<ERRCODE>[,<data>...]`. One type for every reply,
/// success or failure, because the command name travels with the instance.
struct Sw6Reply : MessageBase<Sw6Reply> {
    QString command;
    quint32 errorCode{err::kSuccess};
    Sw6Values values;

    [[nodiscard]] static constexpr OpCode opcode() { return kReplyOpcode; }
    [[nodiscard]] std::optional<OpCode> dynamicOpcode() const override;

    [[nodiscard]] static Result<Sw6Reply> decode(const Frame& frame);
    [[nodiscard]] Result<QByteArray> encodeBody(const EncodeContext& context) const;
    [[nodiscard]] QString describe() const override;

    [[nodiscard]] bool succeeded() const noexcept { return errorCode == err::kSuccess; }

    /// Values of an element-wise query reply, as `<element>` / value pairs.
    [[nodiscard]] std::optional<double> valueForAxis(int axis) const;
    [[nodiscard]] std::optional<double> valueForLeg(int leg) const;

    [[nodiscard]] static MessagePtr ack(QString command);
    [[nodiscard]] static MessagePtr failure(QString command, quint32 code);
    [[nodiscard]] static MessagePtr data(QString command, Sw6Values values);
};

// --- Realtime stream (0x81) ------------------------------------------------

namespace stream {

inline constexpr quint8 kActualPoseBase = 0x01;
inline constexpr quint8 kTheoreticalPoseBase = 0x07;
inline constexpr quint8 kActualLengthBase = 0x0D;
inline constexpr quint8 kTheoreticalLengthBase = 0x13;
inline constexpr quint8 kLegSpeedBase = 0x19;
inline constexpr quint8 kAnalogBase = 0x1F;
inline constexpr quint8 kHighestType = 0x26;

inline constexpr int kAnalogChannels = 8;

/// Section 6.3 mask bits. The actual pose block is always sent and owns no bit.
inline constexpr quint8 kMaskTheoreticalPose = 0x01;
inline constexpr quint8 kMaskActualLength = 0x02;
inline constexpr quint8 kMaskTheoreticalLength = 0x04;
inline constexpr quint8 kMaskLegSpeed = 0x08;
inline constexpr quint8 kMaskAnalog = 0x10;
inline constexpr quint8 kMaskKnownBits = 0x1F;

/// Engineering value per raw count: nm, µrad and µV all scale by 1e-6.
inline constexpr double kScale = 1.0e6;

/// Records a frame must carry for a given mask, which is what section 6.1 tells
/// the receiver to check the length field against.
[[nodiscard]] int expectedRecordCount(quint8 mask);

/// Total frame length in bytes, including header, CRC and tail.
[[nodiscard]] int expectedFrameBytes(quint8 mask);

} // namespace stream

/// One 5-byte typed record: type code plus a little-endian int32.
struct Sw6StreamRecord {
    quint8 type{0};
    qint32 value{0};
};

/// The realtime blocks in engineering units, which is what the device state
/// produces and what a master wants after decoding.
struct Sw6StreamSample {
    quint8 mask{0};
    std::array<double, kAxisCount> actualPose{};
    std::array<double, kAxisCount> theoreticalPose{};
    std::array<double, kLegCount> actualLength{};
    std::array<double, kLegCount> theoreticalLength{};
    std::array<double, kLegCount> legSpeed{};
    std::array<double, stream::kAnalogChannels> analog{};
};

/// The whole 0x81 frame as one message. The 38 type codes are not separate
/// messages because they have no frame boundary of their own: they are records
/// inside this one frame, and a receiver only ever sees them together.
struct RealtimeFrame : MessageBase<RealtimeFrame> {
    QVector<Sw6StreamRecord> records;

    [[nodiscard]] static constexpr OpCode opcode() { return kRealtimeStreamOpcode; }

    [[nodiscard]] static Result<RealtimeFrame> decode(const Frame& frame);
    [[nodiscard]] Result<QByteArray> encodeBody(const EncodeContext& context) const;
    [[nodiscard]] QString describe() const override;

    [[nodiscard]] static RealtimeFrame fromSample(const Sw6StreamSample& sample);
    [[nodiscard]] Sw6StreamSample toSample() const;
};

} // namespace hwsim::plugins::sw6
