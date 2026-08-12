#include <core/ByteBuffer.h>
#include <core/ConfigSchema.h>
#include <core/Crc.h>
#include <core/Endian.h>
#include <core/EventBus.h>
#include <core/HexUtils.h>
#include <core/LogSinks.h>
#include <core/Logger.h>
#include <core/Registry.h>
#include <core/Result.h>
#include <core/TypeId.h>

#include <QObject>
#include <QTest>

#include <array>

using namespace hwsim::core;

namespace {

struct AlphaEvent {
    int value{0};
};

struct BetaEvent {
    QString text;
};

class Animal {
public:
    virtual ~Animal() = default;
    [[nodiscard]] virtual QString speak() const = 0;
};

class Dog final : public Animal {
public:
    [[nodiscard]] QString speak() const override { return QStringLiteral("woof"); }
};

class Cat final : public Animal {
public:
    [[nodiscard]] QString speak() const override { return QStringLiteral("meow"); }
};

} // namespace

class CoreTest : public QObject {
    Q_OBJECT

private slots:
    // --- Result ------------------------------------------------------------

    void resultCarriesValue()
    {
        const Result<int> result = 42;
        QVERIFY(result.hasValue());
        QVERIFY(static_cast<bool>(result));
        QCOMPARE(result.value(), 42);
    }

    void resultCarriesError()
    {
        const Result<int> result = makeError(ErrorCode::OutOfRange, QStringLiteral("bad address"));
        QVERIFY(result.hasError());
        QVERIFY(!static_cast<bool>(result));
        QCOMPARE(result.error().code, ErrorCode::OutOfRange);
        QCOMPARE(result.valueOr(-1), -1);
    }

    void resultVoidDefaultsToSuccess()
    {
        const Result<void> ok;
        QVERIFY(ok.hasValue());

        const Result<void> failed = makeError(ErrorCode::Timeout, QStringLiteral("no reply"));
        QVERIFY(failed.hasError());
    }

    void resultMapPropagatesError()
    {
        const Result<int> failed = makeError(ErrorCode::Internal, QStringLiteral("x"));
        const Result<QString> mapped = failed.map([](int v) { return QString::number(v); });
        QVERIFY(mapped.hasError());
        QCOMPARE(mapped.error().code, ErrorCode::Internal);

        const Result<int> good = 7;
        QCOMPARE(good.map([](int v) { return v * 2; }).value(), 14);
    }

    // --- TypeId ------------------------------------------------------------

    void typeIdIsStableAndDistinct()
    {
        QCOMPARE(TypeId::of<AlphaEvent>(), TypeId::of<AlphaEvent>());
        QVERIFY(TypeId::of<AlphaEvent>() != TypeId::of<BetaEvent>());
        QVERIFY(TypeId::of<AlphaEvent>().isValid());
        QVERIFY(!TypeId{}.isValid());
    }

    void typeIdIgnoresCvAndReferences()
    {
        QCOMPARE(TypeId::of<AlphaEvent>(), TypeId::of<const AlphaEvent&>());
    }

    void typeIdExposesReadableName()
    {
        const auto name = QString::fromUtf8(TypeId::of<AlphaEvent>().name().data(),
                                            static_cast<qsizetype>(TypeId::of<AlphaEvent>().name().size()));
        QVERIFY(name.contains(QStringLiteral("AlphaEvent")));
    }

    // --- ByteBuffer --------------------------------------------------------

    void byteBufferAppendsAndConsumes()
    {
        ByteBuffer buffer;
        buffer.append(QByteArray::fromHex("0102030405"));
        QCOMPARE(buffer.size(), std::size_t{5});

        QCOMPARE(buffer.peek(2).size(), std::size_t{2});
        QCOMPARE(std::to_integer<int>(buffer.peek(2)[0]), 0x01);

        buffer.consume(2);
        QCOMPARE(buffer.size(), std::size_t{3});
        QCOMPARE(std::to_integer<int>(buffer.readable()[0]), 0x03);

        QCOMPARE(buffer.take(3), QByteArray::fromHex("030405"));
        QVERIFY(buffer.isEmpty());
    }

