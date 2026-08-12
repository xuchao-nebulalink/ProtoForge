// SW6 over a real protocol stack: two sessions on an in-memory link, the
// plugin's own codec and registries on both ends.
//
// The point of these tests is what the unit tests cannot cover: that the ASCII
// question-answer channel and the unsolicited 0x81 stream survive sharing one
// link, one codec and one pending-request table.

#include "Sw6Codec.h"
#include "Sw6DeviceState.h"
#include "Sw6Handlers.h"
#include "Sw6Messages.h"
#include "Sw6Types.h"

#include <core/Logger.h>
#include <protocol/CommandRegistry.h>
#include <protocol/ProtocolSession.h>
#include <transport/LoopbackTransport.h>

#include <QTest>

#include <optional>

using namespace hwsim;
using namespace hwsim::plugins::sw6;
using hwsim::core::ErrorCode;
using hwsim::core::Result;
using hwsim::protocol::CommandRegistry;
using hwsim::protocol::EncodeContext;
using hwsim::protocol::MessagePtr;
using hwsim::protocol::ProtocolSession;
using hwsim::transport::LoopbackTransport;
using hwsim::transport::TransportConfig;
using hwsim::transport::TransportKind;
using hwsim::transport::TransportRole;

namespace {

MessagePtr systemCommand(const QString& name, Sw6Values values = {})
{
    return std::make_shared<SystemCommand>(SystemCommand::make(name, std::move(values)));
}

MessagePtr axisCommand(const QString& name, std::initializer_list<Sw6Element> elements)
{
    return std::make_shared<AxisCommand>(AxisCommand::make(name, elements));
}

MessagePtr channelCommand(const QString& name, std::initializer_list<Sw6Element> elements)
{
    return std::make_shared<ChannelCommand>(ChannelCommand::make(name, elements));
}

MessagePtr trajectoryCommand(const QString& name, Sw6Values values = {})
{
    return std::make_shared<TrajectoryCommand>(TrajectoryCommand::make(name, std::move(values)));
}

Sw6Element axisValue(int axis, double value)
{
    return Sw6Element{axis, Sw6Values{Sw6Value::ofFloat(value)}};
}

Sw6Element axisFlag(int axis, qint64 value)
{
    return Sw6Element{axis, Sw6Values{Sw6Value::ofInt(value)}};
}

QByteArray realtimeBytes(const Sw6StreamSample& sample)
{
    const Sw6Codec codec;
    const RealtimeFrame frame = RealtimeFrame::fromSample(sample);
    const auto body = frame.encodeBody(EncodeContext{});
    if (body.hasError()) {
        return {};
    }
    const auto wrapped = codec.wrap(kRealtimeStreamOpcode, body.value(), EncodeContext{});
    return wrapped.hasError() ? QByteArray{} : wrapped.value();
}

/// One controller and one master, wired through a loopback pair.
struct Rig {
    LoopbackTransport deviceEndpoint;
    LoopbackTransport masterEndpoint;

    std::shared_ptr<Sw6DeviceState> device{std::make_shared<Sw6DeviceState>()};
    std::shared_ptr<Sw6DeviceState> mirror{std::make_shared<Sw6DeviceState>()};

    std::unique_ptr<ProtocolSession> deviceSession;
    std::unique_ptr<ProtocolSession> masterSession;

