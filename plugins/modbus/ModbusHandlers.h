#pragma once

#include "ModbusMessages.h"

#include <protocol/ExecutionContext.h>

namespace hwsim::plugins::modbus {

using hwsim::protocol::ExecutionContext;
using hwsim::protocol::MessagePtr;

/// Handlers are plain classes with one handle() method. They inherit nothing,
/// hold only what they need, and are bound to their message type in one line
/// by CommandRegistry::bind.
///
/// Each one shares the same shape: validate against the specification, map the
/// Modbus offset into the device address space, touch the device, and return
/// either a response message or a Modbus exception. Device-level failures
/// become exception responses rather than framework errors, because that is
/// what a real slave puts on the wire.

class ReadBitsHandler {
public:
    explicit ReadBitsHandler(AddressMap map) : map_(map) {}
    [[nodiscard]] core::Result<MessagePtr> handle(const ReadBitsRequest& request,
                                                  ExecutionContext& context);

private:
    AddressMap map_;
};

class ReadRegistersHandler {
public:
    explicit ReadRegistersHandler(AddressMap map) : map_(map) {}
    [[nodiscard]] core::Result<MessagePtr> handle(const ReadRegistersRequest& request,
                                                  ExecutionContext& context);

private:
    AddressMap map_;
};

class WriteSingleCoilHandler {
public:
    explicit WriteSingleCoilHandler(AddressMap map) : map_(map) {}
    [[nodiscard]] core::Result<MessagePtr> handle(const WriteSingleCoilRequest& request,
                                                  ExecutionContext& context);

private:
    AddressMap map_;
};

class WriteSingleRegisterHandler {
public:
    explicit WriteSingleRegisterHandler(AddressMap map) : map_(map) {}
    [[nodiscard]] core::Result<MessagePtr> handle(const WriteSingleRegisterRequest& request,
                                                  ExecutionContext& context);

private:
    AddressMap map_;
};

class WriteMultipleCoilsHandler {
public:
    explicit WriteMultipleCoilsHandler(AddressMap map) : map_(map) {}
    [[nodiscard]] core::Result<MessagePtr> handle(const WriteMultipleCoilsRequest& request,
                                                  ExecutionContext& context);

private:
    AddressMap map_;
};

class WriteMultipleRegistersHandler {
public:
    explicit WriteMultipleRegistersHandler(AddressMap map) : map_(map) {}
    [[nodiscard]] core::Result<MessagePtr> handle(const WriteMultipleRegistersRequest& request,
                                                  ExecutionContext& context);

private:
    AddressMap map_;
};

} // namespace hwsim::plugins::modbus
