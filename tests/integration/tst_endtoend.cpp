// Full-stack tests: two endpoints wired in memory, a real protocol session on
// each side, a real device model behind one of them, and the fault injector in
// the path.
//
// The protocol here is defined inline rather than pulled from a plugin, for two
// reasons: the test then works regardless of whether plugins were built static
// or dynamic, and it doubles as a worked example of the smallest protocol the
// framework accepts.

#include <core/Crc.h>
#include <core/Endian.h>
#include <core/HexUtils.h>
#include <core/Logger.h>
#include <protocol/CommandRegistry.h>
#include <protocol/IFrameCodec.h>
#include <protocol/ProtocolSession.h>
#include <simulator/DeviceModel.h>
#include <transport/LoopbackTransport.h>

#include <QTest>

#include <array>

using namespace hwsim;
using namespace hwsim::core;
using namespace hwsim::protocol;
using hwsim::transport::Direction;
using hwsim::transport::LoopbackTransport;
using hwsim::transport::TransportConfig;
using hwsim::transport::TransportKind;
using hwsim::transport::TransportRole;

namespace {

constexpr quint8 kUnitId = 0x11;
constexpr OpCode kReadRegisters = 0x03;
constexpr OpCode kWriteRegister = 0x06;

void appendWord(QByteArray& out, quint16 value)
{
    out.append(static_cast<char>((value >> 8) & 0xFF));
    out.append(static_cast<char>(value & 0xFF));
}

quint16 readWord(const QByteArray& data, qsizetype offset)
{
    return endian::readBig<quint16>(
        hex::asBytes(data).subspan(static_cast<std::size_t>(offset), 2));
}

// --- Messages --------------------------------------------------------------

struct ReadRegistersRequest : MessageBase<ReadRegistersRequest> {
    quint16 startAddress{0};
    quint16 quantity{1};

    static constexpr OpCode opcode() { return kReadRegisters; }

    static Result<ReadRegistersRequest> decode(const Frame& frame)
    {
        if (frame.payload.size() < 4) {
            return makeError(ErrorCode::FrameMalformed, QStringLiteral("read request too short"));
        }
        ReadRegistersRequest request;
        request.startAddress = readWord(frame.payload, 0);
        request.quantity = readWord(frame.payload, 2);
        return request;
    }

    Result<QByteArray> encodeBody(const EncodeContext&) const
    {
        QByteArray body;
        appendWord(body, startAddress);
        appendWord(body, quantity);
        return body;
    }

    QString describe() const override
    {
        return QStringLiteral("Read addr=%1 count=%2").arg(startAddress).arg(quantity);
    }
};

struct ReadRegistersResponse : MessageBase<ReadRegistersResponse> {
    QVector<quint16> values;

    static constexpr OpCode opcode() { return kReadRegisters; }

    static Result<ReadRegistersResponse> decode(const Frame& frame)
    {
        if (frame.payload.isEmpty()) {
            return makeError(ErrorCode::FrameMalformed, QStringLiteral("read response too short"));
        }
        const auto byteCount = static_cast<quint8>(frame.payload.at(0));
        if (frame.payload.size() < 1 + byteCount) {
            return makeError(ErrorCode::FrameMalformed, QStringLiteral("truncated read response"));
        }

        ReadRegistersResponse response;
        for (int index = 0; index < byteCount / 2; ++index) {
            response.values.append(readWord(frame.payload, 1 + index * 2));
        }
        return response;
    }

    Result<QByteArray> encodeBody(const EncodeContext&) const
    {
        QByteArray body;
        body.append(static_cast<char>(values.size() * 2));
        for (const quint16 value : values) {
            appendWord(body, value);
        }
        return body;
    }

    QString describe() const override
    {
        return QStringLiteral("Read response, %1 reg(s)").arg(values.size());
    }
};

struct WriteRegisterRequest : MessageBase<WriteRegisterRequest> {
    quint16 address{0};
    quint16 value{0};

    static constexpr OpCode opcode() { return kWriteRegister; }