    void build(bool withDevice = true, int streamIntervalMs = 0)
    {
        QVERIFY(deviceEndpoint.open(TransportConfig(TransportKind::Loopback)).hasValue());
        QVERIFY(masterEndpoint.open(TransportConfig(TransportKind::Loopback)).hasValue());
        LoopbackTransport::connectPair(&deviceEndpoint, &masterEndpoint);

        if (withDevice) {
            auto registry = std::make_shared<CommandRegistry>();
            QVERIFY(registerSw6Commands(*registry, device, false, streamIntervalMs).hasValue());

            ProtocolSession::Options options;
            options.name = QStringLiteral("hexapod");
            options.role = TransportRole::Responder;
            options.publishEvents = false;

            deviceSession = std::make_unique<ProtocolSession>(deviceEndpoint.primaryLink(),
                                                              std::make_unique<Sw6Codec>(),
                                                              std::move(registry), options);
        }

        auto masterRegistry = std::make_shared<CommandRegistry>();
        QVERIFY(registerSw6Commands(*masterRegistry, mirror, true).hasValue());

        ProtocolSession::Options masterOptions;
        masterOptions.name = QStringLiteral("master");
        masterOptions.role = TransportRole::Initiator;
        masterOptions.defaultTimeoutMs = 300;
        masterOptions.publishEvents = false;

        masterSession = std::make_unique<ProtocolSession>(masterEndpoint.primaryLink(),
                                                          std::make_unique<Sw6Codec>(),
                                                          std::move(masterRegistry), masterOptions);
    }

    [[nodiscard]] Result<MessagePtr> request(const MessagePtr& message, int timeoutMs = 400)
    {
        std::optional<Result<MessagePtr>> outcome;
        const auto sent = masterSession->sendRequest(
            message, [&outcome](Result<MessagePtr> reply) { outcome = std::move(reply); },
            timeoutMs);
        if (sent.hasError()) {
            return sent.error();
        }

        const int budget = timeoutMs + 500;
        if (!QTest::qWaitFor([&outcome] { return outcome.has_value(); }, budget)) {
            return core::makeError(ErrorCode::Timeout,
                                   QStringLiteral("no reply within %1 ms").arg(budget));
        }
        return *outcome;
    }

    /// Sends a command and returns the decoded reply, or nullopt on timeout.
    [[nodiscard]] std::optional<Sw6Reply> exchange(const MessagePtr& message, int timeoutMs = 400)
    {
        const auto reply = request(message, timeoutMs);
        if (reply.hasError()) {
            return std::nullopt;
        }
        const auto* decoded = dynamic_cast<const Sw6Reply*>(reply.value().get());
        return decoded != nullptr ? std::optional<Sw6Reply>(*decoded) : std::nullopt;
    }

    /// Servo on and referenced, the state section 8 reaches before it moves.
    void enableMotion()
    {
        QVERIFY(exchange(axisCommand(QStringLiteral("SVO"),
                                     {axisFlag(0, 1), axisFlag(1, 1), axisFlag(2, 1),
                                      axisFlag(3, 1), axisFlag(4, 1), axisFlag(5, 1)}))
                    .has_value());
        QVERIFY(exchange(std::make_shared<LegCommand>(LegCommand::make(QStringLiteral("FRF"), {})))
                    .has_value());
    }
};

} // namespace

