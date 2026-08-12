#include <core/Crc.h>
#include <core/Endian.h>
#include <core/HexUtils.h>
#include <protocol/CommandRegistry.h>
#include <protocol/IByteFilter.h>
#include <protocol/IFrameCodec.h>
#include <protocol/IMiddleware.h>
#include <protocol/PendingRequestTable.h>

#include <QTest>

#include <array>

using namespace hwsim::core;
using namespace hwsim::protocol;

namespace {

// --- A toy protocol used to exercise the framework --------------------------
//
// Wire format: AA <opcode> <length> <payload...> <xor8 of everything before>

struct PingMessage : MessageBase<PingMessage> {
    quint16 sequence{0};

    [[nodiscard]] static constexpr OpCode opcode() { return 0x01; }

    [[nodiscard]] static Result<PingMessage> decode(const Frame& frame)
    {
        if (frame.payload.size() < 2) {
            return makeError(ErrorCode::FrameMalformed,
                             QStringLiteral("ping payload must be 2 bytes"));
        }
        PingMessage message;
        message.sequence = endian::readBig<quint16>(hex::asBytes(frame.payload));
        return message;
    }

    [[nodiscard]] Result<QByteArray> encodeBody(const EncodeContext&) const
    {
        std::array<std::byte, 2> raw{};
        endian::writeBig<quint16>(raw, sequence);
        return hex::toByteArray(raw);
    }

    [[nodiscard]] QString describe() const override
    {
        return QStringLiteral("Ping seq=%1").arg(sequence);
    }
};

struct PongMessage : MessageBase<PongMessage> {
    quint16 sequence{0};

    [[nodiscard]] static constexpr OpCode opcode() { return 0x81; }

    [[nodiscard]] static Result<PongMessage> decode(const Frame& frame)
    {
        if (frame.payload.size() < 2) {
            return makeError(ErrorCode::FrameMalformed,
                             QStringLiteral("pong payload must be 2 bytes"));
        }
        PongMessage message;
        message.sequence = endian::readBig<quint16>(hex::asBytes(frame.payload));
        return message;
    }

    [[nodiscard]] Result<QByteArray> encodeBody(const EncodeContext&) const
    {
        std::array<std::byte, 2> raw{};
        endian::writeBig<quint16>(raw, sequence);
        return hex::toByteArray(raw);
    }

    [[nodiscard]] QString describe() const override
    {
        return QStringLiteral("Pong seq=%1").arg(sequence);
    }
};

class PingHandler {
public:
    [[nodiscard]] Result<MessagePtr> handle(const PingMessage& message, ExecutionContext&)
    {
        ++callCount;
        lastSequence = message.sequence;

        auto reply = std::make_shared<PongMessage>();
        reply->sequence = message.sequence;
        return MessagePtr(reply);
    }

    int callCount{0};
    quint16 lastSequence{0};
};

/// Handler that answers nothing, to cover the "no reply" path.
class SilentPingHandler {
public:
    [[nodiscard]] Result<MessagePtr> handle(const PingMessage&, ExecutionContext&)
    {
        return MessagePtr{};
    }
};

class FailingPingHandler {
public:
    [[nodiscard]] Result<MessagePtr> handle(const PingMessage&, ExecutionContext&)
    {
        return makeError(ErrorCode::DeviceFault, QStringLiteral("simulated handler failure"));
    }
};

class ToyCodec final : public IFrameCodec {
public:
    [[nodiscard]] QString name() const override { return QStringLiteral("toy"); }

