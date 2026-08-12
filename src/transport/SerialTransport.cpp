#include "SerialTransport.h"

#include "StreamLink.h"

#include <core/Logger.h>

#include <QHash>
#include <QSerialPort>
#include <QSerialPortInfo>

namespace {
constexpr auto kLogCategory = "transport.serial";
}

namespace hwsim::transport {
namespace {

/// Lookup tables rather than switches: the configuration arrives as strings
/// from JSON or the UI, and a missing entry is a configuration error rather
/// than an unhandled case.
const QHash<QString, QSerialPort::Parity>& parityTable()
{
    static const QHash<QString, QSerialPort::Parity> table{
        {QStringLiteral("none"), QSerialPort::NoParity},
        {QStringLiteral("even"), QSerialPort::EvenParity},
        {QStringLiteral("odd"), QSerialPort::OddParity},
        {QStringLiteral("mark"), QSerialPort::MarkParity},
        {QStringLiteral("space"), QSerialPort::SpaceParity},
    };
    return table;
}

const QHash<QString, QSerialPort::StopBits>& stopBitsTable()
{
    static const QHash<QString, QSerialPort::StopBits> table{
        {QStringLiteral("1"), QSerialPort::OneStop},
        {QStringLiteral("1.5"), QSerialPort::OneAndHalfStop},
        {QStringLiteral("2"), QSerialPort::TwoStop},
    };
    return table;
}

const QHash<QString, QSerialPort::FlowControl>& flowControlTable()
{
    static const QHash<QString, QSerialPort::FlowControl> table{
        {QStringLiteral("none"), QSerialPort::NoFlowControl},
        {QStringLiteral("hardware"), QSerialPort::HardwareControl},
        {QStringLiteral("software"), QSerialPort::SoftwareControl},
    };
    return table;
}

const QHash<QString, QSerialPort::DataBits>& dataBitsTable()
{
    static const QHash<QString, QSerialPort::DataBits> table{
        {QStringLiteral("5"), QSerialPort::Data5},
        {QStringLiteral("6"), QSerialPort::Data6},
        {QStringLiteral("7"), QSerialPort::Data7},
        {QStringLiteral("8"), QSerialPort::Data8},
    };
    return table;
}

} // namespace

SerialTransport::SerialTransport(QObject* parent) : ITransport(parent) {}

SerialTransport::~SerialTransport()
{
    closeImpl();
}

QStringList SerialTransport::availablePorts()
{
    QStringList names;
    const auto ports = QSerialPortInfo::availablePorts();
    names.reserve(ports.size());
    for (const QSerialPortInfo& info : ports) {
        names.append(info.portName());
    }
    return names;
}

core::Result<void> SerialTransport::openImpl(const TransportConfig& config)
{
    const QString portName = config.portName();
    if (portName.isEmpty()) {
        return core::makeError(core::ErrorCode::ConfigInvalid,
                               QStringLiteral("no serial port selected"));
    }

    auto port = std::make_unique<QSerialPort>();
    port->setPortName(portName);

    if (const auto settings = applySettings(*port, config); settings.hasError()) {
        return settings.error();
    }

    if (!port->open(QIODevice::ReadWrite)) {
        return core::makeError(core::ErrorCode::IoError, port->errorString(), portName);
    }

    port->clear();

    // Build the description first and release only after StreamLink has taken
    // ownership: releasing inside the argument list would leave the port owned
    // by nobody if any other argument threw, leaking an open serial handle.
    const QString description =
        QStringLiteral("%1 @ %2").arg(portName).arg(config.baudRate());

    QSerialPort* raw = port.get();
    auto link = std::make_unique<StreamLink>(ILink::allocateId(), raw, description);
    port.release();
    port_ = raw;

    StreamLink* rawLink = link.get();
    currentLinkId_ = rawLink->id();

    connect(raw, &QSerialPort::errorOccurred, this, [this, raw](QSerialPort::SerialPortError error) {
        if (error == QSerialPort::NoError) {
            return;
        }
        const QString reason = raw->errorString();

        // this->config(), not config(): the enclosing function has a parameter
        // of the same name, which name lookup finds first and which the lambda
        // does not capture.
        reportError(core::makeError(core::ErrorCode::IoError, reason,
                                    this->config().portName()));

        // Resource errors mean the adapter was unplugged; the link is dead.
        if (error == QSerialPort::ResourceError || error == QSerialPort::PermissionError) {
            const LinkId id = currentLinkId_;
            currentLinkId_ = kInvalidLinkId;
            port_ = nullptr;
            if (id != kInvalidLinkId) {
                removeLink(id, reason);
            }
        }
    });

    addLink(std::move(link));
    rawLink->markConnected();

    HWSIM_LOG_INFO(kLogCategory) << "opened " << portName << " at " << config.baudRate() << " baud";
    return core::success();
}

void SerialTransport::closeImpl()
{
    // Only drops observer state. The port itself belongs to its StreamLink and
    // is destroyed with it, either by ITransport::close() before this runs or
    // by ~ITransport afterwards when the destructor calls straight in here.
    port_ = nullptr;
    currentLinkId_ = kInvalidLinkId;
}

core::Result<void> SerialTransport::applySettings(QSerialPort& port, const TransportConfig& config)
{
    port.setBaudRate(config.baudRate());

    const QString dataBits = config.value(QStringLiteral("dataBits"), QStringLiteral("8")).toString();
    const QString parity = config.value(QStringLiteral("parity"), QStringLiteral("none")).toString();
    const QString stopBits = config.value(QStringLiteral("stopBits"), QStringLiteral("1")).toString();
    const QString flowControl =
        config.value(QStringLiteral("flowControl"), QStringLiteral("none")).toString();

    if (!dataBitsTable().contains(dataBits)) {
        return core::makeError(core::ErrorCode::ConfigInvalid,
                               QStringLiteral("unsupported data bits '%1'").arg(dataBits));
    }
    if (!parityTable().contains(parity)) {
        return core::makeError(core::ErrorCode::ConfigInvalid,
                               QStringLiteral("unsupported parity '%1'").arg(parity));
    }
    if (!stopBitsTable().contains(stopBits)) {
        return core::makeError(core::ErrorCode::ConfigInvalid,
                               QStringLiteral("unsupported stop bits '%1'").arg(stopBits));
    }
    if (!flowControlTable().contains(flowControl)) {
        return core::makeError(core::ErrorCode::ConfigInvalid,
                               QStringLiteral("unsupported flow control '%1'").arg(flowControl));
    }

    port.setDataBits(dataBitsTable().value(dataBits));
    port.setParity(parityTable().value(parity));
    port.setStopBits(stopBitsTable().value(stopBits));
    port.setFlowControl(flowControlTable().value(flowControl));
    return core::success();
}

} // namespace hwsim::transport
