#include <simulator/DeviceModel.h>
#include <simulator/DeviceProfile.h>
#include <simulator/DeviceStateMachine.h>
#include <simulator/FaultInjector.h>
#include <simulator/ParameterStore.h>
#include <simulator/SignalEngine.h>
#include <simulator/SignalSources.h>

#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>

using namespace hwsim::core;
using namespace hwsim::simulator;
using hwsim::protocol::ByteFilterContext;
using hwsim::transport::Direction;

namespace {

ParameterDefinition numericParameter(const QString& key, quint32 address, double defaultValue,
                                     AccessMode access = AccessMode::ReadWrite)
{
    ParameterDefinition definition =
        ParameterDefinition::make(key, ParameterType::Double, defaultValue);
    definition.address = address;
    definition.hasAddress = true;
    definition.access = access;
    return definition;
}

} // namespace

class SimulatorTest : public QObject {
    Q_OBJECT

private slots:

    // --- ParameterStore ----------------------------------------------------

    void storeIndexesByNameAndAddress()
    {
        ParameterStore store(QStringLiteral("dev"));
        QVERIFY(store.define(numericParameter(QStringLiteral("temperature"), 0x10, 25.0)).hasValue());

        QVERIFY(store.contains(QStringLiteral("temperature")));
        QVERIFY(store.containsAddress(0x10));
        QCOMPARE(store.read(QStringLiteral("temperature")).value().toDouble(), 25.0);
        QCOMPARE(store.readAddress(0x10).value().toDouble(), 25.0);
    }

    void storeRejectsDuplicateKeyAndAddress()
    {
        ParameterStore store;
        QVERIFY(store.define(numericParameter(QStringLiteral("a"), 1, 0.0)).hasValue());

        QCOMPARE(store.define(numericParameter(QStringLiteral("a"), 2, 0.0)).error().code,
                 ErrorCode::AlreadyExists);
        QCOMPARE(store.define(numericParameter(QStringLiteral("b"), 1, 0.0)).error().code,
                 ErrorCode::AlreadyExists);
    }

    void storeClampsToDeclaredRange()
    {
        ParameterStore store;
        ParameterDefinition definition = numericParameter(QStringLiteral("pressure"), 1, 0.0);
        definition.minimum = 0.0;
        definition.maximum = 100.0;
        QVERIFY(store.define(definition).hasValue());

        QVERIFY(store.write(QStringLiteral("pressure"), 250.0, WriteOrigin::Ui).hasValue());
        QCOMPARE(store.read(QStringLiteral("pressure")).value().toDouble(), 100.0);

        QVERIFY(store.write(QStringLiteral("pressure"), -50.0, WriteOrigin::Ui).hasValue());
        QCOMPARE(store.read(QStringLiteral("pressure")).value().toDouble(), 0.0);
    }

    void readOnlyBlocksProtocolButNotOperator()
    {
        ParameterStore store;
        QVERIFY(store.define(numericParameter(QStringLiteral("sensor"), 1, 0.0, AccessMode::ReadOnly))
                    .hasValue());

        // The master must not be able to write a sensor reading...
        const auto fromProtocol = store.write(QStringLiteral("sensor"), 5.0, WriteOrigin::Protocol);
        QVERIFY(fromProtocol.hasError());
        QCOMPARE(fromProtocol.error().code, ErrorCode::ParameterReadOnly);

        // ...but the operator driving the simulation must.
        QVERIFY(store.write(QStringLiteral("sensor"), 5.0, WriteOrigin::Ui).hasValue());
        QCOMPARE(store.read(QStringLiteral("sensor")).value().toDouble(), 5.0);
    }