    static Result<WriteRegisterRequest> decode(const Frame& frame)
    {
        if (frame.payload.size() < 4) {
            return makeError(ErrorCode::FrameMalformed, QStringLiteral("write request too short"));
        }
        WriteRegisterRequest request;
        request.address = readWord(frame.payload, 0);
        request.value = readWord(frame.payload, 2);
        return request;
    }

    Result<QByteArray> encodeBody(const EncodeContext&) const
    {
        QByteArray body;
        appendWord(body, address);
        appendWord(body, value);
        return body;
    }

    QString describe() const override
    {
        return QStringLiteral("Write addr=%1 value=%2").arg(address).arg(value);
    }
};

struct WriteRegisterResponse : MessageBase<WriteRegisterResponse> {
    quint16 address{0};
    quint16 value{0};

    static constexpr OpCode opcode() { return kWriteRegister; }

    static Result<WriteRegisterResponse> decode(const Frame& frame)
    {
        if (frame.payload.size() < 4) {
            return makeError(ErrorCode::FrameMalformed, QStringLiteral("write response too short"));
        }
        WriteRegisterResponse response;
        response.address = readWord(frame.payload, 0);
        response.value = readWord(frame.payload, 2);
        return response;
    }

    Result<QByteArray> encodeBody(const EncodeContext&) const
    {
        QByteArray body;
        appendWord(body, address);
        appendWord(body, value);
        return body;
    }

    QString describe() const override
    {
        return QStringLiteral("Write response addr=%1 value=%2").arg(address).arg(value);
    }
};

// --- Handlers --------------------------------------------------------------

class ReadHandler {
public:
    Result<MessagePtr> handle(const ReadRegistersRequest& request, ExecutionContext& context)
    {
        const auto values = context.readRange(request.startAddress, request.quantity);
        if (values.hasError()) {
            return values.error();
        }

        auto response = std::make_shared<ReadRegistersResponse>();
        for (const QVariant& value : values.value()) {
            response->values.append(static_cast<quint16>(value.toUInt() & 0xFFFF));
        }
        return MessagePtr(response);
    }
};

class WriteHandler {
public:
    Result<MessagePtr> handle(const WriteRegisterRequest& request, ExecutionContext& context)
    {
        if (const auto written = context.write(request.address, request.value);
            written.hasError()) {
            return written.error();
        }

        auto response = std::make_shared<WriteRegisterResponse>();
        response->address = request.address;
        response->value = request.value;
        return MessagePtr(response);
    }
};

// --- Codec -----------------------------------------------------------------

/// [unit][function][length][payload][crc16-modbus little endian]
class MiniCodec final : public IFrameCodec {
public:
    QString name() const override { return QStringLiteral("mini"); }

    FrameScanResult scan(std::span<const std::byte> buffer, Direction) const override
    {
        if (buffer.size() < 5) {
            return FrameScanResult::needMoreData();
        }
        if (std::to_integer<quint8>(buffer[0]) != kUnitId) {
            return FrameScanResult::discard(1, QStringLiteral("unit mismatch"));
        }

        const auto length = std::to_integer<std::size_t>(buffer[2]);
        const std::size_t total = 3 + length + 2;
        if (buffer.size() < total) {
            return FrameScanResult::needMoreData();
        }

        const quint16 computed = crc::modbus(buffer.first(total - 2));
        const quint16 received = endian::readLittle<quint16>(buffer.subspan(total - 2, 2));
        if (computed != received) {
            return FrameScanResult::discard(total, QStringLiteral("CRC mismatch"));
        }

        Frame frame;
        frame.opcode = std::to_integer<OpCode>(buffer[1]);
        frame.payload = hex::toByteArray(buffer.subspan(3, length));
        frame.raw = hex::toByteArray(buffer.first(total));
        return FrameScanResult::ready(std::move(frame), total);
    }

