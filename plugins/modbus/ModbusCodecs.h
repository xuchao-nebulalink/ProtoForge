#pragma once

#include "ModbusTypes.h"

#include <protocol/IFrameCodec.h>

#include <atomic>

namespace hwsim::plugins::modbus {

using hwsim::protocol::EncodeContext;
using hwsim::protocol::FrameScanResult;
using hwsim::protocol::IFrameCodec;
using hwsim::protocol::OpCode;

/// Shared configuration between the two framings.
struct ModbusCodecOptions {
    quint8 unitId{1};

    /// A slave normally ignores frames addressed to someone else. Turning this
    /// on makes the simulator answer everything, which is handy when sniffing
    /// an unfamiliar bus.
    bool acceptAnyUnitId{false};

    /// True when this side decodes requests (a simulated device), false when it
    /// decodes responses (a test master). RTU has no length field, so the
    /// expected frame size depends on which direction is being parsed.
    bool decodesRequests{true};

    [[nodiscard]] static core::ConfigSchema schema();
    [[nodiscard]] static ModbusCodecOptions fromConfig(const QVariantMap& config);

    /// Whether the map carries the framework's reserved role key. A settings
    /// map produced from this plugin's own schema will not, because role is
    /// owned by the endpoint rather than by the protocol panel.
    [[nodiscard]] static bool configHasRole(const QVariantMap& config);
};

/// Modbus RTU: [unit][function][data][crc16-le]
///
/// RTU carries no length field, so framing has to be derived from the function
/// code and, for the variable-length messages, from an embedded byte count.
/// That is why the codec needs to know whether it is parsing requests or
/// responses.
class ModbusRtuCodec final : public IFrameCodec {
public:
    explicit ModbusRtuCodec(ModbusCodecOptions options = {});

    [[nodiscard]] QString name() const override { return QStringLiteral("modbus-rtu"); }

    [[nodiscard]] FrameScanResult scan(std::span<const std::byte> buffer,
                                       transport::Direction direction) const override;
    [[nodiscard]] core::Result<QByteArray> wrap(OpCode opcode, const QByteArray& body,
                                                const EncodeContext& context) const override;

    /// RTU is strictly one transaction at a time, so responses match the oldest
    /// outstanding request and need no correlation token.
    [[nodiscard]] bool isHalfDuplex() const override { return true; }

    [[nodiscard]] core::ConfigSchema configSchema() const override;
    [[nodiscard]] core::Result<void> configure(const QVariantMap& config) override;

private:
    ModbusCodecOptions options_;
};

/// Modbus TCP: [transaction:2][protocol:2][length:2][unit][function][data]
///
/// The MBAP header carries an explicit length, so framing is trivial, and a
/// transaction id, so several requests can be outstanding at once.
class ModbusTcpCodec final : public IFrameCodec {
public:
    explicit ModbusTcpCodec(ModbusCodecOptions options = {});

    [[nodiscard]] QString name() const override { return QStringLiteral("modbus-tcp"); }

    [[nodiscard]] FrameScanResult scan(std::span<const std::byte> buffer,
                                       transport::Direction direction) const override;
    [[nodiscard]] core::Result<QByteArray> wrap(OpCode opcode, const QByteArray& body,
                                                const EncodeContext& context) const override;

    [[nodiscard]] QString correlationKey(const protocol::Frame& frame) const override;
    [[nodiscard]] QString prepareRequest(EncodeContext& context) const override;

    [[nodiscard]] core::ConfigSchema configSchema() const override;
    [[nodiscard]] core::Result<void> configure(const QVariantMap& config) override;

private:
    ModbusCodecOptions options_;
    mutable std::atomic<quint16> nextTransactionId_{1};
};

} // namespace hwsim::plugins::modbus
