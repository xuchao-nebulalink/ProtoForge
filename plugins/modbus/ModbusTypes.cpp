#include "ModbusTypes.h"

#include <QHash>

using hwsim::core::ConfigField;
using hwsim::core::ConfigSchema;
using hwsim::core::ErrorCode;
using hwsim::core::makeError;
using hwsim::core::Result;

namespace hwsim::plugins::modbus {
namespace {

QString key(const char* name)
{
    return QString::fromLatin1(name);
}

} // namespace

QString exceptionCodeName(ExceptionCode code)
{
    static const QHash<ExceptionCode, QString> names{
        {ExceptionCode::IllegalFunction, QStringLiteral("非法功能码")},
        {ExceptionCode::IllegalDataAddress, QStringLiteral("非法数据地址")},
        {ExceptionCode::IllegalDataValue, QStringLiteral("非法数据值")},
        {ExceptionCode::ServerDeviceFailure, QStringLiteral("从站设备故障")},
        {ExceptionCode::Acknowledge, QStringLiteral("确认")},
        {ExceptionCode::ServerDeviceBusy, QStringLiteral("从站设备忙")},
        {ExceptionCode::MemoryParityError, QStringLiteral("存储奇偶校验错")},
        {ExceptionCode::GatewayPathUnavailable, QStringLiteral("网关路径不可用")},
        {ExceptionCode::GatewayTargetNoResponse, QStringLiteral("网关目标无响应")},
    };
    return names.value(code, QStringLiteral("未知异常"));
}

QString functionCodeName(quint8 functionCode)
{
    static const QHash<quint8, QString> names{
        {fc::kReadCoils, QStringLiteral("ReadCoils")},
        {fc::kReadDiscreteInputs, QStringLiteral("ReadDiscreteInputs")},
        {fc::kReadHoldingRegisters, QStringLiteral("ReadHoldingRegisters")},
        {fc::kReadInputRegisters, QStringLiteral("ReadInputRegisters")},
        {fc::kWriteSingleCoil, QStringLiteral("WriteSingleCoil")},
        {fc::kWriteSingleRegister, QStringLiteral("WriteSingleRegister")},
        {fc::kWriteMultipleCoils, QStringLiteral("WriteMultipleCoils")},
        {fc::kWriteMultipleRegisters, QStringLiteral("WriteMultipleRegisters")},
    };

    if (fc::isException(functionCode)) {
        return QStringLiteral("Exception(%1)")
            .arg(names.value(static_cast<quint8>(functionCode & ~fc::kExceptionFlag),
                             QStringLiteral("0x%1").arg(functionCode, 2, 16, QLatin1Char('0'))));
    }
    return names.value(functionCode,
                       QStringLiteral("0x%1").arg(functionCode, 2, 16, QLatin1Char('0')));
}

Result<quint32> AddressMap::resolve(quint8 functionCode, quint16 offset) const
{
    // Table selection is data, not control flow: adding a vendor-specific
    // function code that reads holding registers is one more entry here.
    const QHash<quint8, quint32> bases{
        {fc::kReadCoils, coilBase},
        {fc::kWriteSingleCoil, coilBase},
        {fc::kWriteMultipleCoils, coilBase},
        {fc::kReadDiscreteInputs, discreteInputBase},
        {fc::kReadInputRegisters, inputRegisterBase},
        {fc::kReadHoldingRegisters, holdingRegisterBase},
        {fc::kWriteSingleRegister, holdingRegisterBase},
        {fc::kWriteMultipleRegisters, holdingRegisterBase},
    };

    const auto it = bases.constFind(functionCode);
    if (it == bases.constEnd()) {
        return makeError(ErrorCode::Unsupported,
                         QStringLiteral("function code 0x%1 has no address table")
                             .arg(functionCode, 2, 16, QLatin1Char('0')));
    }
    return it.value() + offset;
}

quint16 AddressMap::readLimit(quint8 functionCode) const
{
    const bool isBitTable = functionCode == fc::kReadCoils
                            || functionCode == fc::kReadDiscreteInputs;
    return isBitTable ? maxReadBits : maxReadRegisters;
}

AddressMap AddressMap::fromConfig(const QVariantMap& config)
{
    AddressMap map;
    map.coilBase = config.value(key("coilBase"), 0).toUInt();
    map.discreteInputBase = config.value(key("discreteInputBase"), 10000).toUInt();
    map.inputRegisterBase = config.value(key("inputRegisterBase"), 30000).toUInt();
    map.holdingRegisterBase = config.value(key("holdingRegisterBase"), 40000).toUInt();
    map.maxReadBits = static_cast<quint16>(config.value(key("maxReadBits"), 2000).toUInt());
    map.maxReadRegisters = static_cast<quint16>(config.value(key("maxReadRegisters"), 125).toUInt());
    map.maxWriteRegisters =
        static_cast<quint16>(config.value(key("maxWriteRegisters"), 123).toUInt());
    return map;
}

ConfigSchema AddressMap::schema()
{
    ConfigSchema schema(QStringLiteral("地址映射"));

    schema.add(ConfigField::integer(key("coilBase"), QStringLiteral("线圈基地址"), 0)
                   .range(0, 4000000000.0)
                   .inGroup(QStringLiteral("地址映射"))
                   .describedAs(QStringLiteral("Modbus 0x 区映射到设备参数表的起始地址")));
    schema.add(ConfigField::integer(key("discreteInputBase"), QStringLiteral("离散输入基地址"), 10000)
                   .range(0, 4000000000.0)
                   .inGroup(QStringLiteral("地址映射")));
    schema.add(ConfigField::integer(key("inputRegisterBase"), QStringLiteral("输入寄存器基地址"), 30000)
                   .range(0, 4000000000.0)
                   .inGroup(QStringLiteral("地址映射")));
    schema.add(ConfigField::integer(key("holdingRegisterBase"), QStringLiteral("保持寄存器基地址"), 40000)
                   .range(0, 4000000000.0)
                   .inGroup(QStringLiteral("地址映射")));

    schema.add(ConfigField::integer(key("maxReadBits"), QStringLiteral("单次最大读位数"), 2000)
                   .range(1, 2000)
                   .inGroup(QStringLiteral("限制"))
                   .asAdvanced());
    schema.add(ConfigField::integer(key("maxReadRegisters"), QStringLiteral("单次最大读寄存器数"), 125)
                   .range(1, 125)
                   .inGroup(QStringLiteral("限制"))
                   .asAdvanced());
    schema.add(ConfigField::integer(key("maxWriteRegisters"), QStringLiteral("单次最大写寄存器数"), 123)
                   .range(1, 123)
                   .inGroup(QStringLiteral("限制"))
                   .asAdvanced());

    return schema;
}

} // namespace hwsim::plugins::modbus