    [[nodiscard]] FrameScanResult scan(std::span<const std::byte> buffer,
                                       hwsim::transport::Direction) const override
    {
        if (buffer.empty()) {
            return FrameScanResult::needMoreData();
        }
        if (std::to_integer<quint8>(buffer[0]) != 0xAA) {
            return FrameScanResult::discard(1, QStringLiteral("bad start byte"));
        }
        if (buffer.size() < 3) {
            return FrameScanResult::needMoreData();
        }

        const auto length = std::to_integer<std::size_t>(buffer[2]);
        const std::size_t total = 3 + length + 1;
        if (buffer.size() < total) {
            return FrameScanResult::needMoreData();
        }

        const auto expected = crc::xor8(buffer.first(total - 1));
        const auto actual = std::to_integer<quint8>(buffer[total - 1]);
        if (expected != actual) {
            return FrameScanResult::discard(total, QStringLiteral("checksum mismatch"));
        }

        Frame frame;
        frame.opcode = std::to_integer<OpCode>(buffer[1]);
        frame.payload = hex::toByteArray(buffer.subspan(3, length));
        frame.raw = hex::toByteArray(buffer.first(total));
        return FrameScanResult::ready(std::move(frame), total);
    }

    [[nodiscard]] Result<QByteArray> wrap(OpCode opcode, const QByteArray& body,
                                          const EncodeContext&) const override
    {
        QByteArray out;
        out.append(static_cast<char>(0xAA));
        out.append(static_cast<char>(opcode & 0xFF));
        out.append(static_cast<char>(body.size()));
        out.append(body);
        out.append(static_cast<char>(crc::xor8(hex::asBytes(out))));
        return out;
    }
};

/// Minimal IDeviceAccess so handlers that touch a device can be exercised.
class StubDevice final : public IDeviceAccess {
public:
    [[nodiscard]] QString deviceName() const override { return QStringLiteral("stub"); }
    [[nodiscard]] bool isResponsive() const override { return responsive; }
    [[nodiscard]] QString currentState() const override { return state; }

    [[nodiscard]] Result<QVariant> readParameter(const QString& key) override
    {
        return values.contains(key) ? Result<QVariant>(values.value(key))
                                    : Result<QVariant>(makeError(ErrorCode::NotFound, key));
    }
    [[nodiscard]] Result<void> writeParameter(const QString& key, const QVariant& value) override
    {
        values.insert(key, value);
        return success();
    }
    [[nodiscard]] Result<QVariant> readAddress(quint32 address) override
    {
        return readParameter(QString::number(address));
    }
    [[nodiscard]] Result<void> writeAddress(quint32 address, const QVariant& value) override
    {
        return writeParameter(QString::number(address), value);
    }
    [[nodiscard]] Result<void> postEvent(const QString&) override { return success(); }

    QVariantMap values;
    bool responsive{true};
    QString state{QStringLiteral("Running")};
};

Frame makePingFrame(quint16 sequence)
{
    Frame frame;
    frame.opcode = PingMessage::opcode();
    std::array<std::byte, 2> raw{};
    endian::writeBig<quint16>(raw, sequence);
    frame.payload = hex::toByteArray(raw);
    return frame;
}

/// Records the order in which stages run, to verify chain composition.
class OrderingMiddleware final : public IMiddleware {
public:
    OrderingMiddleware(QString stageName, int stagePriority, QStringList* log)
        : name_(std::move(stageName)), priority_(stagePriority), log_(log)
    {
    }

    [[nodiscard]] QString name() const override { return name_; }
    [[nodiscard]] int priority() const override { return priority_; }

    [[nodiscard]] Result<void> process(PipelineContext& context, const Next& next) override
    {
        log_->append(name_ + QStringLiteral(":before"));
        const auto result = next(context);
        log_->append(name_ + QStringLiteral(":after"));
        return result;
    }

private:
    QString name_;
    int priority_;
    QStringList* log_;
};

class BlockingMiddleware final : public IMiddleware {
public:
    [[nodiscard]] QString name() const override { return QStringLiteral("blocker"); }
    [[nodiscard]] int priority() const override { return 60; }

    [[nodiscard]] Result<void> process(PipelineContext& context, const Next&) override
    {
        context.suppressed = true;
        context.suppressReason = QStringLiteral("blocked by test");
        return success();
    }
};

class CorruptingFilter final : public IByteFilter {
public:
    [[nodiscard]] QString name() const override { return QStringLiteral("corrupt-last-byte"); }

