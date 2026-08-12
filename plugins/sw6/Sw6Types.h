#pragma once

// -----------------------------------------------------------------------------
// Shared vocabulary of the SW6 hexapod protocol: wire constants, the command
// name hash that turns a name into an OpCode, the typed argument token, and the
// two checksums.
//
// SW6 puts two frame families on one link and tells them apart by the first
// byte (protocol section 3):
//
//   '$' (0x24)  ASCII command / reply, checksum-8, strictly question-answer
//   0x81        binary realtime stream, CRC-16/UMTS, pushed by the device
//
// Both are handled by a single codec, because a session owns exactly one codec
// and splitting them would make each side discard the other's frames as noise.
// -----------------------------------------------------------------------------

#include <core/Result.h>
#include <protocol/ProtocolTypes.h>

#include <QByteArray>
#include <QString>
#include <QStringView>
#include <QVector>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace hwsim::plugins::sw6 {

using hwsim::core::Result;
using hwsim::protocol::OpCode;

// --- Wire constants --------------------------------------------------------

inline constexpr char kAsciiStart = '$';
inline constexpr char kAsciiTerminator = ';';
inline constexpr std::uint8_t kStreamStart = 0x81;
inline constexpr std::uint8_t kStreamEnd = 0x55;

/// Section 4.1: a command name is 1..15 letters and a frame never exceeds 256
/// bytes, which is what bounds the search for the terminator.
inline constexpr int kMaxCommandNameLength = 15;
inline constexpr std::size_t kMaxAsciiFrameBytes = 256;

/// Section 6.1: 0x81 | length | length bytes of records | crc16 | 0x55.
inline constexpr std::size_t kStreamRecordBytes = 5;
inline constexpr std::size_t kStreamOverheadBytes = 5;

/// Carries the ASCII command name from scan() to wrap(), so a reply can echo
/// the name even when it is one the command table does not know.
inline constexpr auto kCommandAttribute = "sw6.command";

// --- Opcodes ---------------------------------------------------------------

/// SW6 identifies commands by name, and the framework by a 32-bit OpCode, so
/// the name is hashed at compile time. Bit 31 is cleared to keep the hash space
/// disjoint from the reserved opcodes below, which stand for frames that carry
/// no command name.
[[nodiscard]] constexpr OpCode commandOpcode(std::string_view name) noexcept
{
    std::uint32_t hash = 2166136261u;
    for (const char character : name) {
        hash ^= static_cast<std::uint8_t>(character);
        hash *= 16777619u;
    }
    return static_cast<OpCode>(hash & 0x7FFFFFFFu);
}

[[nodiscard]] OpCode commandOpcodeOf(QStringView name);

inline constexpr OpCode kRealtimeStreamOpcode = 0x80000001u;
inline constexpr OpCode kUnknownCommandOpcode = 0x80000002u;

/// Fallback opcodes for the request message types. Every instance overrides
/// them through dynamicOpcode(), because one message type serves many command
/// names; the registry still wants a value to bind the encoder with.
inline constexpr OpCode kAxisCommandOpcode = 0x80000010u;
inline constexpr OpCode kLegCommandOpcode = 0x80000011u;
inline constexpr OpCode kSystemCommandOpcode = 0x80000012u;
inline constexpr OpCode kReplyOpcode = 0x80000013u;
inline constexpr OpCode kCoordinateCommandOpcode = 0x80000014u;
inline constexpr OpCode kNamedCommandOpcode = 0x80000015u;
inline constexpr OpCode kChannelCommandOpcode = 0x80000016u;
inline constexpr OpCode kTrajectoryCommandOpcode = 0x80000017u;
inline constexpr OpCode kAlignmentCommandOpcode = 0x80000018u;

// --- Error codes (section 4.7) ---------------------------------------------

namespace err {

inline constexpr quint32 kSuccess = 0x00000000u;
inline constexpr quint32 kInternalConfig = 0x01000000u;
inline constexpr quint32 kUnknownCommandName = 0x03000001u;
inline constexpr quint32 kEmptyCommandName = 0x03000002u;
inline constexpr quint32 kCommandNameOverflow = 0x03000003u;
inline constexpr quint32 kMissingArgument = 0x03000004u;
inline constexpr quint32 kTooManyArguments = 0x03000005u;
inline constexpr quint32 kMissingArgumentType = 0x03000009u;
inline constexpr quint32 kArgumentTypeMismatch = 0x0300000Bu;
inline constexpr quint32 kBadArgumentValue = 0x0300000Cu;
inline constexpr quint32 kServoOff = 0x04000001u;
inline constexpr quint32 kNotReferenced = 0x04000002u;
inline constexpr quint32 kSoftLimitExceeded = 0x04000003u;
inline constexpr quint32 kRejectedWhileMoving = 0x04000004u;
inline constexpr quint32 kEmergencyStopped = 0x04000005u;
inline constexpr quint32 kNoSuchEntry = 0x04000006u;

} // namespace err