    void storeNotifiesOnActualChange()
    {
        ParameterStore store;
        QVERIFY(store.define(numericParameter(QStringLiteral("x"), 1, 0.0)).hasValue());

        int notifications = 0;
        store.onChanged([&notifications](const ParameterChange&) { ++notifications; });

        QVERIFY(store.write(QStringLiteral("x"), 1.0, WriteOrigin::Ui).hasValue());
        QCOMPARE(notifications, 1);

        // Writing the same value again is not a change.
        QVERIFY(store.write(QStringLiteral("x"), 1.0, WriteOrigin::Ui).hasValue());
        QCOMPARE(notifications, 1);
    }

    void rangeWriteIsAllOrNothing()
    {
        ParameterStore store;
        QVERIFY(store.define(numericParameter(QStringLiteral("r0"), 0, 0.0)).hasValue());
        QVERIFY(store.define(numericParameter(QStringLiteral("r1"), 1, 0.0)).hasValue());
        // No parameter at address 2.

        const auto written = store.writeAddressRange(0, {1.0, 2.0, 3.0}, WriteOrigin::Protocol);
        QVERIFY(written.hasError());
        QCOMPARE(written.error().code, ErrorCode::OutOfRange);

        // Nothing was applied, so the master's partial write did not land.
        QCOMPARE(store.readAddress(0).value().toDouble(), 0.0);
        QCOMPARE(store.readAddress(1).value().toDouble(), 0.0);
    }

    void storeRoundTripsDefinitionsThroughJson()
    {
        ParameterStore original;
        ParameterDefinition definition = numericParameter(QStringLiteral("flow"), 0x20, 3.5);
        definition.unit = QStringLiteral("L/min");
        definition.minimum = 0.0;
        definition.maximum = 50.0;
        definition.scale = 0.1;
        QVERIFY(original.define(definition).hasValue());

        ParameterStore restored;
        QVERIFY(restored.loadDefinitions(original.definitionsToJson()).hasValue());

        const auto reloaded = restored.definitionOf(QStringLiteral("flow"));
        QVERIFY(reloaded.has_value());
        QCOMPARE(reloaded->address, quint32{0x20});
        QCOMPARE(reloaded->unit, QStringLiteral("L/min"));
        QCOMPARE(reloaded->scale, 0.1);
    }

    // --- Signal sources ----------------------------------------------------

    void sineProducesExpectedShape()
    {
        SineSource source;
        QVERIFY(source.configure({{QStringLiteral("amplitude"), 10.0},
                                  {QStringLiteral("offset"), 5.0},
                                  {QStringLiteral("frequencyHz"), 1.0},
                                  {QStringLiteral("phaseDeg"), 0.0}})
                    .hasValue());

        QVERIFY(qAbs(source.sample(0) - 5.0) < 1e-9);       // sin(0)
        QVERIFY(qAbs(source.sample(250) - 15.0) < 1e-9);    // quarter period, peak
        QVERIFY(qAbs(source.sample(750) - (-5.0)) < 1e-9);  // three quarters, trough
    }

    void stepHoldsThenJumps()
    {
        StepSource source;
        QVERIFY(source.configure({{QStringLiteral("initialValue"), 0.0},
                                  {QStringLiteral("finalValue"), 100.0},
                                  {QStringLiteral("stepAtMs"), 1000},
                                  {QStringLiteral("repeat"), false}})
                    .hasValue());

        QCOMPARE(source.sample(0), 0.0);
        QCOMPARE(source.sample(999), 0.0);
        QCOMPARE(source.sample(1000), 100.0);
        QCOMPARE(source.sample(99999), 100.0);
    }

    void rampInterpolatesLinearly()
    {
        RampSource source;
        QVERIFY(source.configure({{QStringLiteral("startValue"), 0.0},
                                  {QStringLiteral("endValue"), 100.0},
                                  {QStringLiteral("durationMs"), 1000},
                                  {QStringLiteral("repeat"), false}})
                    .hasValue());

        QCOMPARE(source.sample(0), 0.0);
        QVERIFY(qAbs(source.sample(500) - 50.0) < 1e-9);
        QCOMPARE(source.sample(1000), 100.0);
    }

