#pragma once

// -----------------------------------------------------------------------------
// One codec, two framings.
//
// Both SW6 frame families share the link and are told apart by the first byte,
// so they have to share the codec too: a session owns exactly one, and a codec
// that only understood one family would drop every frame of the other while
// resynchronising.
//
//   '$'   `$<CmdName>[,<arg>...];<XX>`      checksum-8 over '$'..';'
//   0x81  `81 <len> <records> <crc16> 55`   CRC-16/UMTS over header and records
// -----------------------------------------------------------------------------

#include "Sw6Types.h"

#include <core/ConfigSchema.h>
#include <protocol/IFrameCodec.h>

#include <QHash>

namespace hwsim::plugins::sw6 {

using hwsim::protocol::EncodeContext;
using hwsim::protocol::Frame;
using hwsim::protocol::FrameScanResult;
using hwsim::protocol::IFrameCodec;

/// Correlation token allocated by prepareRequest() and written into the
/// encode context, so wrap() can associate it with the command name it ends up
/// putting on the wire.
inline constexpr auto kRequestTokenAttribute = "sw6.token";

/// Correlation key of a realtime frame. Non-empty on purpose: an empty key
/// would complete whichever ASCII request happens to be outstanding.
inline constexpr auto kStreamCorrelationKey = "sw6.stream";

class Sw6Codec final : public IFrameCodec {
public:
    [[nodiscard]] QString name() const override { return QStringLiteral("sw6"); }

    [[nodiscard]] FrameScanResult scan(std::span<const std::byte> buffer,
                                       transport::Direction direction) const override;
    [[nodiscard]] Result<QByteArray> wrap(OpCode opcode, const QByteArray& body,
                                          const EncodeContext& context) const override;

    /// Replies echo the command name (section 4.6), which is a stronger match
    /// than "oldest outstanding request": a duplicated or late reply cannot
    /// complete an unrelated command.
    [[nodiscard]] QString correlationKey(const Frame& frame) const override;
    [[nodiscard]] QString prepareRequest(EncodeContext& context) const override;

    [[nodiscard]] core::ConfigSchema configSchema() const override;
    [[nodiscard]] Result<void> configure(const QVariantMap& config) override;

private:
    [[nodiscard]] FrameScanResult scanAsciiCommand(std::span<const std::byte> buffer) const;
    [[nodiscard]] FrameScanResult scanRealtimeStream(std::span<const std::byte> buffer) const;

    /// 255 / 5, the most a single length byte can describe.
    quint32 maxStreamRecords_{51};

    mutable quint32 nextToken_{1};
    mutable QHash<QString, QString> tokenByCommand_;
};

} // namespace hwsim::plugins::sw6