// --- Status word (section 4.8) ---------------------------------------------

namespace status {

inline constexpr quint32 kMoving = 0x00000001u;
inline constexpr quint32 kOnTarget = 0x00000002u;
inline constexpr quint32 kReferenced = 0x00000004u;
inline constexpr quint32 kError = 0x00000008u;
inline constexpr quint32 kWaveformRunning = 0x00000010u;
inline constexpr quint32 kEmergencyStop = 0x00000020u;
inline constexpr quint32 kAlignmentRunning = 0x00000040u;

} // namespace status

// --- Axes and legs ---------------------------------------------------------

inline constexpr int kAxisCount = 6;
inline constexpr int kLegCount = 6;

/// Section 6.2 gives the realtime stream eight AD channels numbered 0..7, and
/// `$TAVq` and friends of section 5.9 address those same eight.
inline constexpr int kAnalogChannelCount = 8;

/// Digital output lines (`$DIO`) and trigger lines (`$TRO` / `$TRI`), both
/// numbered from 1. Section 5.10 leaves the counts to the firmware; these are
/// what this device model offers, and `$TIOq` reports the trigger count.
inline constexpr int kDigitalLineCount = 8;
inline constexpr int kTriggerLineCount = 4;

/// Parameter ids `$CTO` / `$CTI` accept. Their meaning is firmware-defined, so
/// the device only stores what it is told.
inline constexpr int kTriggerParameterCount = 8;

/// Widest element numbering above, which is what the per-channel state arrays
/// are sized for.
inline constexpr int kChannelSlotCount = 8;

/// Section 5.12 buffers trajectories in the controller. Both counts are this
/// device model's; `$TGIq` reports the capacity that is left.
inline constexpr int kTrajectoryCount = 4;
inline constexpr int kTrajectoryCapacity = 32;

inline constexpr std::array<std::string_view, kAxisCount> kAxisNames{"X", "Y", "Z", "U", "V", "W"};

/// Index 0..5 for X,Y,Z,U,V,W, or -1 when the name is not an axis.
[[nodiscard]] int axisIndex(QStringView name);
[[nodiscard]] QString axisName(int index);

// --- Argument tokens (section 4.3) -----------------------------------------

/// One `<value><suffix>` argument: `1.5f`, `100d`, `Xs`, or a bare `0x00000000`
/// error / status word.
struct Sw6Value {
    enum class Kind : quint8 { Float, Integer, Text, Hex };

    Kind kind{Kind::Float};
    double real{0.0};
    qint64 whole{0};
    QString label;

    /// Hex tokens are fixed width on the wire: 8 digits for error and status
    /// words, 2 for the single-byte realtime stream mask.
    quint8 hexDigits{8};

    [[nodiscard]] static Sw6Value ofFloat(double value);
    [[nodiscard]] static Sw6Value ofInt(qint64 value);
    [[nodiscard]] static Sw6Value ofText(QString value);
    [[nodiscard]] static Sw6Value ofHex(quint32 value, quint8 digits = 8);

    [[nodiscard]] bool isText() const noexcept { return kind == Kind::Text; }
    [[nodiscard]] double asDouble() const;
    [[nodiscard]] qint64 asInt() const;
    [[nodiscard]] QString asText() const;

    [[nodiscard]] QString toToken() const;

    /// Returns an SW6 error code from section 4.7 rather than a core::Error:
    /// a badly typed argument is not a framing failure but something the
    /// device has to answer with `$<Cmd>,0x0300000B;XX`, so the code has to
    /// survive all the way to the reply.
    [[nodiscard]] static quint32 parse(QStringView token, Sw6Value& value);
};

using Sw6Values = QVector<Sw6Value>;

/// Shortest round-trippable rendering, so 10.0 is written `10` and not `10.000`
/// as the protocol examples require.
[[nodiscard]] QString formatFloat(double value);

/// Splits a comma separated argument field. An empty field yields no values.
/// Returns err::kSuccess or the offending argument's error code.
[[nodiscard]] quint32 parseValues(const QByteArray& arguments, Sw6Values& values);
[[nodiscard]] QByteArray joinValues(const Sw6Values& values);

// --- Checksums -------------------------------------------------------------

/// Section 4.2: sum of every byte from '$' up to and including ';'.
[[nodiscard]] quint8 asciiChecksum(const QByteArray& frameThroughTerminator);

/// Builds `$<name>[,<arguments>];<XX>`. Also used by the tests to produce the
/// golden frames from section 9.2.
[[nodiscard]] QByteArray buildAsciiFrame(const QString& command, const QByteArray& arguments = {});

} // namespace hwsim::plugins::sw6
