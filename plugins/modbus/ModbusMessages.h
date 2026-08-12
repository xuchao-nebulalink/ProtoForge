#pragma once

#include "ModbusTypes.h"

#include <protocol/IMessage.h>

#include <QVector>

namespace hwsim::plugins::modbus {

using hwsim::core::Result;
using hwsim::protocol::EncodeContext;
using hwsim::protocol::Frame;
using hwsim::protocol::MessageBase;
using hwsim::protocol::OpCode;

/// Every Modbus message carries the function code it was decoded from and
/// reports it through dynamicOpcode(). That single mechanism covers both places
/// where the wire opcode is not fixed by the message type: one type serving two
/// function codes (coils and discrete inputs read identically), and exception
/// responses echoing the original code with bit 7 set.

// --- Requests --------------------------------------------------------------

/// Read Coils (0x01) and Read Discrete Inputs (0x02).
struct ReadBitsRequest : MessageBase<ReadBitsRequest> {
    quint8 functionCode{fc::kReadCoils};
    quint16 startAddress{0};
    quint16 quantity{1};

    [[nodiscard]] static constexpr OpCode opcode() { return fc::kReadCoils; }
    [[nodiscard]] std::optional<OpCode> dynamicOpcode() const override { return functionCode; }

    [[nodiscard]] static Result<ReadBitsRequest> decode(const Frame& frame);
    [[nodiscard]] Result<QByteArray> encodeBody(const EncodeContext& context) const;
    [[nodiscard]] QString describe() const override;
};

/// Read Holding Registers (0x03) and Read Input Registers (0x04).
struct ReadRegistersRequest : MessageBase<ReadRegistersRequest> {
    quint8 functionCode{fc::kReadHoldingRegisters};
    quint16 startAddress{0};
    quint16 quantity{1};

    [[nodiscard]] static constexpr OpCode opcode() { return fc::kReadHoldingRegisters; }
    [[nodiscard]] std::optional<OpCode> dynamicOpcode() const override { return functionCode; }

    [[nodiscard]] static Result<ReadRegistersRequest> decode(const Frame& frame);
    [[nodiscard]] Result<QByteArray> encodeBody(const EncodeContext& context) const;
    [[nodiscard]] QString describe() const override;
};

/// Write Single Coil (0x05).
struct WriteSingleCoilRequest : MessageBase<WriteSingleCoilRequest> {
    quint16 address{0};
    bool value{false};

    [[nodiscard]] static constexpr OpCode opcode() { return fc::kWriteSingleCoil; }

    [[nodiscard]] static Result<WriteSingleCoilRequest> decode(const Frame& frame);
    [[nodiscard]] Result<QByteArray> encodeBody(const EncodeContext& context) const;
    [[nodiscard]] QString describe() const override;
};

/// Write Single Register (0x06).
struct WriteSingleRegisterRequest : MessageBase<WriteSingleRegisterRequest> {
    quint16 address{0};
    quint16 value{0};

    [[nodiscard]] static constexpr OpCode opcode() { return fc::kWriteSingleRegister; }

    [[nodiscard]] static Result<WriteSingleRegisterRequest> decode(const Frame& frame);
    [[nodiscard]] Result<QByteArray> encodeBody(const EncodeContext& context) const;
    [[nodiscard]] QString describe() const override;
};

/// Write Multiple Coils (0x0F).
struct WriteMultipleCoilsRequest : MessageBase<WriteMultipleCoilsRequest> {
    quint16 startAddress{0};
    QVector<bool> values;

    [[nodiscard]] static constexpr OpCode opcode() { return fc::kWriteMultipleCoils; }

    [[nodiscard]] static Result<WriteMultipleCoilsRequest> decode(const Frame& frame);
    [[nodiscard]] Result<QByteArray> encodeBody(const EncodeContext& context) const;
    [[nodiscard]] QString describe() const override;
};

/// Write Multiple Registers (0x10).
struct WriteMultipleRegistersRequest : MessageBase<WriteMultipleRegistersRequest> {
    quint16 startAddress{0};
    QVector<quint16> values;

