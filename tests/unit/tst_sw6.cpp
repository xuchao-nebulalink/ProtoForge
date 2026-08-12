// SW6 hexapod protocol: framing, checksums, the realtime stream and the
// responder's behaviour.
//
// The golden values come straight out of the protocol document: the checksum-8
// vectors of section 9.2 and the mask/length table of section 6.3.

#include "Sw6Codec.h"
#include "Sw6Commands.h"
#include "Sw6DeviceState.h"
#include "Sw6Handlers.h"
#include "Sw6Messages.h"
#include "Sw6Types.h"

#include <core/Crc.h>
#include <core/HexUtils.h>
#include <core/Logger.h>
#include <protocol/CommandRegistry.h>
#include <protocol/ExecutionContext.h>

#include <QSet>
#include <QTest>

#include <cmath>

using namespace hwsim;
using namespace hwsim::plugins::sw6;
using hwsim::core::hex::asBytes;
using hwsim::protocol::CommandRegistry;
using hwsim::protocol::EncodeContext;
using hwsim::protocol::ExecutionContext;
using hwsim::protocol::Frame;
using hwsim::protocol::FrameScanResult;
using hwsim::protocol::FrameScanStatus;
using hwsim::transport::Direction;

namespace {

FrameScanResult scanFrame(const QByteArray& bytes)
{
    const Sw6Codec codec;
    return codec.scan(asBytes(bytes), Direction::Inbound);
}

/// The responder side wired up the way a session wires it, so a test can push
/// a frame in and read the exact bytes that would go back on the wire.
class Responder {
public:
    Responder()
    {
        registerCommands();
    }

    [[nodiscard]] QByteArray exchange(const QByteArray& request)
    {
        const FrameScanResult scanned = codec_.scan(asBytes(request), Direction::Inbound);
        if (scanned.status != FrameScanStatus::FrameReady) {
            return {};
        }

        const Frame frame = scanned.frame;
        const auto parsed = registry_->parse(frame);
        if (parsed.hasError()) {
            return {};
        }

        ExecutionContext execution(nullptr, frame);
        const auto dispatched = registry_->dispatch(*parsed.value(), execution);
        if (dispatched.hasError() || !dispatched.value()) {
            return {};
        }

        EncodeContext context = EncodeContext::forReply(frame);
        const auto body = registry_->encodeBody(*dispatched.value(), context);
        if (body.hasError()) {
            return {};
        }

        const auto opcode = dispatched.value()->dynamicOpcode();
        if (!opcode.has_value()) {
            return {};
        }

        const auto wrapped = codec_.wrap(*opcode, body.value(), context);
        return wrapped.hasError() ? QByteArray{} : wrapped.value();
    }

    /// Reply as a decoded message, for assertions that care about values
    /// rather than bytes.
    [[nodiscard]] Sw6Reply reply(const QByteArray& request)
    {
        const QByteArray bytes = exchange(request);
        const FrameScanResult scanned = scanFrame(bytes);
        if (scanned.status != FrameScanStatus::FrameReady) {
            return {};
        }
        auto decoded = Sw6Reply::decode(scanned.frame);
        return decoded.hasError() ? Sw6Reply{} : decoded.value();
    }

    [[nodiscard]] Sw6DeviceState& state() { return *state_; }
    [[nodiscard]] std::shared_ptr<Sw6DeviceState> sharedState() const { return state_; }

    /// Servo on and referenced, which is what section 8 does before moving.
    void enableMotion()
    {
        QCOMPARE(reply(buildAsciiFrame(QStringLiteral("SVO"),
                                       QByteArrayLiteral("Xs,1d,Ys,1d,Zs,1d,Us,1d,Vs,1d,Ws,1d")))
                     .errorCode,
                 err::kSuccess);
        QCOMPARE(reply(buildAsciiFrame(QStringLiteral("FRF"))).errorCode, err::kSuccess);
    }

private:
    void registerCommands()
    {
        QVERIFY(registerSw6Commands(*registry_, state_, false).hasValue());
    }

    std::shared_ptr<Sw6DeviceState> state_{std::make_shared<Sw6DeviceState>()};
    std::shared_ptr<CommandRegistry> registry_{std::make_shared<CommandRegistry>()};
    Sw6Codec codec_;
};

} // namespace