    [[nodiscard]] ByteFilterDecision apply(QByteArray& bytes, const ByteFilterContext&) override
    {
        if (!bytes.isEmpty()) {
            bytes[bytes.size() - 1] = static_cast<char>(bytes.at(bytes.size() - 1) ^ 0xFF);
        }
        return ByteFilterDecision{true, 0, QStringLiteral("checksum corrupted")};
    }
};

class DroppingFilter final : public IByteFilter {
public:
    [[nodiscard]] QString name() const override { return QStringLiteral("drop-all"); }

    [[nodiscard]] ByteFilterDecision apply(QByteArray&, const ByteFilterContext&) override
    {
        return ByteFilterDecision::drop(QStringLiteral("packet loss"));
    }
};

} // namespace

class ProtocolTest : public QObject {
    Q_OBJECT

private slots:

    // --- CommandRegistry ---------------------------------------------------

    void registryBindsDecoderHandlerAndEncoderInOneCall()
    {
        CommandRegistry registry;
        QVERIFY(registry.bind<PingMessage>(std::make_shared<PingHandler>()));

        QVERIFY(registry.hasDecoder(PingMessage::opcode()));
        QVERIFY(registry.hasHandler(TypeId::of<PingMessage>()));
        QVERIFY(registry.hasEncoder(TypeId::of<PingMessage>()));
    }

    void registryParsesByOpcode()
    {
        CommandRegistry registry;
        registry.bind<PingMessage>(std::make_shared<PingHandler>());

        const auto parsed = registry.parse(makePingFrame(0x1234));
        QVERIFY(parsed.hasValue());
        QCOMPARE(parsed.value()->describe(), QStringLiteral("Ping seq=4660"));
    }

    void registryReportsUnknownOpcode()
    {
        CommandRegistry registry;

        Frame frame;
        frame.opcode = 0x77;
        const auto parsed = registry.parse(frame);

        QVERIFY(parsed.hasError());
        QCOMPARE(parsed.error().code, ErrorCode::UnknownCommand);
    }

    void registryDispatchesToTheTypedHandler()
    {
        auto handler = std::make_shared<PingHandler>();
        CommandRegistry registry;
        registry.bind<PingMessage>(handler);
        registry.bindEncoder<PongMessage>();

        const Frame frame = makePingFrame(7);
        StubDevice device;
        ExecutionContext context(&device, frame);

        PingMessage request;
        request.sequence = 7;

        const auto reply = registry.dispatch(request, context);
        QVERIFY(reply.hasValue());
        QCOMPARE(handler->callCount, 1);
        QCOMPARE(handler->lastSequence, quint16{7});
        QCOMPARE(reply.value()->describe(), QStringLiteral("Pong seq=7"));
    }

    void registryReportsMissingHandler()
    {
        CommandRegistry registry;
        registry.bindDecoder<PingMessage>(PingMessage::opcode());

        const Frame frame = makePingFrame(1);
        ExecutionContext context(nullptr, frame);
        PingMessage request;

        const auto reply = registry.dispatch(request, context);
        QVERIFY(reply.hasError());
        QCOMPARE(reply.error().code, ErrorCode::UnknownCommand);
    }

    void registryRejectsDuplicateOpcode()
    {
        CommandRegistry registry;
        QVERIFY(registry.bind<PingMessage>(std::make_shared<PingHandler>()));
        // Binding the same opcode again must not silently replace the first.
        QVERIFY(!registry.bindDecoder<PingMessage>(PingMessage::opcode()));
    }

    void registryEncodesThroughTheRegisteredEncoder()
    {
        CommandRegistry registry;
        registry.bindEncoder<PongMessage>();

        PongMessage message;
        message.sequence = 0xBEEF;

        const auto body = registry.encodeBody(message, EncodeContext{});
        QVERIFY(body.hasValue());
        QCOMPARE(body.value(), QByteArray::fromHex("BEEF"));

        const auto opcode = registry.opcodeFor(TypeId::of<PongMessage>());
        QVERIFY(opcode.hasValue());
        QCOMPARE(opcode.value(), OpCode{0x81});
    }

