#include "ModbusPlugin.h"

#include "ModbusCodecs.h"
#include "ModbusHandlers.h"
#include "ModbusMessages.h"

#include <core/Logger.h>

#include <QJsonArray>
#include <QJsonObject>

namespace {
constexpr auto kLogCategory = "plugin.modbus";
}

using hwsim::core::ConfigField;
using hwsim::core::ConfigSchema;
using hwsim::core::ErrorCode;
using hwsim::core::makeError;
using hwsim::core::Result;
using hwsim::protocol::CommandRegistry;
using hwsim::protocol::FrameCodecPtr;
using hwsim::protocol::PluginMetadata;

namespace hwsim::plugins::modbus {
namespace {

bool isInitiator(const QVariantMap& config)
{
    return config.value(QString::fromLatin1(hwsim::protocol::reserved::kRole),
                        QString::fromLatin1(hwsim::protocol::reserved::kRoleResponder))
               .toString()
           == QString::fromLatin1(hwsim::protocol::reserved::kRoleInitiator);
}

QJsonObject registerDefinition(const QString& key, quint32 address, const QString& label,
                               const QString& access, double defaultValue,
                               const QString& group, const QString& unit = {})
{
    QJsonObject json;
    json.insert(QStringLiteral("key"), key);
    json.insert(QStringLiteral("displayName"), label);
    json.insert(QStringLiteral("type"), QStringLiteral("uint"));
    json.insert(QStringLiteral("access"), access);
    json.insert(QStringLiteral("address"), static_cast<qint64>(address));
    json.insert(QStringLiteral("default"), defaultValue);
    json.insert(QStringLiteral("group"), group);
    json.insert(QStringLiteral("minimum"), 0);
    json.insert(QStringLiteral("maximum"), 65535);
    if (!unit.isEmpty()) {
        json.insert(QStringLiteral("unit"), unit);
    }
    return json;
}

} // namespace

PluginMetadata ModbusPlugin::metadata() const
{
    PluginMetadata metadata;
    metadata.id = QStringLiteral("modbus");
    metadata.displayName = QStringLiteral("Modbus");
    metadata.version = QStringLiteral("1.0.0");
    metadata.vendor = QStringLiteral("HwSimPlatform");
    metadata.description =
        QStringLiteral("Modbus RTU / TCP，支持线圈、离散输入、保持寄存器与输入寄存器的读写，"
                       "可作为从站模拟设备，也可作为主站测试真实设备。");
    metadata.variants = {QStringLiteral("rtu"), QStringLiteral("tcp")};
    return metadata;
}

ConfigSchema ModbusPlugin::configSchema() const
{
    ConfigSchema schema(QStringLiteral("Modbus"));

    schema.add(ConfigField::enumeration(QStringLiteral("variant"), QStringLiteral("组帧格式"),
                                        {QStringLiteral("rtu"), QStringLiteral("tcp")},
                                        QStringLiteral("rtu"))
                   .withLabels({QStringLiteral("RTU (串行 / 二进制)"),
                                QStringLiteral("TCP (MBAP 报文头)")})
                   .describedAs(QStringLiteral(
                       "RTU 依据功能码推断帧长并用 CRC16 校验；TCP 使用 MBAP 长度字段")));

    schema.merge(ModbusCodecOptions::schema(), QStringLiteral("站点"));
    schema.merge(AddressMap::schema());

    return schema;
}

Result<FrameCodecPtr> ModbusPlugin::createCodec(const QVariantMap& config) const
{
    const ModbusCodecOptions options = ModbusCodecOptions::fromConfig(config);
    const QString variant = config.value(QStringLiteral("variant"), QStringLiteral("rtu")).toString();

    if (variant == QStringLiteral("rtu")) {
        // RTU has no length field, so the expected frame size is derived from
        // the function code and differs between requests and responses. Getting
        // the role wrong mis-frames every reply rather than failing outright,
        // which is worth a warning. TCP is unaffected: MBAP carries a length.
        if (!ModbusCodecOptions::configHasRole(config)) {
            HWSIM_LOG_WARNING(kLogCategory)
                << "RTU codec created without the reserved '"
                << QString::fromLatin1(hwsim::protocol::reserved::kRole)
                << "' key; assuming responder. Responses will be mis-framed if this is a master.";
        }
        return FrameCodecPtr(std::make_unique<ModbusRtuCodec>(options));
    }
    if (variant == QStringLiteral("tcp")) {
        return FrameCodecPtr(std::make_unique<ModbusTcpCodec>(options));
    }

    return makeError(ErrorCode::ConfigInvalid,
                     QStringLiteral("unknown Modbus variant '%1', expected 'rtu' or 'tcp'")
                         .arg(variant));
}

Result<void> ModbusPlugin::registerCommands(CommandRegistry& registry, const QVariantMap& config) const
{
    const AddressMap map = AddressMap::fromConfig(config);

    if (isInitiator(config)) {
        // Master side: encode requests, decode the replies that come back.
        registry.bindEncoder<ReadBitsRequest>();
        registry.bindEncoder<ReadRegistersRequest>();
        registry.bindEncoder<WriteSingleCoilRequest>();
        registry.bindEncoder<WriteSingleRegisterRequest>();
        registry.bindEncoder<WriteMultipleCoilsRequest>();
        registry.bindEncoder<WriteMultipleRegistersRequest>();

        registry.bindDecoder<ReadBitsResponse>(fc::kReadCoils, QStringLiteral("ReadCoils 响应"));
        registry.bindDecoder<ReadBitsResponse>(fc::kReadDiscreteInputs,
                                               QStringLiteral("ReadDiscreteInputs 响应"));
        registry.bindDecoder<ReadRegistersResponse>(fc::kReadHoldingRegisters,
                                                    QStringLiteral("ReadHoldingRegisters 响应"));
        registry.bindDecoder<ReadRegistersResponse>(fc::kReadInputRegisters,
                                                    QStringLiteral("ReadInputRegisters 响应"));
        registry.bindDecoder<WriteSingleResponse>(fc::kWriteSingleCoil,
                                                  QStringLiteral("WriteSingleCoil 响应"));
        registry.bindDecoder<WriteSingleResponse>(fc::kWriteSingleRegister,
                                                  QStringLiteral("WriteSingleRegister 响应"));
        registry.bindDecoder<WriteMultipleResponse>(fc::kWriteMultipleCoils,
                                                    QStringLiteral("WriteMultipleCoils 响应"));
        registry.bindDecoder<WriteMultipleResponse>(fc::kWriteMultipleRegisters,
                                                    QStringLiteral("WriteMultipleRegisters 响应"));
    } else {
        // Slave side: decode requests, run handlers, encode replies.
        //
        // A read of coils and a read of discrete inputs differ only in which
        // table they hit, so one message type and one handler are registered
        // against both function codes; the handler reads request.functionCode
        // to pick the table.
        auto readBits = std::make_shared<ReadBitsHandler>(map);
        registry.bindAt<ReadBitsRequest>(fc::kReadCoils, readBits, QStringLiteral("ReadCoils"));
        registry.bindDecoder<ReadBitsRequest>(fc::kReadDiscreteInputs,
                                              QStringLiteral("ReadDiscreteInputs"));

        auto readRegisters = std::make_shared<ReadRegistersHandler>(map);
        registry.bindAt<ReadRegistersRequest>(fc::kReadHoldingRegisters, readRegisters,
                                              QStringLiteral("ReadHoldingRegisters"));
        registry.bindDecoder<ReadRegistersRequest>(fc::kReadInputRegisters,
                                                   QStringLiteral("ReadInputRegisters"));

        registry.bindAt<WriteSingleCoilRequest>(fc::kWriteSingleCoil,
                                                std::make_shared<WriteSingleCoilHandler>(map),
                                                QStringLiteral("WriteSingleCoil"));
        registry.bindAt<WriteSingleRegisterRequest>(fc::kWriteSingleRegister,
                                                    std::make_shared<WriteSingleRegisterHandler>(map),
                                                    QStringLiteral("WriteSingleRegister"));
        registry.bindAt<WriteMultipleCoilsRequest>(fc::kWriteMultipleCoils,
                                                   std::make_shared<WriteMultipleCoilsHandler>(map),
                                                   QStringLiteral("WriteMultipleCoils"));
        registry.bindAt<WriteMultipleRegistersRequest>(
            fc::kWriteMultipleRegisters, std::make_shared<WriteMultipleRegistersHandler>(map),
            QStringLiteral("WriteMultipleRegisters"));

        registry.bindEncoder<ReadBitsResponse>();
        registry.bindEncoder<ReadRegistersResponse>();
        registry.bindEncoder<WriteSingleResponse>();
        registry.bindEncoder<WriteMultipleResponse>();
    }

    // Exceptions travel in both directions: a slave sends them, a master has to
    // recognise them. The opcode is the original function code with bit 7 set,
    // so one decoder is registered per supported function.
    registry.bindEncoder<ExceptionResponse>();
    for (const quint8 functionCode :
         {fc::kReadCoils, fc::kReadDiscreteInputs, fc::kReadHoldingRegisters,
          fc::kReadInputRegisters, fc::kWriteSingleCoil, fc::kWriteSingleRegister,
          fc::kWriteMultipleCoils, fc::kWriteMultipleRegisters}) {
        registry.bindDecoder<ExceptionResponse>(
            static_cast<protocol::OpCode>(functionCode | fc::kExceptionFlag),
            QStringLiteral("%1 异常").arg(functionCodeName(functionCode)));
    }

    HWSIM_LOG_INFO(kLogCategory) << "registered " << registry.size() << " Modbus opcodes as "
                                 << (isInitiator(config) ? "initiator" : "responder");
    return core::success();
}

QJsonArray ModbusPlugin::defaultParameterTemplate() const
{
    // A small but realistic starter map: two coils, two discrete inputs, four
    // holding registers and two input registers, using the default table bases.
    QJsonArray parameters;

    parameters.append(registerDefinition(QStringLiteral("coil.run"), 0, QStringLiteral("运行使能"),
                                         QStringLiteral("rw"), 0, QStringLiteral("线圈")));
    parameters.append(registerDefinition(QStringLiteral("coil.reset"), 1, QStringLiteral("复位"),
                                         QStringLiteral("rw"), 0, QStringLiteral("线圈")));

    parameters.append(registerDefinition(QStringLiteral("di.ready"), 10000, QStringLiteral("就绪"),
                                         QStringLiteral("r"), 1, QStringLiteral("离散输入")));
    parameters.append(registerDefinition(QStringLiteral("di.alarm"), 10001, QStringLiteral("报警"),
                                         QStringLiteral("r"), 0, QStringLiteral("离散输入")));

    parameters.append(registerDefinition(QStringLiteral("ir.temperature"), 30000,
                                         QStringLiteral("温度"), QStringLiteral("r"), 250,
                                         QStringLiteral("输入寄存器"), QStringLiteral("0.1℃")));
    parameters.append(registerDefinition(QStringLiteral("ir.pressure"), 30001,
                                         QStringLiteral("压力"), QStringLiteral("r"), 1013,
                                         QStringLiteral("输入寄存器"), QStringLiteral("0.1kPa")));

    parameters.append(registerDefinition(QStringLiteral("hr.setpoint"), 40000,
                                         QStringLiteral("温度设定"), QStringLiteral("rw"), 600,
                                         QStringLiteral("保持寄存器"), QStringLiteral("0.1℃")));
    parameters.append(registerDefinition(QStringLiteral("hr.speed"), 40001, QStringLiteral("转速设定"),
                                         QStringLiteral("rw"), 1500, QStringLiteral("保持寄存器"),
                                         QStringLiteral("rpm")));
    parameters.append(registerDefinition(QStringLiteral("hr.mode"), 40002, QStringLiteral("工作模式"),
                                         QStringLiteral("rw"), 0, QStringLiteral("保持寄存器")));
    parameters.append(registerDefinition(QStringLiteral("hr.reserved"), 40003, QStringLiteral("保留"),
                                         QStringLiteral("rw"), 0, QStringLiteral("保持寄存器")));

    return parameters;
}

} // namespace hwsim::plugins::modbus

HWSIM_EXPORT_STATIC_PLUGIN(modbus, hwsim::plugins::modbus::ModbusPlugin)