class Sw6EndToEndTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        core::Logger::instance().setAsynchronous(false);
        core::Logger::instance().setLevel(core::LogLevel::Error);
    }

    void identityHandshakeCompletes()
    {
        Rig rig;
        rig.build();

        const auto reply = rig.exchange(systemCommand(QStringLiteral("IDN")));
        QVERIFY(reply.has_value());
        QCOMPARE(reply->command, QStringLiteral("IDN"));
        QCOMPARE(reply->errorCode, err::kSuccess);
        QCOMPARE(reply->values.at(0).label, QStringLiteral("SW6_HEXAPOD_V2"));
    }

    /// The startup sequence of protocol section 8.
    void theServoHomeMoveSequenceRunsThrough()
    {
        Rig rig;
        rig.build();

        const auto servo = rig.exchange(axisCommand(
            QStringLiteral("SVO"), {axisFlag(0, 1), axisFlag(1, 1), axisFlag(2, 1), axisFlag(3, 1),
                                    axisFlag(4, 1), axisFlag(5, 1)}));
        QVERIFY(servo.has_value());
        QCOMPARE(servo->errorCode, err::kSuccess);

        const auto homed = rig.exchange(std::make_shared<LegCommand>(
            LegCommand::make(QStringLiteral("FRF"), {})));
        QVERIFY(homed.has_value());
        QCOMPARE(homed->errorCode, err::kSuccess);

        const auto statusReply = rig.exchange(systemCommand(QStringLiteral("STAq")));
        QVERIFY(statusReply.has_value());
        const auto word = static_cast<quint32>(statusReply->values.first().asInt());
        QVERIFY((word & status::kReferenced) != 0);

        const auto speed =
            rig.exchange(axisCommand(QStringLiteral("VEL"), {axisValue(0, 5.0)}));
        QVERIFY(speed.has_value());
        QCOMPARE(speed->errorCode, err::kSuccess);

        const auto moved = rig.exchange(
            axisCommand(QStringLiteral("MOV"), {axisValue(0, 1.5), axisValue(3, 0.2)}));
        QVERIFY(moved.has_value());
        QCOMPARE(moved->errorCode, err::kSuccess);

        const auto pose = rig.exchange(axisCommand(QStringLiteral("POSq"), {}));
        QVERIFY(pose.has_value());
        QCOMPARE(pose->valueForAxis(0).value_or(0.0), 1.5);
        QCOMPARE(pose->valueForAxis(3).value_or(0.0), 0.2);

        // The device model moved with it.
        QCOMPARE(rig.device->axis(AxisField::ActualPose)[0], 1.5);
    }

    void aRejectedCommandComesBackAsAnErrorCode()
    {
        Rig rig;
        rig.build();

        // No servo, no reference: section 4.7 calls this 0x04000001.
        const auto reply =
            rig.exchange(axisCommand(QStringLiteral("MOV"), {axisValue(0, 1.0)}));
        QVERIFY(reply.has_value());
        QCOMPARE(reply->errorCode, err::kServoOff);
    }

    void anUnknownCommandIsAnsweredRatherThanIgnored()
    {
        Rig rig;
        rig.build();

        // Sent as raw bytes: the master's registry has no message type for a
        // command name that does not exist, which is exactly the situation a
        // firmware with extra commands would put us in.
        QVERIFY(rig.masterSession->sendRaw(buildAsciiFrame(QStringLiteral("XYZ"))).hasValue());

        QTRY_COMPARE(rig.deviceSession->counters().framesSent, quint64{1});
    }

    /// Section 5.12 over the wire: three commands, three replies, and a
    /// platform that ends up on the last point of the trajectory.
    void aTrajectoryIsWrittenCheckedAndRun()
    {
        Rig rig;
        rig.build();
        rig.enableMotion();

        const auto point = [](double x, double y) {
            return Sw6Values{Sw6Value::ofInt(1),   Sw6Value::ofInt(0),
                             Sw6Value::ofInt(0),   Sw6Value::ofFloat(0.0),
                             Sw6Value::ofInt(0),   Sw6Value::ofText(QStringLiteral("X")),
                             Sw6Value::ofFloat(x), Sw6Value::ofText(QStringLiteral("Y")),
                             Sw6Value::ofFloat(y)};
        };

        QVERIFY(rig.exchange(trajectoryCommand(QStringLiteral("TGA"), point(1.0, -1.0)))
                    .has_value());

        Sw6Values second = point(2.5, -1.0);
        second[1] = Sw6Value::ofInt(1);
        QVERIFY(rig.exchange(trajectoryCommand(QStringLiteral("TGA"), second)).has_value());

        const auto checked =
            rig.exchange(trajectoryCommand(QStringLiteral("TGF"), {Sw6Value::ofInt(1)}));
        QVERIFY(checked.has_value());
        QCOMPARE(checked->values.at(0).asInt(), qint64{1});

        const auto started =
            rig.exchange(trajectoryCommand(QStringLiteral("TGS"), {Sw6Value::ofInt(1)}));
        QVERIFY(started.has_value());
        QCOMPARE(started->errorCode, err::kSuccess);

        QCOMPARE(rig.device->axis(AxisField::ActualPose)[0], 2.5);
        QCOMPARE(rig.device->axis(AxisField::ActualPose)[1], -1.0);
    }

    void anIoLineIsWrittenAndReadBack()
    {
        Rig rig;
        rig.build();

        QVERIFY(rig.exchange(channelCommand(QStringLiteral("DIO"),
                                            {Sw6Element{1, Sw6Values{Sw6Value::ofInt(1)}}}))
                    .has_value());

        const auto lines = rig.exchange(channelCommand(QStringLiteral("DIOq"), {}));
        QVERIFY(lines.has_value());
        QCOMPARE(lines->valueForLeg(1).value_or(0.0), 1.0);
        QCOMPARE(lines->valueForLeg(2).value_or(-1.0), 0.0);
    }

    // --- The two frame families sharing one link ---------------------------

    void theDeviceStreamReachesTheMaster()
    {
        Rig rig;
        rig.build();

        // `$RSE,3d` adds the theoretical pose and the actual leg lengths.
        const auto mask = rig.exchange(
            systemCommand(QStringLiteral("RSE"), {Sw6Value::ofInt(0x03)}));
        QVERIFY(mask.has_value());
        QCOMPARE(mask->errorCode, err::kSuccess);

        rig.device->axis(AxisField::ActualPose)[0] = 2.5;

        // ProtocolSession::send() is the unsolicited path: no pending entry is
        // registered, which is what a device-driven stream needs.
        const auto frame = std::make_shared<RealtimeFrame>(
            RealtimeFrame::fromSample(rig.device->sample()));
        QVERIFY(rig.deviceSession->send(frame).hasValue());

        QTRY_COMPARE(rig.mirror->axis(AxisField::ActualPose)[0], 2.5);

        // The RSE reply and the stream frame, and nothing thrown away in
        // between: both families framed cleanly on the same buffer.
        QCOMPARE(rig.masterSession->counters().framesDecoded, quint64{2});
        QCOMPARE(rig.masterSession->counters().resyncBytes, quint64{0});
    }

    /// The AD block of section 6.3 makes the stream frame 75 bytes instead of
    /// 35, which the receiver has to take from the length field rather than
    /// from a fixed size.
    void theAnalogBlockWidensTheStreamFrame()
    {
        Rig rig;
        rig.build();

        QVERIFY(rig.exchange(systemCommand(QStringLiteral("RSE"),
                                           {Sw6Value::ofInt(stream::kMaskAnalog)}))
                    .has_value());

        const auto frame = std::make_shared<RealtimeFrame>(
            RealtimeFrame::fromSample(rig.device->sample()));
        QCOMPARE(static_cast<int>(frame->records.size()),
                 stream::expectedRecordCount(stream::kMaskAnalog));
        QVERIFY(rig.deviceSession->send(frame).hasValue());

        QTRY_COMPARE(rig.masterSession->counters().framesDecoded, quint64{2});
        QCOMPARE(rig.masterSession->counters().resyncBytes, quint64{0});

        // Channel 0 carries the alignment signal, so the block is not zeroes.
        QCOMPARE(rig.device->sample().analog[0], rig.device->analogVoltage(0));
        QVERIFY(rig.device->analogVoltage(0) > 0.0);
    }

    /// The device talks on its own, which no handler can express: the plugin
    /// installs an unsolicited source and the session polls it.
    void theDevicePushesTheStreamOnItsOwn()
    {
        Rig rig;
        rig.build(true, 10);

        rig.device->axis(AxisField::ActualPose)[1] = -1.75;

        QTRY_COMPARE(rig.mirror->axis(AxisField::ActualPose)[1], -1.75);
        QVERIFY(rig.masterSession->counters().framesDecoded >= 1);

        // Commands still get through while the stream is running.
        const auto reply = rig.exchange(systemCommand(QStringLiteral("VER")));
        QVERIFY(reply.has_value());
        QCOMPARE(reply->errorCode, err::kSuccess);

        // `$RTO` prepares for power-off, which stops the stream.
        QVERIFY(rig.exchange(systemCommand(QStringLiteral("RTO"))).has_value());
        QTest::qWait(40);
        const quint64 afterShutdown = rig.masterSession->counters().framesDecoded;
        QTest::qWait(60);
        QCOMPARE(rig.masterSession->counters().framesDecoded, afterShutdown);
    }

    void aStreamFrameDoesNotCompleteAPendingRequest()
    {
        // No device session, so the request stays outstanding while the stream
        // keeps arriving. With a correlation key that matched anything, the
        // stream would resolve the request and the master would believe it got
        // an answer.
        Rig rig;
        rig.build(false);

        std::optional<Result<MessagePtr>> outcome;
        QVERIFY(rig.masterSession
                    ->sendRequest(systemCommand(QStringLiteral("STAq")),
                                  [&outcome](Result<MessagePtr> reply) {
                                      outcome = std::move(reply);
                                  },
                                  200)
                    .hasValue());

        Sw6StreamSample sample;
        sample.actualPose[0] = 4.0;
        const QByteArray stream = realtimeBytes(sample);
        QCOMPARE(rig.deviceEndpoint.primaryLink()->send(stream),
                 static_cast<qint64>(stream.size()));

        QTRY_VERIFY(outcome.has_value());
        QVERIFY(outcome->hasError());
        QCOMPARE(outcome->error().code, ErrorCode::Timeout);

        // The stream itself was still delivered to its handler.
        QCOMPARE(rig.mirror->axis(AxisField::ActualPose)[0], 4.0);
    }

    void interleavedStreamAndCommandBothArrive()
    {
        Rig rig;
        rig.build();

        Sw6StreamSample sample;
        sample.actualPose[0] = 1.25;

        // A stream frame glued in front of the reply path: the codec has to
        // route on the first byte of each frame, not on the session's mood.
        QVERIFY(rig.deviceEndpoint.primaryLink()->send(realtimeBytes(sample)) > 0);

        const auto reply = rig.exchange(systemCommand(QStringLiteral("VER")));
        QVERIFY(reply.has_value());
        QCOMPARE(reply->errorCode, err::kSuccess);
        QCOMPARE(rig.mirror->axis(AxisField::ActualPose)[0], 1.25);
    }

    // --- Stream reassembly -------------------------------------------------

    void aFragmentedCommandIsReassembled()
    {
        Rig rig;
        rig.build();

        const QByteArray frame = buildAsciiFrame(QStringLiteral("STAq"));
        for (const char byte : frame) {
            rig.masterEndpoint.primaryLink()->send(QByteArray(1, byte));
            QTest::qWait(1);
        }

        QTRY_COMPARE(rig.deviceSession->counters().framesDecoded, quint64{1});
        QTRY_COMPARE(rig.deviceSession->counters().framesSent, quint64{1});
    }

    void backToBackFramesAreBothDecoded()
    {
        Rig rig;
        rig.build();

        const QByteArray twice =
            buildAsciiFrame(QStringLiteral("STAq")) + buildAsciiFrame(QStringLiteral("VER"));
        rig.masterEndpoint.primaryLink()->send(twice);

        QTRY_COMPARE(rig.deviceSession->counters().framesDecoded, quint64{2});
    }

    void noiseBetweenFramesIsDiscarded()
    {
        Rig rig;
        rig.build();

        rig.masterEndpoint.primaryLink()->send(QByteArray::fromHex("FFEE00")
                                               + buildAsciiFrame(QStringLiteral("STAq")));

        QTRY_COMPARE(rig.deviceSession->counters().framesDecoded, quint64{1});
        QCOMPARE(rig.deviceSession->counters().resyncBytes, quint64{3});
    }

    void aTruncatedFrameDoesNotSwallowTheNextOne()
    {
        Rig rig;
        rig.build();

        // The first frame lost its terminator, so only the second one counts.
        rig.masterEndpoint.primaryLink()->send(QByteArrayLiteral("$STAq")
                                               + buildAsciiFrame(QStringLiteral("VER")));

        QTRY_COMPARE(rig.deviceSession->counters().framesDecoded, quint64{1});
        QVERIFY(rig.deviceSession->counters().resyncBytes >= 5);
    }
};

QTEST_MAIN(Sw6EndToEndTest)
#include "tst_sw6_endtoend.moc"