    void registryDescribesItsBindings()
    {
        CommandRegistry registry;
        registry.bind<PingMessage>(std::make_shared<PingHandler>(), QStringLiteral("Ping"));

        const auto bindings = registry.bindings();
        QCOMPARE(bindings.size(), std::size_t{1});
        QCOMPARE(bindings.front().opcode, OpCode{0x01});
        QVERIFY(bindings.front().hasDecoder);
        QVERIFY(bindings.front().hasHandler);
        QVERIFY(bindings.front().hasEncoder);
    }

    // --- Codec framing -----------------------------------------------------

    void codecNeedsMoreDataForPartialFrame()
    {
        ToyCodec codec;
        const QByteArray partial = QByteArray::fromHex("AA0102");
        const auto scan = codec.scan(hex::asBytes(partial), hwsim::transport::Direction::Inbound);
        QCOMPARE(scan.status, FrameScanStatus::NeedMoreData);
    }

    void codecExtractsOneFrameFromAStickyBuffer()
    {
        ToyCodec codec;
        PingMessage message;
        message.sequence = 0x0102;

        const auto body = message.encodeBody(EncodeContext{});
        const auto first = codec.wrap(PingMessage::opcode(), body.value(), EncodeContext{});
        QVERIFY(first.hasValue());

        // Two frames arriving in a single read.
        QByteArray sticky = first.value() + first.value();

        const auto scan = codec.scan(hex::asBytes(sticky), hwsim::transport::Direction::Inbound);
        QCOMPARE(scan.status, FrameScanStatus::FrameReady);
        QCOMPARE(scan.consumed, static_cast<std::size_t>(first.value().size()));
        QCOMPARE(scan.frame.opcode, PingMessage::opcode());
    }

    void codecDiscardsLeadingGarbage()
    {
        ToyCodec codec;
        const QByteArray noisy = QByteArray::fromHex("FFEEAA");
        const auto scan = codec.scan(hex::asBytes(noisy), hwsim::transport::Direction::Inbound);
        QCOMPARE(scan.status, FrameScanStatus::Discard);
        QCOMPARE(scan.consumed, std::size_t{1});
    }

    void codecDiscardsFrameWithBadChecksum()
    {
        ToyCodec codec;
        QByteArray frame = QByteArray::fromHex("AA010201020F");  // last byte is wrong
        const auto scan = codec.scan(hex::asBytes(frame), hwsim::transport::Direction::Inbound);
        QCOMPARE(scan.status, FrameScanStatus::Discard);
        QVERIFY(scan.diagnostic.contains(QStringLiteral("checksum")));
    }

    // --- Middleware --------------------------------------------------------

    void middlewareRunsInPriorityOrderAndUnwinds()
    {
        QStringList log;
        MiddlewareChain chain;
        chain.add(std::make_shared<OrderingMiddleware>(QStringLiteral("outer"), 10, &log));
        chain.add(std::make_shared<OrderingMiddleware>(QStringLiteral("inner"), 90, &log));

        const Frame frame = makePingFrame(1);
        StubDevice device;
        ExecutionContext execution(&device, frame);
        auto request = std::make_shared<PingMessage>();
        PipelineContext context{execution, frame, request, nullptr, false, {}, 0};

        const auto result = chain.run(context, [&log](PipelineContext&) -> Result<void> {
            log.append(QStringLiteral("handler"));
            return success();
        });

        QVERIFY(result.hasValue());
        QCOMPARE(log, QStringList({QStringLiteral("outer:before"), QStringLiteral("inner:before"),
                                   QStringLiteral("handler"), QStringLiteral("inner:after"),
                                   QStringLiteral("outer:after")}));
    }

    void middlewareCanShortCircuitTheChain()
    {
        MiddlewareChain chain;
        chain.add(std::make_shared<BlockingMiddleware>());

        const Frame frame = makePingFrame(1);
        ExecutionContext execution(nullptr, frame);
        auto request = std::make_shared<PingMessage>();
        PipelineContext context{execution, frame, request, nullptr, false, {}, 0};

        bool handlerRan = false;
        const auto result = chain.run(context, [&handlerRan](PipelineContext&) -> Result<void> {
            handlerRan = true;
            return success();
        });

        QVERIFY(result.hasValue());
        QVERIFY(!handlerRan);
        QVERIFY(context.suppressed);
        QCOMPARE(context.suppressReason, QStringLiteral("blocked by test"));
    }