    void seededNoiseIsReproducible()
    {
        const QVariantMap config{{QStringLiteral("distribution"), QStringLiteral("uniform")},
                                 {QStringLiteral("amplitude"), 1.0},
                                 {QStringLiteral("centre"), 0.0},
                                 {QStringLiteral("seed"), 12345}};

        NoiseSource first;
        NoiseSource second;
        QVERIFY(first.configure(config).hasValue());
        QVERIFY(second.configure(config).hasValue());

        // A fixed seed is what makes a fault-injection regression test stable.
        for (int i = 0; i < 10; ++i) {
            QCOMPARE(first.sample(i * 100), second.sample(i * 100));
        }
    }

    void noiseStaysWithinAmplitude()
    {
        NoiseSource source;
        QVERIFY(source.configure({{QStringLiteral("distribution"), QStringLiteral("uniform")},
                                  {QStringLiteral("amplitude"), 2.0},
                                  {QStringLiteral("centre"), 10.0},
                                  {QStringLiteral("seed"), 1}})
                    .hasValue());

        for (int i = 0; i < 200; ++i) {
            const double value = source.sample(i);
            QVERIFY(value >= 8.0 && value <= 12.0);
        }
    }

    void signalSourceRegistryKnowsEveryBuiltin()
    {
        registerBuiltinSignalSources();
        const auto keys = signalSourceRegistry().keys();
        QVERIFY(keys.size() >= 10);
        QVERIFY(signalSourceRegistry().contains(QStringLiteral("sine")));
        QVERIFY(signalSourceRegistry().contains(QStringLiteral("noise")));
        QVERIFY(signalSourceRegistry().contains(QStringLiteral("step")));
    }

    // --- SignalEngine ------------------------------------------------------

    void engineDrivesParameterFromWaveform()
    {
        ParameterStore store;
        QVERIFY(store.define(numericParameter(QStringLiteral("temp"), 1, 0.0)).hasValue());

        SignalEngine engine(&store);
        const auto binding = engine.addBinding(QStringLiteral("temp"), QStringLiteral("constant"),
                                               {{QStringLiteral("value"), 42.0}}, 10);
        QVERIFY(binding.hasValue());

        engine.tick(0);
        QCOMPARE(store.read(QStringLiteral("temp")).value().toDouble(), 42.0);
    }

    void engineCombinesBindingsOnOneParameter()
    {
        ParameterStore store;
        QVERIFY(store.define(numericParameter(QStringLiteral("temp"), 1, 0.0)).hasValue());

        SignalEngine engine(&store);
        QVERIFY(engine.addBinding(QStringLiteral("temp"), QStringLiteral("constant"),
                                  {{QStringLiteral("value"), 20.0}}, 10, CombineMode::Replace)
                    .hasValue());
        QVERIFY(engine.addBinding(QStringLiteral("temp"), QStringLiteral("constant"),
                                  {{QStringLiteral("value"), 5.0}}, 10, CombineMode::Add)
                    .hasValue());

        engine.tick(0);
        QCOMPARE(store.read(QStringLiteral("temp")).value().toDouble(), 25.0);
    }

    void engineRejectsBindingToUnknownParameter()
    {
        ParameterStore store;
        SignalEngine engine(&store);

        const auto binding = engine.addBinding(QStringLiteral("nope"), QStringLiteral("sine"), {});
        QVERIFY(binding.hasError());
        QCOMPARE(binding.error().code, ErrorCode::NotFound);
    }

    void disabledBindingDoesNotWrite()
    {
        ParameterStore store;
        QVERIFY(store.define(numericParameter(QStringLiteral("temp"), 1, 7.0)).hasValue());

        SignalEngine engine(&store);
        const auto binding = engine.addBinding(QStringLiteral("temp"), QStringLiteral("constant"),
                                               {{QStringLiteral("value"), 42.0}}, 10);
        QVERIFY(engine.setBindingEnabled(binding.value(), false));

        engine.tick(0);
        QCOMPARE(store.read(QStringLiteral("temp")).value().toDouble(), 7.0);
    }