class Sw6Test : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        core::Logger::instance().setAsynchronous(false);
        core::Logger::instance().setLevel(core::LogLevel::Error);
    }

    // --- Checksums and argument tokens -------------------------------------

    void checksumVectorsFromTheSpecification_data()
    {
        QTest::addColumn<QString>("command");
        QTest::addColumn<QByteArray>("arguments");
        QTest::addColumn<QByteArray>("frame");

        const auto add = [](const char* command, const char* arguments, const char* frame) {
            QTest::newRow(frame) << QString::fromLatin1(command) << QByteArray(arguments)
                                 << QByteArray(frame);
        };

        // Section 9.2, system / motion / servo.
        add("IDN", "", "$IDN;3A");
        add("VER", "", "$VER;4C");
        add("ERR", "", "$ERR;48");
        add("HLPq", "", "$HLPq;B4");
        add("STAq", "", "$STAq;B8");
        add("SAV", "", "$SAV;49");
        add("RTO", "", "$RTO;54");
        add("STP", "", "$STP;56");
        add("HLT", "", "$HLT;47");
        add("STF", "", "$STF;4C");
        add("GOH", "", "$GOH;3D");
        add("JOG", "Xs,5f", "$JOG,Xs,5f;FD");
        add("SVO", "Xs,1d", "$SVO,Xs,1d;0F");
        add("SVOq", "", "$SVOq;C8");
        add("ONTq", "Xs", "$ONTq,Xs;B8");
        add("EAX", "Xs,1d", "$EAX,Xs,1d;F5");
        add("LLC", "1d,0.12f", "$LLC,1d,0.12f;4E");
        add("LLC", "1d,0.12f,2d,-0.05f,3d,0f,4d,0.08f,5d,-0.03f,6d,0.01f",
            "$LLC,1d,0.12f,2d,-0.05f,3d,0f,4d,0.08f,5d,-0.03f,6d,0.01f;8F");
        add("LLCq", "", "$LLCq;AB");
        add("LLCq", "1d", "$LLCq,1d;6C");
        add("PAR", "ADCHs,0d,HPRTs,130f", "$PAR,ADCHs,0d,HPRTs,130f;B4");

        // Section 9.2, speed / homing / limits.
        add("VEL", "Xs,10f", "$VEL,Xs,10f;30");
        add("ACC", "Xs,50f", "$ACC,Xs,50f;14");
        add("DEC", "Xs,50f", "$DEC,Xs,50f;19");
        add("VMXq", "Xs", "$VMXq,Xs;C2");
        add("AMXq", "", "$AMXq;B6");
        add("RMXq", "", "$RMXq;C7");
        add("FRF", "", "$FRF;3D");
        add("NLM", "Xs,-10f", "$NLM,Xs,-10f;5D");
        add("PLM", "Xs,10f", "$PLM,Xs,10f;32");
        add("SSL", "Xs,1d", "$SSL,Xs,1d;09");
        add("TMNq", "", "$TMNq;BF");
        add("TMXq", "", "$TMXq;C9");
        add("LIMq", "", "$LIMq;B2");
        add("KINq", "Hs", "$KINq,Hs;99");
        add("GEOq", "B1s", "$GEOq,B1s;BD");
        add("VLS", "10f", "$VLS,10f;47");

        // Section 9.2, I/O and trajectories.
        add("DIO", "1d,1d", "$DIO,1d,1d;BD");
        add("CTO", "1d,3d,1f", "$CTO,1d,3d,1f;8C");
        add("TIOq", "", "$TIOq;BC");
        add("TGT", "10d", "$TGT,10d;3F");
        add("TGS", "", "$TGS;4D");

        // Section 9.2, alignment and dynamics identification.
        add("FRS", "ALIGNAs", "$FRS,ALIGNAs;95");
        add("FLM", "Xs,10f", "$FLM,Xs,10f;28");
        add("FSM", "Xs,10f,Ys,10f", "$FSM,Xs,10f,Ys,10f;1A");
        add("STE", "Xs,0.1f", "$STE,Xs,0.1f;63");
        add("IMP", "Xs,0.1f", "$IMP,Xs,0.1f;5D");
        add("WFR", "Xs,1d,0.1f,10f,1000f", "$WFR,Xs,1d,0.1f,10f,1000f;6D");
        add("DPO", "Xs", "$DPO,Xs;39");

        // Section 9.2, replies.
        add("STAq", "0x00000000,0x00000025", "$STAq,0x00000000,0x00000025;67");
        add("SVOq", "0x00000000,Xs,1d,Ys,1d,Zs,1d,Us,1d,Vs,1d,Ws,1d",
            "$SVOq,0x00000000,Xs,1d,Ys,1d,Zs,1d,Us,1d,Vs,1d,Ws,1d;69");
        add("VELq", "0x00000000,Xs,10f", "$VELq,0x00000000,Xs,10f;F5");
        add("FRFq", "0x00000000,Xs,1d", "$FRFq,0x00000000,Xs,1d;BA");
        add("TMNq", "0x00000000,Xs,-50f,Ys,-50f,Zs,-25f,Us,-0.2f,Vs,-0.2f,Ws,-0.4f",
            "$TMNq,0x00000000,Xs,-50f,Ys,-50f,Zs,-25f,Us,-0.2f,Vs,-0.2f,Ws,-0.4f;37");
    }

    void checksumVectorsFromTheSpecification()
    {
        QFETCH(QString, command);
        QFETCH(QByteArray, arguments);
        QFETCH(QByteArray, frame);

        QCOMPARE(buildAsciiFrame(command, arguments), frame);
    }

    void argumentTokensRoundTrip()
    {
        Sw6Values values;
        QCOMPARE(parseValues(QByteArrayLiteral("Xs,1.5f,3d,0x0300000C"), values), err::kSuccess);
        QCOMPARE(static_cast<int>(values.size()), 4);

        QVERIFY(values.at(0).isText());
        QCOMPARE(values.at(0).label, QStringLiteral("X"));
        QCOMPARE(values.at(1).asDouble(), 1.5);
        QCOMPARE(values.at(2).asInt(), qint64{3});
        QCOMPARE(values.at(3).asInt(), qint64{0x0300000C});

        QCOMPARE(joinValues(values), QByteArrayLiteral("Xs,1.5f,3d,0x0300000C"));
    }

    void argumentTokensReportTheProtocolErrorCode()
    {
        Sw6Values values;
        QCOMPARE(parseValues(QByteArrayLiteral("Xs,1.5"), values), err::kMissingArgumentType);
        QCOMPARE(parseValues(QByteArrayLiteral("Xs,abcf"), values), err::kArgumentTypeMismatch);
    }

    void commandOpcodesAreUniqueAndOutsideTheReservedRange()
    {
        QSet<protocol::OpCode> seen;
        for (const QString& name : commandNames()) {
            const protocol::OpCode opcode = commandOpcodeOf(name);
            QVERIFY2(!seen.contains(opcode), qPrintable(name));
            seen.insert(opcode);

            QVERIFY(opcode != kRealtimeStreamOpcode);
            QVERIFY(opcode != kUnknownCommandOpcode);
            QCOMPARE(commandNameFor(opcode), name);
        }
        QCOMPARE(seen.size(), commandNames().size());
    }

    // --- ASCII framing -----------------------------------------------------

    void asciiCommandIsFramedAndRouted()
    {
        const QByteArray bytes = buildAsciiFrame(QStringLiteral("MOV"),
                                                 QByteArrayLiteral("Xs,1.5f,Us,0.2f"));
        const FrameScanResult scanned = scanFrame(bytes);

        QCOMPARE(scanned.status, FrameScanStatus::FrameReady);
        QCOMPARE(scanned.consumed, static_cast<std::size_t>(bytes.size()));
        QCOMPARE(scanned.frame.opcode, commandOpcode("MOV"));
        QCOMPARE(scanned.frame.payload, QByteArrayLiteral("Xs,1.5f,Us,0.2f"));
        QCOMPARE(scanned.frame.attribute(QString::fromLatin1(kCommandAttribute)).toString(),
                 QStringLiteral("MOV"));
    }

    void argumentlessCommandHasAnEmptyPayload()
    {
        const FrameScanResult scanned = scanFrame(QByteArrayLiteral("$STAq;B8"));

        QCOMPARE(scanned.status, FrameScanStatus::FrameReady);
        QVERIFY(scanned.frame.payload.isEmpty());
        QCOMPARE(scanned.frame.opcode, commandOpcode("STAq"));
    }

    void aPartialFrameWaitsForTheRest()
    {
        const QByteArray complete = buildAsciiFrame(QStringLiteral("VEL"),
                                                    QByteArrayLiteral("Xs,10f"));
        for (qsizetype length = 1; length < complete.size(); ++length) {
            const FrameScanResult scanned = scanFrame(complete.first(length));
            QVERIFY2(scanned.status == FrameScanStatus::NeedMoreData,
                     qPrintable(QString::number(length)));
        }
        QCOMPARE(scanFrame(complete).status, FrameScanStatus::FrameReady);
    }

    void leadingNoiseIsDiscardedOneByteAtATime()
    {
        const FrameScanResult scanned = scanFrame(QByteArrayLiteral("\x02\x03$IDN;3A"));

        QCOMPARE(scanned.status, FrameScanStatus::Discard);
        QCOMPARE(scanned.consumed, std::size_t{1});
    }

    void aWrongChecksumTriggersResynchronisation()
    {
        const FrameScanResult scanned = scanFrame(QByteArrayLiteral("$IDN;3B"));

        QCOMPARE(scanned.status, FrameScanStatus::Discard);
        QVERIFY(scanned.diagnostic.contains(QStringLiteral("checksum")));
    }

    void aRepeatedStartByteDropsTheTruncatedFrame()
    {
        // The first frame was cut short, so everything up to the new '$' goes.
        const FrameScanResult scanned = scanFrame(QByteArrayLiteral("$IDN$STAq;B8"));

        QCOMPARE(scanned.status, FrameScanStatus::Discard);
        QCOMPARE(scanned.consumed, std::size_t{4});
    }

    void anUnknownCommandNameStillProducesAFrame()
    {
        const FrameScanResult scanned = scanFrame(buildAsciiFrame(QStringLiteral("XYZ")));

        QCOMPARE(scanned.status, FrameScanStatus::FrameReady);
        QCOMPARE(scanned.frame.opcode, kUnknownCommandOpcode);
        QCOMPARE(scanned.frame.attribute(QString::fromLatin1(kCommandAttribute)).toString(),
                 QStringLiteral("XYZ"));
    }

    // --- Realtime stream ---------------------------------------------------

    void umtsMatchesTheCatalogueCheckValue()
    {
        const QByteArray check = QByteArrayLiteral("123456789");
        QCOMPARE(core::crc::umts(asBytes(check)), quint16{0xFEE8});
    }

    void streamFrameLengthsFollowTheMaskTable_data()
    {
        QTest::addColumn<quint8>("mask");
        QTest::addColumn<int>("records");
        QTest::addColumn<int>("bytes");

        // Section 6.3.
        QTest::newRow("0x00") << quint8{0x00} << 6 << 35;
        QTest::newRow("0x01") << quint8{0x01} << 12 << 65;
        QTest::newRow("0x02") << quint8{0x02} << 12 << 65;
        QTest::newRow("0x03") << quint8{0x03} << 18 << 95;
        QTest::newRow("0x0F") << quint8{0x0F} << 30 << 155;
        QTest::newRow("0x10") << quint8{0x10} << 14 << 75;
        QTest::newRow("0x1F") << quint8{0x1F} << 38 << 195;
    }

    void streamFrameLengthsFollowTheMaskTable()
    {
        QFETCH(quint8, mask);
        QFETCH(int, records);
        QFETCH(int, bytes);

        QCOMPARE(stream::expectedRecordCount(mask), records);
        QCOMPARE(stream::expectedFrameBytes(mask), bytes);

        Sw6StreamSample sample;
        sample.mask = mask;
        sample.actualPose = {1.5, -2.25, 3.0, 0.1, -0.2, 0.3};
        sample.theoreticalPose = sample.actualPose;
        sample.actualLength = {100.1, 100.2, 100.3, 100.4, 100.5, 100.6};
        sample.theoreticalLength = sample.actualLength;
        sample.legSpeed = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0};
        sample.analog = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};

        const RealtimeFrame frame = RealtimeFrame::fromSample(sample);
        QCOMPARE(static_cast<int>(frame.records.size()), records);

        const auto body = frame.encodeBody(EncodeContext{});
        QVERIFY(body.hasValue());

        const Sw6Codec codec;
        const auto wrapped = codec.wrap(kRealtimeStreamOpcode, body.value(), EncodeContext{});
        QVERIFY(wrapped.hasValue());
        QCOMPARE(static_cast<int>(wrapped.value().size()), bytes);
        QCOMPARE(static_cast<quint8>(wrapped.value().front()), quint8{0x81});
        QCOMPARE(static_cast<quint8>(wrapped.value().back()), quint8{0x55});

        const FrameScanResult scanned = scanFrame(wrapped.value());
        QCOMPARE(scanned.status, FrameScanStatus::FrameReady);
        QCOMPARE(scanned.frame.opcode, kRealtimeStreamOpcode);

        const auto decoded = RealtimeFrame::decode(scanned.frame);
        QVERIFY(decoded.hasValue());

        const Sw6StreamSample restored = decoded.value().toSample();
        QCOMPARE(restored.mask, mask);
        for (int axis = 0; axis < kAxisCount; ++axis) {
            QVERIFY(qFuzzyCompare(restored.actualPose[static_cast<std::size_t>(axis)] + 10.0,
                                  sample.actualPose[static_cast<std::size_t>(axis)] + 10.0));
        }
        if ((mask & stream::kMaskAnalog) != 0) {
            QVERIFY(qFuzzyCompare(restored.analog[7] + 10.0, sample.analog[7] + 10.0));
        }
    }

    void aCorruptedStreamFrameIsDiscarded()
    {
        Sw6StreamSample sample;
        const RealtimeFrame frame = RealtimeFrame::fromSample(sample);
        const Sw6Codec codec;
        const QByteArray good =
            codec.wrap(kRealtimeStreamOpcode, frame.encodeBody(EncodeContext{}).value(),
                       EncodeContext{})
                .value();

        QByteArray badCrc = good;
        badCrc[badCrc.size() - 2] = static_cast<char>(badCrc.at(badCrc.size() - 2) ^ 0xFF);
        QCOMPARE(scanFrame(badCrc).status, FrameScanStatus::Discard);

        QByteArray badTail = good;
        badTail[badTail.size() - 1] = 0x00;
        QCOMPARE(scanFrame(badTail).status, FrameScanStatus::Discard);

        QByteArray badLength = good;
        badLength[1] = 0x1D; // not a multiple of five
        QCOMPARE(scanFrame(badLength).status, FrameScanStatus::Discard);
    }

    void aPartialStreamFrameWaitsForTheRest()
    {
        Sw6StreamSample sample;
        const Sw6Codec codec;
        const QByteArray complete =
            codec.wrap(kRealtimeStreamOpcode,
                       RealtimeFrame::fromSample(sample).encodeBody(EncodeContext{}).value(),
                       EncodeContext{})
                .value();

        for (qsizetype length = 1; length < complete.size(); ++length) {
            QCOMPARE(scanFrame(complete.first(length)).status, FrameScanStatus::NeedMoreData);
        }
    }

    void aRepeatedRecordTypeIsRejected()
    {
        Frame frame;
        frame.opcode = kRealtimeStreamOpcode;
        frame.payload = QByteArray::fromHex("0101000000") + QByteArray::fromHex("0102000000");

        QVERIFY(RealtimeFrame::decode(frame).hasError());
    }

    // --- Responder behaviour -----------------------------------------------

    void identityAndStatusAreAnswered()
    {
        Responder device;

        const Sw6Reply identity = device.reply(QByteArrayLiteral("$IDN;3A"));
        QCOMPARE(identity.errorCode, err::kSuccess);
        QCOMPARE(static_cast<int>(identity.values.size()), 3);
        QCOMPARE(identity.values.at(0).label, QStringLiteral("SW6_HEXAPOD_V2"));

        // Fresh state: on target, not referenced yet, no error.
        QCOMPARE(device.exchange(QByteArrayLiteral("$STAq;B8")),
                 buildAsciiFrame(QStringLiteral("STAq"),
                                 QByteArrayLiteral("0x00000000,0x00000002")));
    }

    void anUnknownCommandIsAnsweredWithItsOwnName()
    {
        Responder device;

        // The name is not in the table, so wrap() has to echo it from the
        // request's attributes rather than from the reverse lookup.
        QCOMPARE(device.exchange(buildAsciiFrame(QStringLiteral("XYZ"))),
                 buildAsciiFrame(QStringLiteral("XYZ"), QByteArrayLiteral("0x03000001")));
    }

    void theCommandListStaysInsideTheFrameLimit()
    {
        Responder device;

        const QByteArray frame = device.exchange(buildAsciiFrame(QStringLiteral("HLPq")));
        QVERIFY(!frame.isEmpty());
        QVERIFY(frame.size() <= static_cast<qsizetype>(kMaxAsciiFrameBytes));

        const Sw6Reply reply = device.reply(buildAsciiFrame(QStringLiteral("HLPq")));
        QCOMPARE(reply.errorCode, err::kSuccess);
        QCOMPARE(reply.values.first().asInt(), static_cast<qint64>(reply.values.size() - 1));
        QVERIFY(reply.values.at(1).label == QStringLiteral("MOV"));
    }

    void aSetIsReadBackByItsQuery()
    {
        Responder device;

        QCOMPARE(device.exchange(QByteArrayLiteral("$VEL,Xs,10f;30")),
                 buildAsciiFrame(QStringLiteral("VEL"), QByteArrayLiteral("0x00000000")));

        // Golden reply frame from section 9.2.
        QCOMPARE(device.exchange(buildAsciiFrame(QStringLiteral("VELq"),
                                                 QByteArrayLiteral("Xs"))),
                 QByteArrayLiteral("$VELq,0x00000000,Xs,10f;F5"));
    }

    void aQueryWithoutAxesReturnsAllSix()
    {
        Responder device;

        const Sw6Reply reply = device.reply(QByteArrayLiteral("$SVOq;C8"));
        QCOMPARE(reply.errorCode, err::kSuccess);
        QCOMPARE(static_cast<int>(reply.values.size()), 12);
        for (int axis = 0; axis < kAxisCount; ++axis) {
            QCOMPARE(reply.valueForAxis(axis).value_or(-1.0), 0.0);
        }
    }

    void aValueAboveTheCapIsRejected()
    {
        Responder device;

        // VMXq is 100 mm/s on the translation axes.
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("VEL"),
                                              QByteArrayLiteral("Xs,500f")))
                     .errorCode,
                 err::kBadArgumentValue);
    }

    void motionNeedsServoAndReference()
    {
        Responder device;

        const QByteArray move = buildAsciiFrame(QStringLiteral("MOV"),
                                                QByteArrayLiteral("Xs,1.5f"));
        QCOMPARE(device.reply(move).errorCode, err::kServoOff);

        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("SVO"),
                                              QByteArrayLiteral("Xs,1d")))
                     .errorCode,
                 err::kSuccess);
        QCOMPARE(device.reply(move).errorCode, err::kNotReferenced);

        QCOMPARE(device.reply(QByteArrayLiteral("$FRF;3D")).errorCode, err::kSuccess);
        QCOMPARE(device.reply(move).errorCode, err::kSuccess);

        const Sw6Reply pose = device.reply(buildAsciiFrame(QStringLiteral("POSq")));
        QCOMPARE(pose.valueForAxis(0).value_or(0.0), 1.5);
    }

    void aTargetOutsideTheSoftLimitsIsRejected()
    {
        Responder device;
        device.enableMotion();

        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("MOV"),
                                              QByteArrayLiteral("Xs,500f")))
                     .errorCode,
                 err::kSoftLimitExceeded);

        // The rejected axis must not have moved.
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("POSq"), QByteArrayLiteral("Xs")))
                     .valueForAxis(0)
                     .value_or(-1.0),
                 0.0);
    }

    void aMultiAxisMoveIsAppliedInFull()
    {
        Responder device;
        device.enableMotion();

        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("MOV"),
                                              QByteArrayLiteral("Xs,1.5f,Us,0.1f")))
                     .errorCode,
                 err::kSuccess);

        const Sw6Reply pose = device.reply(buildAsciiFrame(QStringLiteral("POSq")));
        QCOMPARE(pose.valueForAxis(0).value_or(0.0), 1.5);
        QCOMPARE(pose.valueForAxis(3).value_or(0.0), 0.1);

        // A relative move stacks on the target that was accepted.
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("MVR"),
                                              QByteArrayLiteral("Xs,0.5f")))
                     .errorCode,
                 err::kSuccess);
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("POSq")))
                     .valueForAxis(0)
                     .value_or(0.0),
                 2.0);
    }

    void legCommandsAddressLegsByNumber()
    {
        Responder device;

        QCOMPARE(device.exchange(QByteArrayLiteral("$LLC,1d,0.12f;4E")),
                 buildAsciiFrame(QStringLiteral("LLC"), QByteArrayLiteral("0x00000000")));

        const Sw6Reply stored = device.reply(QByteArrayLiteral("$LLCq;AB"));
        QCOMPARE(static_cast<int>(stored.values.size()), 12);
        QCOMPARE(stored.valueForLeg(1).value_or(0.0), 0.12);
        QCOMPARE(stored.valueForLeg(2).value_or(-1.0), 0.0);
    }

    void referenceStatusIsReportedPerLeg()
    {
        Responder device;

        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("FRF"), QByteArrayLiteral("2d")))
                     .errorCode,
                 err::kSuccess);

        const Sw6Reply referenced = device.reply(buildAsciiFrame(QStringLiteral("FRFq")));
        QCOMPARE(referenced.valueForLeg(1).value_or(-1.0), 0.0);
        QCOMPARE(referenced.valueForLeg(2).value_or(-1.0), 1.0);
    }

    void theStreamMaskSurvivesASetAndQuery()
    {
        Responder device;

        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("RSE"), QByteArrayLiteral("3d")))
                     .errorCode,
                 err::kSuccess);
        QCOMPARE(device.state().streamMask(), quint8{0x03});

        const Sw6Reply mask = device.reply(buildAsciiFrame(QStringLiteral("RSEq")));
        QCOMPARE(static_cast<int>(mask.values.size()), 1);
        QCOMPARE(mask.values.first().toToken(), QStringLiteral("0x03"));

        // The stream the device would now push carries the two extra blocks.
        QCOMPARE(stream::expectedFrameBytes(device.state().sample().mask), 95);
    }

    void statusWordReflectsAStopAndItsClear()
    {
        Responder device;
        device.enableMotion();

        QCOMPARE(device.reply(QByteArrayLiteral("$STP;56")).errorCode, err::kSuccess);

        const Sw6Reply stopped = device.reply(QByteArrayLiteral("$STAq;B8"));
        const auto word = static_cast<quint32>(stopped.values.first().asInt());
        QVERIFY((word & status::kEmergencyStop) != 0);
        QVERIFY((word & status::kReferenced) != 0);

        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("MOV"), QByteArrayLiteral("Xs,1f")))
                     .errorCode,
                 err::kEmergencyStopped);

        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("CLR"))).errorCode, err::kSuccess);
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("MOV"), QByteArrayLiteral("Xs,1f")))
                     .errorCode,
                 err::kSuccess);
    }

    void savedConfigurationIsRestored()
    {
        Responder device;

        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("VEL"), QByteArrayLiteral("Xs,5f")))
                     .errorCode,
                 err::kSuccess);
        QCOMPARE(device.reply(QByteArrayLiteral("$SAV;49")).errorCode, err::kSuccess);

        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("VEL"), QByteArrayLiteral("Xs,7f")))
                     .errorCode,
                 err::kSuccess);
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("RST"))).errorCode, err::kSuccess);

        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("VELq"), QByteArrayLiteral("Xs")))
                     .valueForAxis(0)
                     .value_or(0.0),
                 5.0);
    }

    void aMalformedArgumentIsAnsweredNotDropped()
    {
        Responder device;

        // `1.5` has no type suffix, which section 4.7 calls 0x03000009.
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("VEL"), QByteArrayLiteral("Xs,1.5")))
                     .errorCode,
                 err::kMissingArgumentType);

        // A value without an axis in front of it is a bad argument.
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("VEL"), QByteArrayLiteral("10f")))
                     .errorCode,
                 err::kBadArgumentValue);

        // And an axis without its value is a missing one.
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("VEL"), QByteArrayLiteral("Xs")))
                     .errorCode,
                 err::kMissingArgument);
    }

    // --- Coordinate systems (section 5.7) ----------------------------------

    void aToolFrameSurvivesDefineAndQuery()
    {
        Responder device;

        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("KST"),
                                              QByteArrayLiteral("TOOLAs,Xs,10f,Zs,-2.5f")))
                     .errorCode,
                 err::kSuccess);

        const Sw6Reply frame = device.reply(buildAsciiFrame(QStringLiteral("KSTq"),
                                                            QByteArrayLiteral("TOOLAs")));
        QCOMPARE(frame.errorCode, err::kSuccess);
        QCOMPARE(frame.values.at(0).label, QStringLiteral("TOOLA"));
        QCOMPARE(frame.valueForAxis(0).value_or(0.0), 10.0);
        QCOMPARE(frame.valueForAxis(2).value_or(0.0), -2.5);

        // A second define only touches the axes it names.
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("KST"),
                                              QByteArrayLiteral("TOOLAs,Ys,4f")))
                     .errorCode,
                 err::kSuccess);
        const Sw6Reply updated = device.reply(buildAsciiFrame(QStringLiteral("KSTq"),
                                                              QByteArrayLiteral("TOOLAs")));
        QCOMPARE(updated.valueForAxis(0).value_or(0.0), 10.0);
        QCOMPARE(updated.valueForAxis(1).value_or(0.0), 4.0);

        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("KSTq"),
                                              QByteArrayLiteral("MISSINGs")))
                     .errorCode,
                 err::kNoSuchEntry);
    }

    void anEnabledFrameCannotBeDeleted()
    {
        Responder device;

        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("KST"),
                                             QByteArrayLiteral("TOOLAs,Xs,1f")))
                    .succeeded());
        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("KEN"),
                                             QByteArrayLiteral("TOOLs,TOOLAs,1d")))
                    .succeeded());

        const Sw6Reply enabled = device.reply(buildAsciiFrame(QStringLiteral("KENq"),
                                                              QByteArrayLiteral("TOOLs,TOOLAs")));
        QCOMPARE(enabled.values.at(2).asInt(), qint64{1});

        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("KRM"),
                                              QByteArrayLiteral("TOOLs,TOOLAs")))
                     .errorCode,
                 err::kBadArgumentValue);

        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("KEN"),
                                             QByteArrayLiteral("TOOLs,TOOLAs,0d")))
                    .succeeded());
        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("KRM"),
                                             QByteArrayLiteral("TOOLs,TOOLAs")))
                    .succeeded());
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("KSTq"),
                                              QByteArrayLiteral("TOOLAs")))
                     .errorCode,
                 err::kNoSuchEntry);
    }

    void onlyOneFrameOfAKindStaysEnabled()
    {
        Responder device;

        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("KST"),
                                             QByteArrayLiteral("As,Xs,1f")))
                    .succeeded());
        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("KCP"),
                                             QByteArrayLiteral("TOOLs,As,Bs")))
                    .succeeded());

        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("KEN"),
                                             QByteArrayLiteral("TOOLs,As,1d")))
                    .succeeded());
        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("KEN"),
                                             QByteArrayLiteral("TOOLs,Bs,1d")))
                    .succeeded());

        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("KENq"),
                                              QByteArrayLiteral("TOOLs,As")))
                     .values.at(2)
                     .asInt(),
                 qint64{0});

        const Sw6Reply list = device.reply(buildAsciiFrame(QStringLiteral("KLSq"),
                                                           QByteArrayLiteral("TOOLs")));
        QCOMPARE(static_cast<int>(list.values.size()), 3);
        QCOMPARE(list.values.at(0).label, QStringLiteral("TOOL"));
    }

    void frameLinksRejectLoops()
    {
        Responder device;

        for (const char* name : {"As", "Bs"}) {
            QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("KST"),
                                                 QByteArray(name) + QByteArrayLiteral(",Xs,1f")))
                        .succeeded());
        }

        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("KLN"),
                                             QByteArrayLiteral("TOOLs,Bs,TOOLs,As")))
                    .succeeded());

        const Sw6Reply link = device.reply(buildAsciiFrame(QStringLiteral("KLNq"),
                                                           QByteArrayLiteral("TOOLs,Bs")));
        QCOMPARE(link.values.at(2).label, QStringLiteral("TOOL"));
        QCOMPARE(link.values.at(3).label, QStringLiteral("A"));

        // A -> B would close the loop B -> A -> B.
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("KLN"),
                                              QByteArrayLiteral("TOOLs,As,TOOLs,Bs")))
                     .errorCode,
                 err::kBadArgumentValue);

        // Self-links go the same way.
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("KLN"),
                                              QByteArrayLiteral("TOOLs,As,TOOLs,As")))
                     .errorCode,
                 err::kBadArgumentValue);

        // BASE is the root, so it always unlinks.
        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("KLN"),
                                             QByteArrayLiteral("TOOLs,Bs,BASEs,BASEs")))
                    .succeeded());
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("KLNq"),
                                              QByteArrayLiteral("TOOLs,Bs")))
                     .values.at(2)
                     .label,
                 QStringLiteral("BASE"));
    }

    void aFrameCanBeTakenFromTheCurrentPose()
    {
        Responder device;
        device.enableMotion();

        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("MOV"), QByteArrayLiteral("Xs,3f")))
                    .succeeded());
        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("KSF"),
                                             QByteArrayLiteral("WORKs,W1s")))
                    .succeeded());

        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("KSWq"), QByteArrayLiteral("W1s")))
                     .valueForAxis(0)
                     .value_or(0.0),
                 3.0);
    }

    void theRotationCentreTakesTranslationAxesOnly()
    {
        Responder device;

        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("SPI"),
                                             QByteArrayLiteral("Xs,1f,Zs,2f")))
                    .succeeded());

        const Sw6Reply pivot = device.reply(buildAsciiFrame(QStringLiteral("SPIq")));
        QCOMPARE(static_cast<int>(pivot.values.size()), 6);
        QCOMPARE(pivot.valueForAxis(0).value_or(0.0), 1.0);
        QCOMPARE(pivot.valueForAxis(2).value_or(0.0), 2.0);

        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("SPI"), QByteArrayLiteral("Us,1f")))
                     .errorCode,
                 err::kBadArgumentValue);
    }

    // --- Named parameters and read-only registries (section 5.8) -----------

    void namedParametersAreTypeChecked()
    {
        Responder device;

        QCOMPARE(device.exchange(QByteArrayLiteral("$PARq;B3")),
                 buildAsciiFrame(QStringLiteral("PARq"),
                                 QByteArrayLiteral("0x00000000,ADCHs,0d,HPRTs,130f")));

        QVERIFY(device.reply(QByteArrayLiteral("$PAR,ADCHs,0d,HPRTs,130f;B4")).succeeded());
        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("PAR"),
                                             QByteArrayLiteral("HPRTs,95.5f")))
                    .succeeded());
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("PARq"),
                                              QByteArrayLiteral("HPRTs")))
                     .values.at(1)
                     .asDouble(),
                 95.5);

        // `130d` where the registry says float.
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("PAR"),
                                              QByteArrayLiteral("HPRTs,130d")))
                     .errorCode,
                 err::kArgumentTypeMismatch);

        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("PAR"),
                                              QByteArrayLiteral("NOPEs,1d")))
                     .errorCode,
                 err::kNoSuchEntry);
    }

    void kinematicScalarsMatchTheSpecExample()
    {
        Responder device;

        // Section 5.8.1 prints this exchange in full.
        QCOMPARE(device.exchange(QByteArrayLiteral("$KINq,Hs;99")),
                 QByteArrayLiteral("$KINq,0x00000000,Hs,89.705f;BA"));

        const Sw6Reply all = device.reply(QByteArrayLiteral("$KINq;B2"));
        QCOMPARE(static_cast<int>(all.values.size()), 2 * static_cast<int>(kKinematicScalars.size()));

        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("KINq"), QByteArrayLiteral("NOPEs")))
                     .errorCode,
                 err::kNoSuchEntry);
    }

    void geometryRowsAreReportedPerHinge()
    {
        Responder device;

        const Sw6Reply row = device.reply(QByteArrayLiteral("$GEOq,B1s;BD"));
        QCOMPARE(row.errorCode, err::kSuccess);
        QCOMPARE(row.values.at(0).label, QStringLiteral("B1"));
        QCOMPARE(static_cast<int>(row.values.size()), 13);

        // Base hinges sit on a circle, so the first two rows mirror each other.
        const Sw6Reply mirrored =
            device.reply(buildAsciiFrame(QStringLiteral("GEOq"), QByteArrayLiteral("B2s")));
        QVERIFY(qFuzzyCompare(row.valueForAxis(0).value_or(0.0),
                              mirrored.valueForAxis(0).value_or(1.0)));
        QVERIFY(qFuzzyCompare(row.valueForAxis(1).value_or(0.0),
                              -mirrored.valueForAxis(1).value_or(1.0)));

        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("GEOq"), QByteArrayLiteral("B9s")))
                     .errorCode,
                 err::kNoSuchEntry);
    }

    // --- Frame-relative and limit moves (sections 5.2 and 5.5) -------------

    void aToolRelativeMoveTurnsWithItsFrame()
    {
        Responder device;
        device.enableMotion();

        // No tool system is enabled yet, which section 5.2 answers 0x04000006.
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("MRT"),
                                              QByteArrayLiteral("Xs,1f")))
                     .errorCode,
                 err::kNoSuchEntry);

        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("KST"),
                                             QByteArrayLiteral("TOOLAs,Ws,1.5707963f")))
                    .succeeded());
        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("KEN"),
                                             QByteArrayLiteral("TOOLs,TOOLAs,1d")))
                    .succeeded());

        // The tool is turned a quarter turn about Z, so its X becomes the
        // platform's Y.
        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("MRT"), QByteArrayLiteral("Xs,1f")))
                    .succeeded());

        const Sw6Reply pose = device.reply(buildAsciiFrame(QStringLiteral("POSq")));
        QVERIFY(std::abs(pose.valueForAxis(0).value_or(1.0)) < 1e-6);
        QVERIFY(std::abs(pose.valueForAxis(1).value_or(0.0) - 1.0) < 1e-6);
    }

    void limitMovesGoToTheLimitTheyName()
    {
        Responder device;
        device.enableMotion();

        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("FNL"), QByteArrayLiteral("Xs")))
                    .succeeded());
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("POSq"), QByteArrayLiteral("Xs")))
                     .valueForAxis(0)
                     .value_or(0.0),
                 -50.0);

        // A narrowed soft limit gates `$FPL` but not the limit switch `$MPL`
        // drives onto.
        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("PLM"), QByteArrayLiteral("Xs,10f")))
                    .succeeded());
        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("FPL"), QByteArrayLiteral("Xs")))
                    .succeeded());
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("POSq"), QByteArrayLiteral("Xs")))
                     .valueForAxis(0)
                     .value_or(0.0),
                 10.0);

        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("MPL"), QByteArrayLiteral("Xs")))
                    .succeeded());
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("POSq"), QByteArrayLiteral("Xs")))
                     .valueForAxis(0)
                     .value_or(0.0),
                 50.0);
    }

    void theDelayCommandAdvancesTheTimer()
    {
        Responder device;

        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("TIM"), QByteArrayLiteral("0f")))
                    .succeeded());
        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("DEL"), QByteArrayLiteral("500d")))
                    .succeeded());
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("TIMq"))).values.first().asDouble(),
                 0.5);

        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("DEL"), QByteArrayLiteral("-1d")))
                     .errorCode,
                 err::kBadArgumentValue);
    }

    // --- Analog inputs and diagnostics (section 5.9) -----------------------

    void analogChannelsShareOneInputVoltage()
    {
        Responder device;

        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("TACq"))).values.first().asInt(),
                 qint64{kAnalogChannelCount});

        // At the user zero pose the simulated alignment signal is at its peak,
        // and every reading of channel 0 is a view of that one voltage.
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("TAVq"), QByteArrayLiteral("0d")))
                     .values.at(1)
                     .asDouble(),
                 10.0);
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("TADq"), QByteArrayLiteral("0d")))
                     .values.at(1)
                     .asInt(),
                 qint64{32767});
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("TNSq"), QByteArrayLiteral("0d")))
                     .values.at(1)
                     .asDouble(),
                 1.0);
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("TSPq"), QByteArrayLiteral("0d")))
                     .values.at(1)
                     .asDouble(),
                 10.0);

        // Leaving the channel out reports all eight, and the AD block of the
        // realtime stream carries the same voltages.
        QCOMPARE(static_cast<int>(device.reply(buildAsciiFrame(QStringLiteral("TAVq")))
                                      .values.size()),
                 2 * kAnalogChannelCount);
        QCOMPARE(device.state().sample().analog[0], 10.0);

        device.enableMotion();
        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("MOV"), QByteArrayLiteral("Xs,5f")))
                    .succeeded());
        const double moved = device.reply(buildAsciiFrame(QStringLiteral("TAVq"),
                                                          QByteArrayLiteral("0d")))
                                 .values.at(1)
                                 .asDouble();
        QVERIFY(moved > 0.0 && moved < 10.0);
    }

    void anUnknownChannelIsRejected()
    {
        Responder device;

        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("TAVq"), QByteArrayLiteral("9d")))
                     .errorCode,
                 err::kBadArgumentValue);
    }

    void theAveragingSampleCountSurvivesASetAndQuery()
    {
        Responder device;

        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("NAV"), QByteArrayLiteral("8d")))
                    .succeeded());
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("NAVq"))).values.first().asInt(),
                 qint64{8});

        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("NAV"), QByteArrayLiteral("0d")))
                     .errorCode,
                 err::kBadArgumentValue);
    }

    // --- Digital I/O and triggers (section 5.10) ---------------------------

    void digitalLinesAreWrittenAndReadBack()
    {
        Responder device;

        QCOMPARE(device.exchange(QByteArrayLiteral("$DIO,1d,1d;BD")),
                 buildAsciiFrame(QStringLiteral("DIO"), QByteArrayLiteral("0x00000000")));

        const Sw6Reply single = device.reply(buildAsciiFrame(QStringLiteral("DIOq"),
                                                             QByteArrayLiteral("1d")));
        QCOMPARE(single.values.at(0).asInt(), qint64{1});
        QCOMPARE(single.values.at(1).asInt(), qint64{1});

        QCOMPARE(static_cast<int>(device.reply(buildAsciiFrame(QStringLiteral("DIOq")))
                                      .values.size()),
                 2 * kDigitalLineCount);

        // Section 4.5 rule 6: a line carries a logic level, nothing else.
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("DIO"), QByteArrayLiteral("1d,2d")))
                     .errorCode,
                 err::kBadArgumentValue);
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("DIO"), QByteArrayLiteral("9d,1d")))
                     .errorCode,
                 err::kBadArgumentValue);
    }

    void triggerLinesKeepTheirConfiguration()
    {
        Responder device;

        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("TIOq"))).values.first().asInt(),
                 qint64{kTriggerLineCount});

        // Golden frame from section 9.2.
        QVERIFY(device.reply(QByteArrayLiteral("$CTO,1d,3d,1f;8C")).succeeded());
        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("CTO"),
                                             QByteArrayLiteral("1d,4d,-2.5f")))
                    .succeeded());

        const Sw6Reply configured = device.reply(buildAsciiFrame(QStringLiteral("CTOq"),
                                                                 QByteArrayLiteral("1d")));
        QCOMPARE(static_cast<int>(configured.values.size()), 6);
        QCOMPARE(configured.values.at(1).asInt(), qint64{3});
        QCOMPARE(configured.values.at(2).asDouble(), 1.0);
        QCOMPARE(configured.values.at(5).asDouble(), -2.5);

        // A line nobody configured has nothing to report, and the input side
        // is a separate table.
        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("CTIq"))).values.isEmpty());
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("CTO"),
                                              QByteArrayLiteral("1d,9d,1f")))
                     .errorCode,
                 err::kBadArgumentValue);

        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("TRO"), QByteArrayLiteral("2d,1d")))
                    .succeeded());
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("TROq"), QByteArrayLiteral("2d")))
                     .values.at(1)
                     .asInt(),
                 qint64{1});
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("TRIq"), QByteArrayLiteral("2d")))
                     .values.at(1)
                     .asInt(),
                 qint64{0});
    }

    // --- Trajectories (section 5.12) ---------------------------------------

    void aCheckedTrajectoryRunsToItsLastPoint()
    {
        Responder device;
        device.enableMotion();

        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("TGA"),
                                             QByteArrayLiteral("1d,0d,0d,0f,0d,Xs,1f,Ys,2f")))
                    .succeeded());
        // The second point inherits Y from the first one.
        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("TGA"),
                                             QByteArrayLiteral("1d,1d,1d,5f,10d,Xs,3f")))
                    .succeeded());

        const Sw6Reply checked = device.reply(buildAsciiFrame(QStringLiteral("TGF"),
                                                              QByteArrayLiteral("1d")));
        QCOMPARE(checked.errorCode, err::kSuccess);
        QCOMPARE(checked.values.at(0).asInt(), qint64{1});
        QCOMPARE(checked.values.at(2).asInt(), qint64{0});

        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("TGS"), QByteArrayLiteral("1d")))
                    .succeeded());

        const Sw6Reply pose = device.reply(buildAsciiFrame(QStringLiteral("POSq")));
        QCOMPARE(pose.valueForAxis(0).value_or(0.0), 3.0);
        QCOMPARE(pose.valueForAxis(1).value_or(0.0), 2.0);

        const Sw6Reply info = device.reply(buildAsciiFrame(QStringLiteral("TGIq"),
                                                           QByteArrayLiteral("1d")));
        QCOMPARE(info.values.at(1).asInt(), qint64{2});
        QCOMPARE(info.values.at(2).asInt(),
                 static_cast<qint64>(TrajectoryState::Completed));
        QCOMPARE(info.values.at(3).asInt(), qint64{1});
        QCOMPARE(info.values.at(5).asInt(), qint64{kTrajectoryCapacity - 2});
    }

    void theTrajectoryCheckNamesTheOffendingPoint_data()
    {
        QTest::addColumn<QByteArrayList>("points");
        QTest::addColumn<int>("errorPoint");
        QTest::addColumn<int>("reason");

        QTest::newRow("empty") << QByteArrayList{} << 0 << 1;
        QTest::newRow("gap") << QByteArrayList{QByteArrayLiteral("1d,0d,0d,0f,0d,Xs,1f"),
                                               QByteArrayLiteral("1d,2d,0d,0f,0d,Xs,2f")}
                             << 1 << 2;
        QTest::newRow("soft limit")
            << QByteArrayList{QByteArrayLiteral("1d,0d,0d,0f,0d,Xs,500f")} << 0 << 3;
        QTest::newRow("interpolation")
            << QByteArrayList{QByteArrayLiteral("1d,0d,7d,0f,0d,Xs,1f")} << 0 << 7;
        QTest::newRow("speed")
            << QByteArrayList{QByteArrayLiteral("1d,0d,0d,5000f,0d,Xs,1f")} << 0 << 6;
    }

    void theTrajectoryCheckNamesTheOffendingPoint()
    {
        QFETCH(QByteArrayList, points);
        QFETCH(int, errorPoint);
        QFETCH(int, reason);

        Responder device;
        device.enableMotion();

        for (const QByteArray& point : points) {
            QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("TGA"), point)).succeeded());
        }

        const Sw6Reply checked = device.reply(buildAsciiFrame(QStringLiteral("TGF"),
                                                              QByteArrayLiteral("1d")));
        QCOMPARE(checked.values.at(0).asInt(), qint64{0});
        QCOMPARE(checked.values.at(1).asInt(), static_cast<qint64>(errorPoint));
        QCOMPARE(checked.values.at(2).asInt(), static_cast<qint64>(reason));

        // A failed check leaves the trajectory unstartable.
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("TGS"), QByteArrayLiteral("1d")))
                     .errorCode,
                 err::kBadArgumentValue);
    }

    void aTrajectoryOutsideTheLegTravelIsRejected()
    {
        Responder device;
        device.enableMotion();

        // Widen the platform limit so the leg travel is what stops the point.
        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("PLM"), QByteArrayLiteral("Zs,60f")))
                    .succeeded());
        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("TGA"),
                                             QByteArrayLiteral("1d,0d,0d,0f,0d,Zs,45f")))
                    .succeeded());

        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("TGF"), QByteArrayLiteral("1d")))
                     .values.at(2)
                     .asInt(),
                 qint64{4});
    }

    void anUnreferencedPlatformFailsTheTrajectoryCheck()
    {
        Responder device;

        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("TGA"),
                                             QByteArrayLiteral("1d,0d,0d,0f,0d,Xs,1f")))
                    .succeeded());
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("TGF"), QByteArrayLiteral("1d")))
                     .values.at(2)
                     .asInt(),
                 qint64{8});
    }

    void anEndlessTrajectoryPausesAndStops()
    {
        Responder device;
        device.enableMotion();

        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("TLC"), QByteArrayLiteral("1d,0d")))
                    .succeeded());
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("TLCq"), QByteArrayLiteral("1d")))
                     .values.at(1)
                     .asInt(),
                 qint64{0});

        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("TGA"),
                                             QByteArrayLiteral("1d,0d,0d,0f,0d,Xs,1f")))
                    .succeeded());
        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("TGF"), QByteArrayLiteral("1d")))
                    .succeeded());
        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("TGS"), QByteArrayLiteral("1d")))
                    .succeeded());

        const auto state = [&device] {
            return device.reply(buildAsciiFrame(QStringLiteral("TGIq"), QByteArrayLiteral("1d")))
                .values.at(2)
                .asInt();
        };
        QCOMPARE(state(), static_cast<qint64>(TrajectoryState::Running));

        // Section 5.12: a running trajectory may not be cleared or rewritten.
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("TGC"), QByteArrayLiteral("1d")))
                     .errorCode,
                 err::kRejectedWhileMoving);
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("TGA"),
                                              QByteArrayLiteral("1d,1d,0d,0f,0d,Xs,2f")))
                     .errorCode,
                 err::kRejectedWhileMoving);

        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("TST"), QByteArrayLiteral("1d,1d")))
                    .succeeded());
        QCOMPARE(state(), static_cast<qint64>(TrajectoryState::Paused));
        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("TST"), QByteArrayLiteral("1d,2d")))
                    .succeeded());
        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("TST"), QByteArrayLiteral("1d,0d")))
                    .succeeded());
        QCOMPARE(state(), static_cast<qint64>(TrajectoryState::Stopped));

        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("TGC"), QByteArrayLiteral("1d")))
                    .succeeded());
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("TGIq"), QByteArrayLiteral("1d")))
                     .values.at(1)
                     .asInt(),
                 qint64{0});
    }

    void theInterpolationPeriodAndTrajectoryNumbersAreValidated()
    {
        Responder device;

        QCOMPARE(device.exchange(QByteArrayLiteral("$TGT,10d;3F")),
                 buildAsciiFrame(QStringLiteral("TGT"), QByteArrayLiteral("0x00000000")));
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("TGTq"))).values.first().asInt(),
                 qint64{10});
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("TGT"), QByteArrayLiteral("0d")))
                     .errorCode,
                 err::kBadArgumentValue);

        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("TGF"), QByteArrayLiteral("9d")))
                     .errorCode,
                 err::kNoSuchEntry);

        // Leaving the number out addresses every trajectory.
        QCOMPARE(static_cast<int>(device.reply(buildAsciiFrame(QStringLiteral("TGIq")))
                                      .values.size()),
                 6 * kTrajectoryCount);
    }

    // --- Alignment and identification (section 5.15) -----------------------

    void aScanMovesTowardsThePeakWithinItsWindow()
    {
        Responder device;
        device.enableMotion();

        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("MOV"), QByteArrayLiteral("Xs,3f")))
                    .succeeded());
        QVERIFY(device.reply(QByteArrayLiteral("$FLM,Xs,10f;28")).succeeded());

        // The peak is inside the ±5 mm window, so the scan reaches it.
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("POSq"), QByteArrayLiteral("Xs")))
                     .valueForAxis(0)
                     .value_or(1.0),
                 0.0);
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("FSNq"), QByteArrayLiteral("Xs")))
                     .valueForAxis(0)
                     .value_or(1.0),
                 0.0);

        // A window too small to contain the peak stops at its own edge.
        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("MOV"), QByteArrayLiteral("Xs,20f")))
                    .succeeded());
        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("FLS"), QByteArrayLiteral("Xs,4f")))
                    .succeeded());
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("FSNq"), QByteArrayLiteral("Xs")))
                     .valueForAxis(0)
                     .value_or(0.0),
                 18.0);

        // Two-axis scans do the same on both.
        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("MOV"),
                                             QByteArrayLiteral("Xs,2f,Ys,-2f")))
                    .succeeded());
        QVERIFY(device.reply(QByteArrayLiteral("$FSM,Xs,10f,Ys,10f;1A")).succeeded());
        const Sw6Reply pose = device.reply(buildAsciiFrame(QStringLiteral("POSq")));
        QCOMPARE(pose.valueForAxis(0).value_or(1.0), 0.0);
        QCOMPARE(pose.valueForAxis(1).value_or(1.0), 0.0);
    }

    void aScanNeedsAServoedPlatform()
    {
        Responder device;

        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("FLM"), QByteArrayLiteral("Xs,10f")))
                     .errorCode,
                 err::kServoOff);
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("FLM"), QByteArrayLiteral("Qs,10f")))
                     .errorCode,
                 err::kBadArgumentValue);
    }

    void anAlignmentProcessRunsPausesAndReportsItsResult()
    {
        Responder device;
        device.enableMotion();

        QCOMPARE(device.reply(QByteArrayLiteral("$FRS,ALIGNAs;95")).errorCode, err::kNoSuchEntry);

        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("FDR"),
                                             QByteArrayLiteral("ALIGNAs,Xs,10f,Ys,10f")))
                    .succeeded());
        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("MOV"),
                                             QByteArrayLiteral("Xs,2f,Ys,1f")))
                    .succeeded());
        QVERIFY(device.reply(QByteArrayLiteral("$FRS,ALIGNAs;95")).succeeded());

        const Sw6Reply result = device.reply(buildAsciiFrame(QStringLiteral("FRRq"),
                                                             QByteArrayLiteral("ALIGNAs")));
        QCOMPARE(static_cast<int>(result.values.size()), 1 + kProcessResultCount);
        QCOMPARE(result.values.at(1).asDouble(), 0.0);
        QCOMPARE(result.values.at(2).asDouble(), 0.0);
        QCOMPARE(result.values.at(3).asDouble(), 1.0);

        // One result at a time, by id.
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("FRRq"),
                                              QByteArrayLiteral("ALIGNAs,3d")))
                     .values.at(1)
                     .asDouble(),
                 1.0);
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("FRRq"),
                                              QByteArrayLiteral("ALIGNAs,9d")))
                     .errorCode,
                 err::kBadArgumentValue);

        // A running alignment shows up in the status word of section 4.8.
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("FRPq"),
                                              QByteArrayLiteral("ALIGNAs")))
                     .values.at(1)
                     .asInt(),
                 qint64{1});
        QVERIFY((static_cast<quint32>(device.reply(QByteArrayLiteral("$STAq;B8"))
                                          .values.first()
                                          .asInt())
                 & status::kAlignmentRunning)
                != 0);

        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("FRP"),
                                             QByteArrayLiteral("ALIGNAs,1d")))
                    .succeeded());
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("FRPq"),
                                              QByteArrayLiteral("ALIGNAs")))
                     .values.at(1)
                     .asInt(),
                 qint64{2});
        // Pausing twice is not a state this process can be in.
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("FRP"),
                                              QByteArrayLiteral("ALIGNAs,1d")))
                     .errorCode,
                 err::kBadArgumentValue);

        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("FRP"),
                                             QByteArrayLiteral("ALIGNAs,0d")))
                    .succeeded());
        QVERIFY((static_cast<quint32>(device.reply(QByteArrayLiteral("$STAq;B8"))
                                          .values.first()
                                          .asInt())
                 & status::kAlignmentRunning)
                == 0);

        // The result survives the stop.
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("FRRq"),
                                              QByteArrayLiteral("ALIGNAs,3d")))
                     .values.at(1)
                     .asDouble(),
                 1.0);
    }

    void aGradientSearchFollowsItsOffsetCentre()
    {
        Responder device;
        device.enableMotion();

        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("FDG"),
                                             QByteArrayLiteral("Gs,Xs,Ys")))
                    .succeeded());
        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("FGC"),
                                             QByteArrayLiteral("Gs,2f,-1f")))
                    .succeeded());

        const Sw6Reply centre = device.reply(buildAsciiFrame(QStringLiteral("FGCq"),
                                                             QByteArrayLiteral("Gs")));
        QCOMPARE(centre.values.at(1).asDouble(), 2.0);
        QCOMPARE(centre.values.at(2).asDouble(), -1.0);

        // A gradient search has no window, so it lands straight on the centre.
        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("FRS"), QByteArrayLiteral("Gs")))
                    .succeeded());
        const Sw6Reply pose = device.reply(buildAsciiFrame(QStringLiteral("POSq")));
        QCOMPARE(pose.valueForAxis(0).value_or(0.0), 2.0);
        QCOMPARE(pose.valueForAxis(1).value_or(0.0), -1.0);
    }

    void processesCoupleByNameAndDescribeTheirResults()
    {
        Responder device;

        for (const char* definition : {"As,Xs,Ys", "Bs,Ys,Zs"}) {
            QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("FDG"), QByteArray(definition)))
                        .succeeded());
        }

        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("FRC"), QByteArrayLiteral("As,Bs")))
                    .succeeded());
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("FRCq"), QByteArrayLiteral("As")))
                     .values.at(1)
                     .label,
                 QStringLiteral("B"));
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("FRCq"), QByteArrayLiteral("Bs")))
                     .values.at(1)
                     .label,
                 QStringLiteral("NONE"));

        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("FRC"), QByteArrayLiteral("As,As")))
                     .errorCode,
                 err::kBadArgumentValue);
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("FRC"),
                                              QByteArrayLiteral("As,MISSINGs")))
                     .errorCode,
                 err::kNoSuchEntry);

        const Sw6Reply help = device.reply(buildAsciiFrame(QStringLiteral("FRHq")));
        QCOMPARE(static_cast<int>(help.values.size()), kProcessResultCount);
        QCOMPARE(help.values.at(0).label, QStringLiteral("SCAN"));
    }

    void surfaceDetectionStopsAtItsBias()
    {
        Responder device;
        device.enableMotion();

        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("FSF"),
                                             QByteArrayLiteral("Zs,1f,-2f,0.5f")))
                    .succeeded());

        const Sw6Reply parameters = device.reply(buildAsciiFrame(QStringLiteral("FSFq"),
                                                                 QByteArrayLiteral("Zs")));
        QCOMPARE(static_cast<int>(parameters.values.size()), 4);
        QCOMPARE(parameters.values.at(1).asDouble(), 1.0);
        QCOMPARE(parameters.values.at(2).asDouble(), -2.0);
        QCOMPARE(parameters.values.at(3).asDouble(), 0.5);

        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("FSRq"), QByteArrayLiteral("Zs")))
                     .valueForAxis(2)
                     .value_or(0.0),
                 -2.0);
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("POSq"), QByteArrayLiteral("Zs")))
                     .valueForAxis(2)
                     .value_or(0.0),
                 -2.0);

        // The third force is optional (section 5.15), the first two are not.
        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("FSF"),
                                             QByteArrayLiteral("Zs,1f,-1f")))
                    .succeeded());
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("FSF"), QByteArrayLiteral("Zs,1f")))
                     .errorCode,
                 err::kMissingArgument);
    }

    void identificationSweepsAnswerForTheModeTheyRan()
    {
        Responder device;
        device.enableMotion();

        QVERIFY(device.reply(QByteArrayLiteral("$WFR,Xs,1d,0.1f,10f,1000f;6D")).succeeded());

        const Sw6Reply sweep = device.reply(buildAsciiFrame(QStringLiteral("WFRq"),
                                                            QByteArrayLiteral("Xs,1d")));
        QCOMPARE(static_cast<int>(sweep.values.size()), 5);
        QCOMPARE(sweep.values.at(2).asDouble(), 0.1);
        QCOMPARE(sweep.values.at(4).asDouble(), 1000.0);

        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("WFRq"), QByteArrayLiteral("Xs,2d")))
                     .errorCode,
                 err::kNoSuchEntry);
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("WFR"),
                                              QByteArrayLiteral("Xs,1d,0.1f,100f,10f")))
                     .errorCode,
                 err::kBadArgumentValue);

        // Section 4.5 rule 2: the query of an excitation reports what was set.
        QVERIFY(device.reply(QByteArrayLiteral("$IMP,Xs,0.1f;5D")).succeeded());
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("IMPq"), QByteArrayLiteral("Xs")))
                     .valueForAxis(0)
                     .value_or(0.0),
                 0.1);
        QVERIFY(device.reply(QByteArrayLiteral("$STE,Xs,0.1f;63")).succeeded());
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("STEq"), QByteArrayLiteral("Xs")))
                     .valueForAxis(0)
                     .value_or(0.0),
                 0.1);
        QVERIFY(device.reply(QByteArrayLiteral("$DPO,Xs;39")).succeeded());
    }

    void identificationNeedsAServoedPlatform()
    {
        Responder device;

        QCOMPARE(device.reply(QByteArrayLiteral("$IMP,Xs,0.1f;5D")).errorCode, err::kServoOff);
        QCOMPARE(device.reply(QByteArrayLiteral("$DPO,Xs;39")).errorCode, err::kServoOff);
        QCOMPARE(device.reply(QByteArrayLiteral("$WFR,Xs,1d,0.1f,10f,1000f;6D")).errorCode,
                 err::kServoOff);
    }

    void inputCalculationsFeedTheComputedValue()
    {
        Responder device;

        // Input 1 reads analog channel 0, which is at full scale at the peak.
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("TCIq"), QByteArrayLiteral("1d")))
                     .values.at(1)
                     .asDouble(),
                 10.0);

        QVERIFY(device.reply(buildAsciiFrame(QStringLiteral("SIC"),
                                             QByteArrayLiteral("1d,1d,2.5f")))
                    .succeeded());
        const Sw6Reply calculation = device.reply(buildAsciiFrame(QStringLiteral("SICq"),
                                                                  QByteArrayLiteral("1d")));
        QCOMPARE(static_cast<int>(calculation.values.size()), 3);
        QCOMPARE(calculation.values.at(1).asInt(), qint64{1});
        QCOMPARE(calculation.values.at(2).asDouble(), 2.5);

        // Type 1 normalises the reading.
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("TCIq"), QByteArrayLiteral("1d")))
                     .values.at(1)
                     .asDouble(),
                 1.0);

        QCOMPARE(static_cast<int>(device.reply(buildAsciiFrame(QStringLiteral("TCIq")))
                                      .values.size()),
                 2 * kAnalogChannelCount);
        QCOMPARE(device.reply(buildAsciiFrame(QStringLiteral("SIC"), QByteArrayLiteral("9d,1d")))
                     .errorCode,
                 err::kBadArgumentValue);
    }

    // --- Realtime stream source --------------------------------------------

    void theStreamSourceStopsAfterPrepareShutdown()
    {
        Responder device;
        Sw6StreamSource source(device.sharedState(), 20);

        QCOMPARE(source.intervalMs(), 20);
        QCOMPARE(static_cast<int>(source.poll(0).size()), 1);

        QVERIFY(device.reply(QByteArrayLiteral("$RTO;54")).succeeded());
        QVERIFY(source.poll(0).empty());
    }

    // --- Initiator side ----------------------------------------------------

    void aRequestBuiltByTheMasterMatchesTheSpecVector()
    {
        const Sw6Codec codec;

        const AxisCommand command =
            AxisCommand::make(QStringLiteral("VEL"),
                              {Sw6Element{0, Sw6Values{Sw6Value::ofFloat(10.0)}}});
        const auto body = command.encodeBody(EncodeContext{});
        QVERIFY(body.hasValue());

        const auto opcode = command.dynamicOpcode();
        QVERIFY(opcode.has_value());

        const auto wrapped = codec.wrap(*opcode, body.value(), EncodeContext{});
        QVERIFY(wrapped.hasValue());
        QCOMPARE(wrapped.value(), QByteArrayLiteral("$VEL,Xs,10f;30"));
    }

    void repliesCorrelateByCommandName()
    {
        const Sw6Codec codec;

        EncodeContext context;
        const QString token = codec.prepareRequest(context);
        QVERIFY(!token.isEmpty());

        const auto request = SystemCommand::make(QStringLiteral("STAq"));
        const auto body = request.encodeBody(context);
        QVERIFY(codec.wrap(*request.dynamicOpcode(), body.value(), context).hasValue());

        const FrameScanResult reply =
            scanFrameWith(codec, QByteArrayLiteral("$STAq,0x00000000,0x00000025;67"));
        QCOMPARE(reply.status, FrameScanStatus::FrameReady);
        QCOMPARE(codec.correlationKey(reply.frame), token);

        // A reply to something never sent must not match an outstanding entry.
        const FrameScanResult other =
            scanFrameWith(codec, QByteArrayLiteral("$VELq,0x00000000,Xs,10f;F5"));
        QCOMPARE(other.status, FrameScanStatus::FrameReady);
        QCOMPARE(codec.correlationKey(other.frame), QStringLiteral("VELq"));

        // The stream never completes a pending ASCII request either.
        Frame streamFrame;
        streamFrame.opcode = kRealtimeStreamOpcode;
        QCOMPARE(codec.correlationKey(streamFrame), QString::fromLatin1(kStreamCorrelationKey));
    }

private:
    static FrameScanResult scanFrameWith(const Sw6Codec& codec, const QByteArray& bytes)
    {
        return codec.scan(asBytes(bytes), Direction::Inbound);
    }
};

QTEST_MAIN(Sw6Test)
#include "tst_sw6.moc"