    void deviceStateGateSuppressesWhenDeviceIsDown()
    {
        MiddlewareChain chain;
        chain.add(std::make_shared<DeviceStateGateMiddleware>());

        StubDevice device;
        device.responsive = false;
        device.state = QStringLiteral("Fault");

        const Frame frame = makePingFrame(1);
        ExecutionContext execution(&device, frame);
        auto request = std::make_shared<PingMessage>();
        PipelineContext context{execution, frame, request, nullptr, false, {}, 0};

        bool handlerRan = false;
        QVERIFY(chain.run(context, [&handlerRan](PipelineContext&) -> Result<void> {
                         handlerRan = true;
                         return success();
                     }).hasValue());

        QVERIFY(!handlerRan);
        QVERIFY(context.suppressed);
        QVERIFY(context.suppressReason.contains(QStringLiteral("Fault")));
    }

    void statisticsMiddlewareCountsOutcomes()
    {
        auto statistics = std::make_shared<StatisticsMiddleware>();
        MiddlewareChain chain;
        chain.add(statistics);

        const Frame frame = makePingFrame(1);
        StubDevice device;

        for (int i = 0; i < 3; ++i) {
            ExecutionContext execution(&device, frame);
            auto request = std::make_shared<PingMessage>();
            PipelineContext context{execution, frame, request, nullptr, false, {}, 0};
            QVERIFY(chain.run(context, [](PipelineContext& ctx) -> Result<void> {
                             ctx.response = std::make_shared<PongMessage>();
                             return success();
                         }).hasValue());
        }

        {
            ExecutionContext execution(&device, frame);
            auto request = std::make_shared<PingMessage>();
            PipelineContext context{execution, frame, request, nullptr, false, {}, 0};
            QVERIFY(chain.run(context, [](PipelineContext&) -> Result<void> {
                             return makeError(ErrorCode::DeviceFault, QStringLiteral("boom"));
                         }).hasError());
        }

        QCOMPARE(statistics->counters().requests, quint64{4});
        QCOMPARE(statistics->counters().replies, quint64{3});
        QCOMPARE(statistics->counters().failures, quint64{1});
    }

    void handlerErrorsPropagateThroughTheChain()
    {
        CommandRegistry registry;
        registry.bind<PingMessage>(std::make_shared<FailingPingHandler>());

        const Frame frame = makePingFrame(1);
        StubDevice device;
        ExecutionContext execution(&device, frame);
        PingMessage request;

        const auto result = registry.dispatch(request, execution);
        QVERIFY(result.hasError());
        QCOMPARE(result.error().code, ErrorCode::DeviceFault);
    }

    void handlerMayDeclineToReply()
    {
        CommandRegistry registry;
        registry.bind<PingMessage>(std::make_shared<SilentPingHandler>());

        const Frame frame = makePingFrame(1);
        ExecutionContext execution(nullptr, frame);
        PingMessage request;

        const auto result = registry.dispatch(request, execution);
        QVERIFY(result.hasValue());
        QVERIFY(result.value() == nullptr);
    }

    // --- Byte filters ------------------------------------------------------

    void byteFilterCanCorruptInPlace()
    {
        ByteFilterChain chain;
        chain.add(std::make_shared<CorruptingFilter>());

        QByteArray bytes = QByteArray::fromHex("AA010201020A");
        const auto decision = chain.apply(bytes, ByteFilterContext{});

        QVERIFY(decision.deliver);
        QCOMPARE(static_cast<quint8>(bytes.at(bytes.size() - 1)), quint8{0xF5});
        QVERIFY(decision.note.contains(QStringLiteral("corrupted")));
    }