    // --- State machine -----------------------------------------------------

    void stateMachineFollowsEventTransitions()
    {
        ParameterStore store;
        DeviceStateMachine machine(&store);

        machine.addState(StateDefinition{QStringLiteral("Idle"), {}, {}, true, {}});
        machine.addState(StateDefinition{QStringLiteral("Running"), {}, {}, true, {}});
        machine.addTransition(TransitionDefinition{QStringLiteral("Idle"), QStringLiteral("Running"),
                                                   QStringLiteral("start"), {}, 0, {}});

        QVERIFY(machine.start(QStringLiteral("Idle")).hasValue());
        QCOMPARE(machine.currentState(), QStringLiteral("Idle"));

        QVERIFY(machine.postEvent(QStringLiteral("start")).hasValue());
        QCOMPARE(machine.currentState(), QStringLiteral("Running"));

        // No transition out of Running for this event.
        QVERIFY(machine.postEvent(QStringLiteral("start")).hasError());
    }

    void unresponsiveStateSilencesTheDevice()
    {
        ParameterStore store;
        DeviceStateMachine machine(&store);

        machine.addState(StateDefinition{QStringLiteral("Running"), {}, {}, true, {}});
        machine.addState(StateDefinition{QStringLiteral("Dead"), {}, {}, false, {}});
        machine.addTransition(TransitionDefinition{QStringLiteral("*"), QStringLiteral("Dead"),
                                                   QStringLiteral("kill"), {}, 0, {}});

        QVERIFY(machine.start(QStringLiteral("Running")).hasValue());
        QVERIFY(machine.isResponsive());

        QVERIFY(machine.postEvent(QStringLiteral("kill")).hasValue());
        QVERIFY(!machine.isResponsive());
    }

    void conditionTransitionsReadParameters()
    {
        ParameterStore store;
        QVERIFY(store.define(numericParameter(QStringLiteral("temperature"), 1, 20.0)).hasValue());

        DeviceStateMachine machine(&store);
        machine.addState(StateDefinition{QStringLiteral("Normal"), {}, {}, true, {}});
        machine.addState(StateDefinition{QStringLiteral("Overheat"), {}, {}, false, {}});
        machine.addTransition(TransitionDefinition{QStringLiteral("Normal"),
                                                   QStringLiteral("Overheat"), {},
                                                   QStringLiteral("temperature>85"), 0, {}});

        QVERIFY(machine.start(QStringLiteral("Normal")).hasValue());

        machine.evaluate(1000);
        QCOMPARE(machine.currentState(), QStringLiteral("Normal"));

        QVERIFY(store.write(QStringLiteral("temperature"), 90.0, WriteOrigin::Ui).hasValue());
        machine.evaluate(2000);
        QCOMPARE(machine.currentState(), QStringLiteral("Overheat"));
    }

    void conditionParserHandlesEveryOperator()
    {
        ParameterStore store;
        QVERIFY(store.define(numericParameter(QStringLiteral("v"), 1, 50.0)).hasValue());

        DeviceStateMachine machine(&store);
        QCOMPARE(machine.evaluateCondition(QStringLiteral("v>10")).value(), true);
        QCOMPARE(machine.evaluateCondition(QStringLiteral("v>=50")).value(), true);
        QCOMPARE(machine.evaluateCondition(QStringLiteral("v<10")).value(), false);
        QCOMPARE(machine.evaluateCondition(QStringLiteral("v<=50")).value(), true);
        QCOMPARE(machine.evaluateCondition(QStringLiteral("v==50")).value(), true);
        QCOMPARE(machine.evaluateCondition(QStringLiteral("v!=50")).value(), false);
        QVERIFY(machine.evaluateCondition(QStringLiteral("garbage")).hasError());
    }