    [[nodiscard]] static constexpr OpCode opcode() { return fc::kWriteMultipleRegisters; }

    [[nodiscard]] static Result<WriteMultipleRegistersRequest> decode(const Frame& frame);
    [[nodiscard]] Result<QByteArray> encodeBody(const EncodeContext& context) const;
    [[nodiscard]] QString describe() const override;
};

// --- Responses -------------------------------------------------------------

struct ReadBitsResponse : MessageBase<ReadBitsResponse> {
    quint8 functionCode{fc::kReadCoils};
    QVector<bool> values;

    [[nodiscard]] static constexpr OpCode opcode() { return fc::kReadCoils; }
    [[nodiscard]] std::optional<OpCode> dynamicOpcode() const override { return functionCode; }

    [[nodiscard]] static Result<ReadBitsResponse> decode(const Frame& frame);
    [[nodiscard]] Result<QByteArray> encodeBody(const EncodeContext& context) const;
    [[nodiscard]] QString describe() const override;
};

struct ReadRegistersResponse : MessageBase<ReadRegistersResponse> {
    quint8 functionCode{fc::kReadHoldingRegisters};
    QVector<quint16> values;

    [[nodiscard]] static constexpr OpCode opcode() { return fc::kReadHoldingRegisters; }
    [[nodiscard]] std::optional<OpCode> dynamicOpcode() const override { return functionCode; }

    [[nodiscard]] static Result<ReadRegistersResponse> decode(const Frame& frame);
    [[nodiscard]] Result<QByteArray> encodeBody(const EncodeContext& context) const;
    [[nodiscard]] QString describe() const override;
};

/// Echo response for 0x05 and 0x06. Structurally identical to the request, but
/// a distinct type so the registry can tell a received request from a received
/// reply when a session plays both roles.
struct WriteSingleResponse : MessageBase<WriteSingleResponse> {
    quint8 functionCode{fc::kWriteSingleRegister};
    quint16 address{0};
    quint16 rawValue{0};

    [[nodiscard]] static constexpr OpCode opcode() { return fc::kWriteSingleRegister; }
    [[nodiscard]] std::optional<OpCode> dynamicOpcode() const override { return functionCode; }

    [[nodiscard]] static Result<WriteSingleResponse> decode(const Frame& frame);
    [[nodiscard]] Result<QByteArray> encodeBody(const EncodeContext& context) const;
    [[nodiscard]] QString describe() const override;
};

/// Echo response for 0x0F and 0x10: start address and quantity written.
struct WriteMultipleResponse : MessageBase<WriteMultipleResponse> {
    quint8 functionCode{fc::kWriteMultipleRegisters};
    quint16 startAddress{0};
    quint16 quantity{0};

    [[nodiscard]] static constexpr OpCode opcode() { return fc::kWriteMultipleRegisters; }
    [[nodiscard]] std::optional<OpCode> dynamicOpcode() const override { return functionCode; }

    [[nodiscard]] static Result<WriteMultipleResponse> decode(const Frame& frame);
    [[nodiscard]] Result<QByteArray> encodeBody(const EncodeContext& context) const;
    [[nodiscard]] QString describe() const override;
};

struct ExceptionResponse : MessageBase<ExceptionResponse> {
    /// Original function code, without the exception flag.
    quint8 functionCode{0};
    ExceptionCode exceptionCode{ExceptionCode::ServerDeviceFailure};

    [[nodiscard]] static constexpr OpCode opcode() { return fc::kExceptionFlag; }
    [[nodiscard]] std::optional<OpCode> dynamicOpcode() const override
    {
        return static_cast<OpCode>(functionCode | fc::kExceptionFlag);
    }

    [[nodiscard]] static Result<ExceptionResponse> decode(const Frame& frame);
    [[nodiscard]] Result<QByteArray> encodeBody(const EncodeContext& context) const;
    [[nodiscard]] QString describe() const override;

    [[nodiscard]] static std::shared_ptr<ExceptionResponse> make(quint8 originalFunctionCode,
                                                                 ExceptionCode code);
};

} // namespace hwsim::plugins::modbus