    void byteBufferClampsOverConsume()
    {
        ByteBuffer buffer;
        buffer.append(QByteArray::fromHex("AABB"));
        buffer.consume(99);
        QVERIFY(buffer.isEmpty());
        QVERIFY(buffer.peek(4).empty());
    }

    void byteBufferSurvivesInterleavedAppendAndConsume()
    {
        ByteBuffer buffer;
        buffer.setCompactThreshold(4);
        for (int i = 0; i < 100; ++i) {
            buffer.append(QByteArray(8, static_cast<char>(i)));
            buffer.consume(8);
        }
        QVERIFY(buffer.isEmpty());
    }

    // --- Endian ------------------------------------------------------------

    void endianRoundTrips()
    {
        std::array<std::byte, 4> storage{};
        endian::writeBig<quint32>(storage, 0x11223344u);
        QCOMPARE(std::to_integer<int>(storage[0]), 0x11);
        QCOMPARE(endian::readBig<quint32>(storage), 0x11223344u);

        endian::writeLittle<quint32>(storage, 0x11223344u);
        QCOMPARE(std::to_integer<int>(storage[0]), 0x44);
        QCOMPARE(endian::readLittle<quint32>(storage), 0x11223344u);
    }

    void endianByteswap()
    {
        QCOMPARE(endian::byteswap<quint16>(0x1234u), quint16{0x3412u});
        QCOMPARE(endian::byteswap<quint32>(0x11223344u), 0x44332211u);
    }

    void endianReadsShortSpansAsZeroExtended()
    {
        const std::array<std::byte, 1> single{std::byte{0xAB}};
        QCOMPARE(endian::readBig<quint32>(single), 0xABu);
    }

    // --- CRC ---------------------------------------------------------------

    void crcModbusMatchesKnownVector()
    {
        // Standard Modbus response 01 04 02 FF FF computes to 0x80B8, which goes
        // onto the wire low byte first as B8 80.
        const QByteArray frame = QByteArray::fromHex("010402FFFF");
        QCOMPARE(crc::modbus(hex::asBytes(frame)), quint16{0x80B8});
    }