    Result<QByteArray> wrap(OpCode opcode, const QByteArray& body, const EncodeContext&) const override
    {
        QByteArray frame;
        frame.append(static_cast<char>(kUnitId));
        frame.append(static_cast<char>(opcode & 0xFF));
        frame.append(static_cast<char>(body.size()));
        frame.append(body);

        const quint16 value = crc::modbus(hex::asBytes(frame));
        frame.append(static_cast<char>(value & 0xFF));
        frame.append(static_cast<char>((value >> 8) & 0xFF));
        return frame;
    }
};

std::shared_ptr<CommandRegistry> makeSlaveRegistry()
{
    auto registry = std::make_shared<CommandRegistry>();
    registry->bind<ReadRegistersRequest>(std::make_shared<ReadHandler>());
    registry->bind<WriteRegisterRequest>(std::make_shared<WriteHandler>());
    registry->bindEncoder<ReadRegistersResponse>();
    registry->bindEncoder<WriteRegisterResponse>();
    return registry;
}

std::shared_ptr<CommandRegistry> makeMasterRegistry()
{
    auto registry = std::make_shared<CommandRegistry>();
    registry->bindEncoder<ReadRegistersRequest>();
    registry->bindEncoder<WriteRegisterRequest>();
    registry->bindDecoder<ReadRegistersResponse>(kReadRegisters);
    registry->bindDecoder<WriteRegisterResponse>(kWriteRegister);
    return registry;
}

simulator::ParameterDefinition registerAt(quint32 address, double value)
{
    auto definition = simulator::ParameterDefinition::make(
        QStringLiteral("reg%1").arg(address), simulator::ParameterType::UInt, value);
    definition.address = address;
    definition.hasAddress = true;
    definition.minimum = 0;
    definition.maximum = 65535;
    return definition;
}

/// Holds one complete master/slave pair so each test starts from a clean rig.
struct Rig {
    LoopbackTransport slaveEndpoint;
    LoopbackTransport masterEndpoint;
    simulator::DeviceModel device{QStringLiteral("dut")};
    std::unique_ptr<ProtocolSession> slaveSession;
    std::unique_ptr<ProtocolSession> masterSession;

    void build(int registerCount = 10)
    {
        for (quint32 address = 0; address < static_cast<quint32>(registerCount); ++address) {
            QVERIFY(device.parameters().define(registerAt(address, address * 100)).hasValue());
        }

        QVERIFY(slaveEndpoint.open(TransportConfig(TransportKind::Loopback)).hasValue());
        QVERIFY(masterEndpoint.open(TransportConfig(TransportKind::Loopback)).hasValue());
        LoopbackTransport::connectPair(&slaveEndpoint, &masterEndpoint);

        ProtocolSession::Options slaveOptions;
        slaveOptions.name = QStringLiteral("slave");
        slaveOptions.deviceName = QStringLiteral("dut");
        slaveOptions.role = TransportRole::Responder;
        slaveOptions.publishEvents = false;

        slaveSession = std::make_unique<ProtocolSession>(
            slaveEndpoint.primaryLink(), std::make_unique<MiniCodec>(), makeSlaveRegistry(),
            slaveOptions);
        slaveSession->setDevice(&device);
        slaveSession->middleware().add(std::make_shared<DeviceStateGateMiddleware>());
        device.faults().attachTo(*slaveSession);

        ProtocolSession::Options masterOptions;
        masterOptions.name = QStringLiteral("master");
        masterOptions.role = TransportRole::Initiator;
        masterOptions.defaultTimeoutMs = 300;
        masterOptions.publishEvents = false;

        masterSession = std::make_unique<ProtocolSession>(
            masterEndpoint.primaryLink(), std::make_unique<MiniCodec>(), makeMasterRegistry(),
            masterOptions);

        device.start();
    }

    /// Issues a read and spins the loop until the reply or the timeout lands.
    Result<MessagePtr> read(quint16 address, quint16 count, int timeoutMs = 400)
    {
        auto request = std::make_shared<ReadRegistersRequest>();
        request->startAddress = address;
        request->quantity = count;

        std::optional<Result<MessagePtr>> outcome;
        const auto sent = masterSession->sendRequest(
            request, [&outcome](Result<MessagePtr> reply) { outcome = std::move(reply); },
            timeoutMs);
        if (sent.hasError()) {
            return sent.error();
        }

        return await(outcome, timeoutMs);
    }