    void byteFilterCanDropTheBuffer()
    {
        ByteFilterChain chain;
        chain.add(std::make_shared<DroppingFilter>());

        QByteArray bytes = QByteArray::fromHex("AABBCC");
        const auto decision = chain.apply(bytes, ByteFilterContext{});

        QVERIFY(!decision.deliver);
        QVERIFY(decision.note.contains(QStringLiteral("packet loss")));
    }

    void byteFilterDropShortCircuitsLaterFilters()
    {
        ByteFilterChain chain;
        chain.add(std::make_shared<DroppingFilter>());
        chain.add(std::make_shared<CorruptingFilter>());

        QByteArray bytes = QByteArray::fromHex("AABBCC");
        const QByteArray original = bytes;
        const auto decision = chain.apply(bytes, ByteFilterContext{});

        QVERIFY(!decision.deliver);
        QCOMPARE(bytes, original);
    }

    // --- Pending requests --------------------------------------------------

    void pendingTableMatchesOldestWhenThereIsNoKey()
    {
        PendingRequestTable table;
        QStringList resolved;

        for (int i = 0; i < 2; ++i) {
            auto request = std::make_shared<PingMessage>();
            request->sequence = static_cast<quint16>(i);
            table.add({}, request, [&resolved, i](Result<MessagePtr> result) {
                resolved.append(QStringLiteral("%1:%2").arg(i).arg(
                    result.hasValue() ? result.value()->describe() : result.error().toString()));
            }, 1000, 0);
        }

        auto reply = std::make_shared<PongMessage>();
        reply->sequence = 42;
        QVERIFY(table.resolve({}, reply));

        QCOMPARE(resolved.size(), 1);
        QVERIFY(resolved.front().startsWith(QStringLiteral("0:")));
        QCOMPARE(table.size(), std::size_t{1});
    }

    void pendingTableMatchesByCorrelationKey()
    {
        PendingRequestTable table;
        QString resolvedKey;

        table.add(QStringLiteral("t1"), std::make_shared<PingMessage>(),
                  [&resolvedKey](Result<MessagePtr>) { resolvedKey = QStringLiteral("t1"); }, 1000, 0);
        table.add(QStringLiteral("t2"), std::make_shared<PingMessage>(),
                  [&resolvedKey](Result<MessagePtr>) { resolvedKey = QStringLiteral("t2"); }, 1000, 0);

        QVERIFY(table.resolve(QStringLiteral("t2"), std::make_shared<PongMessage>()));
        QCOMPARE(resolvedKey, QStringLiteral("t2"));
        QCOMPARE(table.size(), std::size_t{1});
    }

    void pendingTableExpiresWithTimeoutError()
    {
        PendingRequestTable table;
        std::optional<Error> observed;

        table.add({}, std::make_shared<PingMessage>(),
                  [&observed](Result<MessagePtr> result) {
                      if (result.hasError()) {
                          observed = result.error();
                      }
                  },
                  100, 0);

        QVERIFY(table.takeExpired(50).empty());
        QCOMPARE(table.takeExpired(150).size(), std::size_t{1});

        QVERIFY(observed.has_value());
        QCOMPARE(observed->code, ErrorCode::Timeout);
        QVERIFY(table.isEmpty());
    }

    void pendingTableFailsEverythingWhenLinkDrops()
    {
        PendingRequestTable table;
        int failures = 0;

        for (int i = 0; i < 3; ++i) {
            table.add({}, std::make_shared<PingMessage>(),
                      [&failures](Result<MessagePtr> result) {
                          if (result.hasError()) ++failures;
                      },
                      1000, 0);
        }

        table.failAll(makeError(ErrorCode::NotConnected, QStringLiteral("link dropped")));
        QCOMPARE(failures, 3);
        QVERIFY(table.isEmpty());
    }

    void pendingTableResolveReportsWhenNothingWaits()
    {
        PendingRequestTable table;
        QVERIFY(!table.resolve({}, std::make_shared<PongMessage>()));
    }
};

QTEST_MAIN(ProtocolTest)
#include "tst_protocol.moc"