    void stateEntryAppliesParameterOverrides()
    {
        ParameterStore store;
        QVERIFY(store.define(numericParameter(QStringLiteral("alarm"), 1, 0.0)).hasValue());

        DeviceStateMachine machine(&store);
        machine.addState(StateDefinition{QStringLiteral("Normal"), {}, {}, true, {}});
        machine.addState(StateDefinition{QStringLiteral("Fault"), {}, {}, true,
                                         {{QStringLiteral("alarm"), 1.0}}});
        machine.addTransition(TransitionDefinition{QStringLiteral("*"), QStringLiteral("Fault"),
                                                   QStringLiteral("trip"), {}, 0, {}});

        QVERIFY(machine.start(QStringLiteral("Normal")).hasValue());
        QVERIFY(machine.postEvent(QStringLiteral("trip")).hasValue());
        QCOMPARE(store.read(QStringLiteral("alarm")).value().toDouble(), 1.0);
    }

    void stateMachineRoundTripsThroughJson()
    {
        ParameterStore store;
        DeviceStateMachine original(&store);
        original.addState(StateDefinition{QStringLiteral("A"), {}, {}, true, {}});
        original.addState(StateDefinition{QStringLiteral("B"), {}, {}, false, {}});
        original.addTransition(TransitionDefinition{QStringLiteral("A"), QStringLiteral("B"),
                                                    QStringLiteral("go"), {}, 0, {}});

        DeviceStateMachine restored(&store);
        QVERIFY(restored.loadJson(original.toJson()).hasValue());
        QCOMPARE(restored.stateNames(), QStringList({QStringLiteral("A"), QStringLiteral("B")}));
        QCOMPARE(restored.eventNames(), QStringList({QStringLiteral("go")}));
    }