    Result<MessagePtr> write(quint16 address, quint16 value, int timeoutMs = 400)
    {
        auto request = std::make_shared<WriteRegisterRequest>();
        request->address = address;
        request->value = value;

        std::optional<Result<MessagePtr>> outcome;
        const auto sent = masterSession->sendRequest(
            request, [&outcome](Result<MessagePtr> reply) { outcome = std::move(reply); },
            timeoutMs);
        if (sent.hasError()) {
            return sent.error();
        }

        return await(outcome, timeoutMs);
    }

private:
    /// Spins the event loop until the reply lands or the wait runs out.
    ///
    /// QTRY_VERIFY_WITH_TIMEOUT expands to a bare `return;` on failure, so it
    /// only compiles inside a void test slot. qWaitFor is the plain-function
    /// equivalent, which lets a timeout surface as a Result the caller can
    /// assert on rather than as an opaque test abort.
    static Result<MessagePtr> await(std::optional<Result<MessagePtr>>& slot, int timeoutMs)
    {
        const int budget = timeoutMs + 500;
        if (!QTest::qWaitFor([&slot] { return slot.has_value(); }, budget)) {
            return makeError(ErrorCode::Timeout,
                             QStringLiteral("no reply within %1 ms").arg(budget));
        }
        return *slot;
    }
};

} // namespace

class EndToEndTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        Logger::instance().setAsynchronous(false);
        Logger::instance().setLevel(LogLevel::Error);
    }

    // --- Happy path --------------------------------------------------------

    void masterReadsRegistersFromTheDeviceModel()
    {
        Rig rig;
        rig.build();

        const auto reply = rig.read(0, 4);
        QVERIFY(reply.hasValue());

        const auto* response = dynamic_cast<const ReadRegistersResponse*>(reply.value().get());
        QVERIFY(response != nullptr);
        QCOMPARE(response->values, QVector<quint16>({0, 100, 200, 300}));
    }

    void masterWriteReachesTheDeviceModel()
    {
        Rig rig;
        rig.build();

        const auto reply = rig.write(2, 4242);
        QVERIFY(reply.hasValue());

        const auto* response = dynamic_cast<const WriteRegisterResponse*>(reply.value().get());
        QVERIFY(response != nullptr);
        QCOMPARE(response->value, quint16{4242});

        QCOMPARE(rig.device.parameters().readAddress(2).value().toUInt(), 4242u);
    }

    void readOutsideTheRegisterMapFails()
    {
        Rig rig;
        rig.build(4);

        // Handler returns an error, so the slave sends nothing and the master
        // times out. That is the correct wire behaviour for a protocol with no
        // exception response defined.
        const auto reply = rig.read(0, 99, 200);
        QVERIFY(reply.hasError());
        QCOMPARE(reply.error().code, ErrorCode::Timeout);
    }

    void sequentialRequestsAllComplete()
    {
        Rig rig;
        rig.build();

        for (quint16 address = 0; address < 5; ++address) {
            const auto written = rig.write(address, static_cast<quint16>(address + 1));
            QVERIFY2(written.hasValue(), qPrintable(QString::number(address)));
        }

        const auto reply = rig.read(0, 5);
        QVERIFY(reply.hasValue());
        const auto* response = dynamic_cast<const ReadRegistersResponse*>(reply.value().get());
        QCOMPARE(response->values, QVector<quint16>({1, 2, 3, 4, 5}));
    }

    // --- Framing -----------------------------------------------------------

    void fragmentedDeliveryIsReassembled()
    {
        Rig rig;
        rig.build();

        // Hand the slave its request one byte at a time, which is what a slow
        // serial link produces.
        MiniCodec codec;
        ReadRegistersRequest request;
        request.startAddress = 1;
        request.quantity = 2;

        const auto body = request.encodeBody(EncodeContext{});
        const auto framed = codec.wrap(kReadRegisters, body.value(), EncodeContext{});
        QVERIFY(framed.hasValue());

        QVector<QByteArray> received;
        QObject::connect(rig.masterEndpoint.primaryLink(), &transport::ILink::bytesReceived,
                         [&received](transport::LinkId, const QByteArray& data) {
                             received.append(data);
                         });

        for (const char byte : framed.value()) {
            rig.masterEndpoint.primaryLink()->send(QByteArray(1, byte));
            QTest::qWait(1);
        }

        QTRY_VERIFY(!received.isEmpty());
        QCOMPARE(rig.slaveSession->counters().framesDecoded, quint64{1});
    }

    void backToBackFramesAreBothDecoded()
    {
        Rig rig;
        rig.build();

        MiniCodec codec;
        ReadRegistersRequest request;
        request.startAddress = 0;
        request.quantity = 1;

        const auto body = request.encodeBody(EncodeContext{});
        const auto framed = codec.wrap(kReadRegisters, body.value(), EncodeContext{});

        // Two complete frames arriving in one read.
        rig.masterEndpoint.primaryLink()->send(framed.value() + framed.value());

        QTRY_COMPARE(rig.slaveSession->counters().framesDecoded, quint64{2});
    }

    void leadingNoiseIsDiscardedAndTheFrameStillArrives()
    {
        Rig rig;
        rig.build();

        MiniCodec codec;
        ReadRegistersRequest request;
        request.startAddress = 0;
        request.quantity = 1;

        const auto body = request.encodeBody(EncodeContext{});
        const auto framed = codec.wrap(kReadRegisters, body.value(), EncodeContext{});

        rig.masterEndpoint.primaryLink()->send(QByteArray::fromHex("FFEEDD") + framed.value());

        QTRY_COMPARE(rig.slaveSession->counters().framesDecoded, quint64{1});
        QCOMPARE(rig.slaveSession->counters().resyncBytes, quint64{3});
    }

    // --- Fault injection ---------------------------------------------------

    void checksumFaultMakesTheMasterTimeOut()
    {
        Rig rig;
        rig.build();

        QVERIFY(rig.device.faults()
                    .addRule(QStringLiteral("checksum-error"),
                             {{QStringLiteral("direction"), QStringLiteral("outbound")},
                              {QStringLiteral("trigger"), QStringLiteral("always")},
                              {QStringLiteral("checksumBytes"), 2}})
                    .hasValue());

        // The reply is sent but its CRC is wrong, so the master's codec discards
        // it during resynchronisation and the request never completes.
        const auto reply = rig.read(0, 2, 200);
        QVERIFY(reply.hasError());
        QCOMPARE(reply.error().code, ErrorCode::Timeout);
        QVERIFY(rig.masterSession->counters().resyncBytes > 0);
    }

    void packetLossOnTheRequestPathIsInvisibleToTheSlave()
    {
        Rig rig;
        rig.build();

        QVERIFY(rig.device.faults()
                    .addRule(QStringLiteral("packet-loss"),
                             {{QStringLiteral("direction"), QStringLiteral("inbound")},
                              {QStringLiteral("trigger"), QStringLiteral("always")}})
                    .hasValue());

        const auto reply = rig.read(0, 2, 200);
        QVERIFY(reply.hasError());
        QCOMPARE(reply.error().code, ErrorCode::Timeout);
        QCOMPARE(rig.slaveSession->counters().framesDecoded, quint64{0});
    }

    void timeoutFaultWithholdsTheReply()
    {
        Rig rig;
        rig.build();

        QVERIFY(rig.device.faults()
                    .addRule(QStringLiteral("timeout"),
                             {{QStringLiteral("direction"), QStringLiteral("outbound")},
                              {QStringLiteral("trigger"), QStringLiteral("always")}})
                    .hasValue());

        const auto reply = rig.read(0, 2, 200);
        QVERIFY(reply.hasError());
        QCOMPARE(reply.error().code, ErrorCode::Timeout);

        // The request was processed; only the answer was suppressed.
        QCOMPARE(rig.slaveSession->counters().framesDecoded, quint64{1});
    }

    void everyNthFaultLetsOtherRequestsThrough()
    {
        Rig rig;
        rig.build();

        QVERIFY(rig.device.faults()
                    .addRule(QStringLiteral("packet-loss"),
                             {{QStringLiteral("direction"), QStringLiteral("outbound")},
                              {QStringLiteral("trigger"), QStringLiteral("every-nth")},
                              {QStringLiteral("everyNth"), 2}})
                    .hasValue());

        int successes = 0;
        int timeouts = 0;
        for (int attempt = 0; attempt < 4; ++attempt) {
            if (rig.read(0, 1, 150).hasValue()) {
                ++successes;
            } else {
                ++timeouts;
            }
        }

        QCOMPARE(successes, 2);
        QCOMPARE(timeouts, 2);
    }

    void disablingTheInjectorRestoresNormalService()
    {
        Rig rig;
        rig.build();

        QVERIFY(rig.device.faults()
                    .addRule(QStringLiteral("packet-loss"),
                             {{QStringLiteral("direction"), QStringLiteral("outbound")},
                              {QStringLiteral("trigger"), QStringLiteral("always")}})
                    .hasValue());

        QVERIFY(rig.read(0, 1, 150).hasError());

        rig.device.faults().setGloballyEnabled(false);
        QVERIFY(rig.read(0, 1, 400).hasValue());
    }

    // --- Device behaviour --------------------------------------------------

    void offlineDeviceStopsAnswering()
    {
        Rig rig;
        rig.build();

        QVERIFY(rig.read(0, 1).hasValue());

        rig.device.setOnline(false);
        const auto reply = rig.read(0, 1, 200);
        QVERIFY(reply.hasError());
        QCOMPARE(reply.error().code, ErrorCode::Timeout);

        rig.device.setOnline(true);
        QVERIFY(rig.read(0, 1).hasValue());
    }

    void unresponsiveStateSilencesTheDevice()
    {
        Rig rig;
        rig.build();

        rig.device.stateMachine().addState(
            simulator::StateDefinition{QStringLiteral("Running"), {}, {}, true, {}});
        rig.device.stateMachine().addState(
            simulator::StateDefinition{QStringLiteral("Fault"), {}, {}, false, {}});
        rig.device.stateMachine().addTransition(simulator::TransitionDefinition{
            QStringLiteral("*"), QStringLiteral("Fault"), QStringLiteral("trip"), {}, 0, {}});
        QVERIFY(rig.device.stateMachine().start(QStringLiteral("Running")).hasValue());

        QVERIFY(rig.read(0, 1).hasValue());

        QVERIFY(rig.device.postEvent(QStringLiteral("trip")).hasValue());
        const auto reply = rig.read(0, 1, 200);
        QVERIFY(reply.hasError());
        QCOMPARE(reply.error().code, ErrorCode::Timeout);
    }

    void signalGeneratorDrivesWhatTheMasterReads()
    {
        Rig rig;
        rig.build();

        QVERIFY(rig.device.signalEngine()
                    .addBinding(QStringLiteral("reg0"), QStringLiteral("constant"),
                                {{QStringLiteral("value"), 777.0}}, 10)
                    .hasValue());
        rig.device.signalEngine().tick(core::monotonicMs());

        const auto reply = rig.read(0, 1);
        QVERIFY(reply.hasValue());

        const auto* response = dynamic_cast<const ReadRegistersResponse*>(reply.value().get());
        QCOMPARE(response->values.first(), quint16{777});
    }

    void linkDropFailsOutstandingRequests()
    {
        Rig rig;
        rig.build();

        auto request = std::make_shared<ReadRegistersRequest>();
        request->startAddress = 0;
        request->quantity = 1;

        // Silence the slave first so the request stays outstanding.
        rig.device.setOnline(false);

        std::optional<Result<MessagePtr>> outcome;
        QVERIFY(rig.masterSession
                    ->sendRequest(request,
                                  [&outcome](Result<MessagePtr> reply) { outcome = std::move(reply); },
                                  5000)
                    .hasValue());

        rig.masterEndpoint.disconnectPeer();

        QTRY_VERIFY(outcome.has_value());
        QVERIFY(outcome->hasError());
        QCOMPARE(outcome->error().code, ErrorCode::NotConnected);
    }
};

QTEST_MAIN(EndToEndTest)
#include "tst_endtoend.moc"