    void crcIsUsableAtCompileTime()
    {
        static constexpr std::array<std::byte, 3> data{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
        static constexpr auto value = crc::modbus(data);
        static_assert(value != 0, "compile time CRC evaluation must work");
        QVERIFY(value != 0);
    }

    void crcAlgorithmLookup()
    {
        const QByteArray data = QByteArray::fromHex("0102030405");
        QCOMPARE(crc::compute(crc::Algorithm::Modbus16, hex::asBytes(data)),
                 static_cast<quint32>(crc::modbus(hex::asBytes(data))));
        QCOMPARE(crc::widthBytes(crc::Algorithm::Crc32), std::size_t{4});
        QCOMPARE(crc::widthBytes(crc::Algorithm::None), std::size_t{0});
    }

    // --- Hex ---------------------------------------------------------------

    void hexRoundTrip()
    {
        const QByteArray original = QByteArray::fromHex("00FF10AB");
        const QString text = hex::toHex(original);
        QCOMPARE(text, QStringLiteral("00 FF 10 AB"));

        bool ok = false;
        QCOMPARE(hex::fromHex(text, &ok), original);
        QVERIFY(ok);
    }

    void hexAcceptsPrefixedAndSeparatedInput()
    {
        bool ok = false;
        QCOMPARE(hex::fromHex(QStringLiteral("0x01,0x02 03-04"), &ok), QByteArray::fromHex("01020304"));
        QVERIFY(ok);
    }

    void hexRejectsGarbage()
    {
        bool ok = true;
        QVERIFY(hex::fromHex(QStringLiteral("zz"), &ok).isEmpty());
        QVERIFY(!ok);
    }

    // --- EventBus ----------------------------------------------------------

    void eventBusDeliversByType()
    {
        EventBus bus;
        int alphaSum = 0;
        QString betaText;

        bus.subscribe<AlphaEvent>([&alphaSum](const AlphaEvent& e) { alphaSum += e.value; });
        bus.subscribe<BetaEvent>([&betaText](const BetaEvent& e) { betaText = e.text; });

        bus.publish(AlphaEvent{5});
        bus.publish(AlphaEvent{7});
        bus.publish(BetaEvent{QStringLiteral("hello")});

        QCOMPARE(alphaSum, 12);
        QCOMPARE(betaText, QStringLiteral("hello"));
    }

    void eventBusUnsubscribeStopsDelivery()
    {
        EventBus bus;
        int count = 0;
        const auto id = bus.subscribe<AlphaEvent>([&count](const AlphaEvent&) { ++count; });

        bus.publish(AlphaEvent{1});
        bus.unsubscribe(id);
        bus.publish(AlphaEvent{1});

        QCOMPARE(count, 1);
        QCOMPARE(bus.subscriberCount<AlphaEvent>(), std::size_t{0});
    }

    void eventBusScopedSubscriptionUnsubscribes()
    {
        EventBus bus;
        int count = 0;
        {
            ScopedSubscription guard(&bus,
                                     bus.subscribe<AlphaEvent>([&count](const AlphaEvent&) { ++count; }));
            bus.publish(AlphaEvent{1});
        }
        bus.publish(AlphaEvent{1});
        QCOMPARE(count, 1);
    }

    // --- Registry ----------------------------------------------------------

    void registryCreatesRegisteredTypes()
    {
        Registry<QString, Animal> registry;
        QVERIFY(registry.addType<Dog>(QStringLiteral("dog"), QStringLiteral("Dog")));
        QVERIFY(registry.addType<Cat>(QStringLiteral("cat"), QStringLiteral("Cat")));
        QCOMPARE(registry.size(), std::size_t{2});

        const auto dog = registry.create(QStringLiteral("dog"));
        QVERIFY(dog.hasValue());
        QCOMPARE(dog.value()->speak(), QStringLiteral("woof"));
    }

    void registryRejectsDuplicatesAndReportsMissing()
    {
        Registry<QString, Animal> registry;
        QVERIFY(registry.addType<Dog>(QStringLiteral("dog"), QStringLiteral("Dog")));
        QVERIFY(!registry.addType<Cat>(QStringLiteral("dog"), QStringLiteral("Cat")));

        const auto missing = registry.create(QStringLiteral("fish"));
        QVERIFY(missing.hasError());
        QCOMPARE(missing.error().code, ErrorCode::NotFound);
    }

    // --- ConfigSchema ------------------------------------------------------

    void configSchemaProducesDefaults()
    {
        ConfigSchema schema(QStringLiteral("TCP"));
        schema.add(ConfigField::host(QStringLiteral("host"), QStringLiteral("Host")));
        schema.add(ConfigField::port(QStringLiteral("port"), QStringLiteral("Port"), 502));

        const QVariantMap defaults = schema.defaults();
        QCOMPARE(defaults.value(QStringLiteral("host")).toString(), QStringLiteral("127.0.0.1"));
        QCOMPARE(defaults.value(QStringLiteral("port")).toInt(), 502);
    }

    void configSchemaValidatesRange()
    {
        ConfigSchema schema;
        schema.add(ConfigField::port(QStringLiteral("port"), QStringLiteral("Port")));

        QVERIFY(schema.validate({{QStringLiteral("port"), 502}}).hasValue());

        const auto tooLarge = schema.validate({{QStringLiteral("port"), 70000}});
        QVERIFY(tooLarge.hasError());
        QCOMPARE(tooLarge.error().code, ErrorCode::ConfigInvalid);
    }

    void configSchemaValidatesEnum()
    {
        ConfigSchema schema;
        schema.add(ConfigField::enumeration(QStringLiteral("mode"), QStringLiteral("Mode"),
                                            {QStringLiteral("server"), QStringLiteral("client")}));

        QVERIFY(schema.validate({{QStringLiteral("mode"), QStringLiteral("client")}}).hasValue());
        QVERIFY(schema.validate({{QStringLiteral("mode"), QStringLiteral("peer")}}).hasError());
    }

    void configSchemaSkipsHiddenFields()
    {
        ConfigSchema schema;
        schema.add(ConfigField::enumeration(QStringLiteral("mode"), QStringLiteral("Mode"),
                                            {QStringLiteral("server"), QStringLiteral("client")}));
        ConfigField remote = ConfigField::host(QStringLiteral("remoteHost"), QStringLiteral("Remote"), {});
        remote.shownWhen(QStringLiteral("mode==client"));
        schema.add(remote);

        // remoteHost is required but hidden while mode is server
        QVERIFY(schema.validate({{QStringLiteral("mode"), QStringLiteral("server")}}).hasValue());
        QVERIFY(schema.validate({{QStringLiteral("mode"), QStringLiteral("client")}}).hasError());
    }

    void configSchemaClampsOnNormalise()
    {
        ConfigSchema schema;
        schema.add(ConfigField::port(QStringLiteral("port"), QStringLiteral("Port")));

        const QVariantMap normalised = schema.normalise({{QStringLiteral("port"), 999999}});
        QCOMPARE(normalised.value(QStringLiteral("port")).toInt(), 65535);
    }

    // --- Logger ------------------------------------------------------------

    void loggerDeliversToSink()
    {
        auto& logger = Logger::instance();
        logger.setAsynchronous(false);
        logger.setLevel(LogLevel::Trace);

        auto sink = std::make_shared<RingBufferLogSink>(16);
        const auto id = logger.addSink(sink);

        HWSIM_LOG_INFO("test.category") << "value=" << 42;
        logger.flush();

        const auto records = sink->snapshot();
        QCOMPARE(records.size(), std::size_t{1});
        QCOMPARE(records.front().category, QStringLiteral("test.category"));
        QCOMPARE(records.front().message, QStringLiteral("value=42"));
        QCOMPARE(records.front().level, LogLevel::Info);

        logger.removeSink(id);
    }

    void loggerRespectsLevelThreshold()
    {
        auto& logger = Logger::instance();
        logger.setAsynchronous(false);
        logger.setLevel(LogLevel::Warning);

        auto sink = std::make_shared<RingBufferLogSink>(16);
        const auto id = logger.addSink(sink);

        HWSIM_LOG_DEBUG("test.category") << "suppressed";
        HWSIM_LOG_ERROR("test.category") << "kept";
        logger.flush();

        QCOMPARE(sink->size(), std::size_t{1});

        logger.removeSink(id);
        logger.setLevel(LogLevel::Info);
    }

    void loggerCategoryOverrideWins()
    {
        auto& logger = Logger::instance();
        logger.setAsynchronous(false);
        logger.setLevel(LogLevel::Error);
        logger.setCategoryLevel(QStringLiteral("chatty"), LogLevel::Trace);

        auto sink = std::make_shared<RingBufferLogSink>(16);
        const auto id = logger.addSink(sink);

        HWSIM_LOG_DEBUG("chatty") << "kept";
        HWSIM_LOG_DEBUG("quiet") << "suppressed";
        logger.flush();

        QCOMPARE(sink->size(), std::size_t{1});

        logger.removeSink(id);
        logger.clearAllCategoryLevels();
        logger.setLevel(LogLevel::Info);
    }
};

QTEST_MAIN(CoreTest)
#include "tst_core.moc"