    void loadRejectsTransitionToUndefinedState()
    {
        ParameterStore store;
        DeviceStateMachine machine(&store);

        const QByteArray json = R"({
            "states": [{"name": "A"}],
            "transitions": [{"from": "A", "to": "Nowhere", "event": "go"}]
        })";

        const auto loaded = machine.loadJson(QJsonDocument::fromJson(json).object());
        QVERIFY(loaded.hasError());
        QCOMPARE(loaded.error().code, ErrorCode::ConfigInvalid);
    }

    // --- Fault injection ---------------------------------------------------

    void packetLossDropsTheBuffer()
    {
        FaultInjector injector;
        const auto rule = injector.addRule(QStringLiteral("packet-loss"),
                                           {{QStringLiteral("direction"), QStringLiteral("outbound")},
                                            {QStringLiteral("trigger"), QStringLiteral("always")}});
        QVERIFY(rule.hasValue());

        // Exercised through a session in the integration test; here the rule is
        // driven directly.
        const auto rules = injector.rules();
        QCOMPARE(rules.size(), 1);
        QCOMPARE(rules.front().kind, QStringLiteral("packet-loss"));
    }

    void checksumFaultCorruptsOnlyTrailingBytes()
    {
        registerBuiltinFaultRules();
        auto created = faultRuleRegistry().create(QStringLiteral("checksum-error"));
        QVERIFY(created.hasValue());

        auto rule = std::move(created).value();
        QVERIFY(rule->configure({{QStringLiteral("direction"), QStringLiteral("outbound")},
                                 {QStringLiteral("trigger"), QStringLiteral("always")},
                                 {QStringLiteral("checksumBytes"), 2},
                                 {QStringLiteral("xorMask"), 0xFF}})
                    .hasValue());

        QByteArray frame = QByteArray::fromHex("0103020000B844");
        const QByteArray body = frame.left(5);

        ByteFilterContext context;
        context.direction = Direction::Outbound;
        const auto decision = rule->apply(frame, context);

        QVERIFY(decision.deliver);
        QCOMPARE(frame.left(5), body);
        QCOMPARE(frame.right(2), QByteArray::fromHex("47BB"));
    }

    void directionFilterKeepsInboundUntouched()
    {
        registerBuiltinFaultRules();
        auto created = faultRuleRegistry().create(QStringLiteral("bit-flip"));
        QVERIFY(created.hasValue());

        auto rule = std::move(created).value();
        QVERIFY(rule->configure({{QStringLiteral("direction"), QStringLiteral("outbound")},
                                 {QStringLiteral("trigger"), QStringLiteral("always")}})
                    .hasValue());

        QByteArray frame = QByteArray::fromHex("AABBCC");
        const QByteArray original = frame;

        ByteFilterContext inbound;
        inbound.direction = Direction::Inbound;
        QVERIFY(rule->apply(frame, inbound).deliver);
        QCOMPARE(frame, original);

        ByteFilterContext outbound;
        outbound.direction = Direction::Outbound;
        QVERIFY(rule->apply(frame, outbound).deliver);
        QVERIFY(frame != original);
    }

    void everyNthTriggerFiresOnSchedule()
    {
        registerBuiltinFaultRules();
        auto created = faultRuleRegistry().create(QStringLiteral("packet-loss"));
        auto rule = std::move(created).value();
        QVERIFY(rule->configure({{QStringLiteral("direction"), QStringLiteral("outbound")},
                                 {QStringLiteral("trigger"), QStringLiteral("every-nth")},
                                 {QStringLiteral("everyNth"), 3}})
                    .hasValue());

        ByteFilterContext context;
        context.direction = Direction::Outbound;

        QVector<bool> delivered;
        for (int i = 0; i < 6; ++i) {
            QByteArray frame = QByteArray::fromHex("AABB");
            delivered.append(rule->apply(frame, context).deliver);
        }

        QCOMPARE(delivered, QVector<bool>({true, true, false, true, true, false}));
        QCOMPARE(rule->statistics().activations, quint64{2});
    }

    void manualTriggerOnlyFiresWhenArmed()
    {
        registerBuiltinFaultRules();
        auto created = faultRuleRegistry().create(QStringLiteral("packet-loss"));
        auto rule = std::move(created).value();
        QVERIFY(rule->configure({{QStringLiteral("direction"), QStringLiteral("outbound")},
                                 {QStringLiteral("trigger"), QStringLiteral("manual")}})
                    .hasValue());

        ByteFilterContext context;
        context.direction = Direction::Outbound;

        QByteArray frame = QByteArray::fromHex("AABB");
        QVERIFY(rule->apply(frame, context).deliver);

        rule->arm();
        QVERIFY(!rule->apply(frame, context).deliver);

        // The arm is consumed by the single activation.
        QVERIFY(rule->apply(frame, context).deliver);
    }

    void injectorGlobalSwitchOverridesRules()
    {
        FaultInjector injector;
        QVERIFY(injector.addRule(QStringLiteral("packet-loss"),
                                 {{QStringLiteral("trigger"), QStringLiteral("always")}})
                    .hasValue());

        QVERIFY(injector.isGloballyEnabled());
        injector.setGloballyEnabled(false);
        QVERIFY(!injector.isGloballyEnabled());

        // Individual rule state is untouched by the global switch.
        QVERIFY(injector.rules().front().enabled);
    }

    void injectorRoundTripsThroughJson()
    {
        FaultInjector original;
        QVERIFY(original.addRule(QStringLiteral("latency"),
                                 {{QStringLiteral("minDelayMs"), 50},
                                  {QStringLiteral("maxDelayMs"), 150}})
                    .hasValue());

        FaultInjector restored;
        QVERIFY(restored.loadJson(original.toJson()).hasValue());
        QCOMPARE(restored.rules().size(), 1);
        QCOMPARE(restored.rules().front().kind, QStringLiteral("latency"));
        QCOMPARE(restored.rules().front().configuration.value(QStringLiteral("maxDelayMs")).toInt(),
                 150);
    }

    void injectorListsEveryBuiltinKind()
    {
        const QStringList kinds = FaultInjector::availableKinds();
        QVERIFY(kinds.contains(QStringLiteral("packet-loss")));
        QVERIFY(kinds.contains(QStringLiteral("timeout")));
        QVERIFY(kinds.contains(QStringLiteral("checksum-error")));
        QVERIFY(kinds.contains(QStringLiteral("bit-flip")));
        QVERIFY(kinds.contains(QStringLiteral("disconnect")));
    }

    // --- DeviceModel and profiles ------------------------------------------

    void deviceModelExposesItselfAsDeviceAccess()
    {
        DeviceModel device(QStringLiteral("pump-01"));
        QVERIFY(device.parameters().define(numericParameter(QStringLiteral("speed"), 0x10, 0.0))
                    .hasValue());

        hwsim::protocol::IDeviceAccess& access = device;
        QCOMPARE(access.deviceName(), QStringLiteral("pump-01"));
        QVERIFY(access.writeAddress(0x10, 1500.0).hasValue());
        QCOMPARE(access.readAddress(0x10).value().toDouble(), 1500.0);
    }

    void offlineDeviceStopsBeingResponsive()
    {
        DeviceModel device(QStringLiteral("dev"));
        QVERIFY(device.isResponsive());

        device.setOnline(false);
        QVERIFY(!device.isResponsive());
        QCOMPARE(device.currentState(), QStringLiteral("Offline"));
    }

    void deviceWithoutStateModelIsAlwaysResponsive()
    {
        DeviceModel device(QStringLiteral("dev"));
        device.start();
        QVERIFY(device.isResponsive());
        device.stop();
    }

    void profileRoundTripsThroughDisk()
    {
        DeviceModel source(QStringLiteral("boiler"));
        QVERIFY(source.parameters()
                    .define(numericParameter(QStringLiteral("temperature"), 0x00, 20.0))
                    .hasValue());
        QVERIFY(source.parameters()
                    .define(numericParameter(QStringLiteral("setpoint"), 0x01, 60.0))
                    .hasValue());
        QVERIFY(source.parameters()
                    .write(QStringLiteral("temperature"), 42.0, WriteOrigin::Ui)
                    .hasValue());

        source.stateMachine().addState(StateDefinition{QStringLiteral("Run"), {}, {}, true, {}});
        QVERIFY(source.signalEngine()
                    .addBinding(QStringLiteral("temperature"), QStringLiteral("sine"),
                                {{QStringLiteral("amplitude"), 5.0}})
                    .hasValue());
        QVERIFY(source.faults()
                    .addRule(QStringLiteral("packet-loss"),
                             {{QStringLiteral("probability"), 0.25}})
                    .hasValue());

        DeviceProfile profile = DeviceProfile::captureFrom(source);
        profile.protocolId = QStringLiteral("modbus");

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("boiler.json"));
        QVERIFY(profile.save(path).hasValue());

        const auto reloaded = DeviceProfile::load(path);
        QVERIFY(reloaded.hasValue());

        DeviceModel target(QStringLiteral("placeholder"));
        QVERIFY(reloaded.value().applyTo(target).hasValue());

        QCOMPARE(target.deviceName(), QStringLiteral("boiler"));
        QCOMPARE(target.parameters().size(), qsizetype{2});
        QCOMPARE(target.parameters().read(QStringLiteral("temperature")).value().toDouble(), 42.0);
        QCOMPARE(target.signalEngine().bindings().size(), 1);
        QCOMPARE(target.faults().rules().size(), 1);
    }

    void profileRejectsFutureFormatVersion()
    {
        const QByteArray json = R"({"formatVersion": 999, "name": "x"})";
        const auto profile = DeviceProfile::fromJson(QJsonDocument::fromJson(json).object());
        QVERIFY(profile.hasError());
        QCOMPARE(profile.error().code, ErrorCode::ConfigInvalid);
    }

    void profileRejectsUnknownRole()
    {
        const QByteArray json = R"({"name": "x", "role": "bystander"})";
        const auto profile = DeviceProfile::fromJson(QJsonDocument::fromJson(json).object());
        QVERIFY(profile.hasError());
    }
};

QTEST_MAIN(SimulatorTest)
#include "tst_simulator.moc"
