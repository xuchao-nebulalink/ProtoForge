#include <core/Logger.h>
#include <scripting/ScriptEngine.h>

#include <QTest>

using namespace hwsim;
using namespace hwsim::core;
using namespace hwsim::scripting;

namespace {

/// In-memory stand-in for a Workspace, so the scripting surface can be tested
/// without starting sockets or device threads.
class FakeHost final : public IScriptHost {
public:
    QStringList scriptDeviceIds() const override { return values.keys(); }
    bool hasDevice(const QString& deviceId) const override { return values.contains(deviceId); }

    Result<void> scriptStartDevice(const QString& deviceId) override
    {
        if (!hasDevice(deviceId)) {
            return makeError(ErrorCode::NotFound, deviceId);
        }
        started.insert(deviceId);
        return success();
    }

    void scriptStopDevice(const QString& deviceId) override { started.remove(deviceId); }

    int scriptLinkCount(const QString& deviceId) const override
    {
        return started.contains(deviceId) ? 1 : 0;
    }

    Result<QVariant> scriptReadParameter(const QString& deviceId, const QString& key) const override
    {
        if (!hasDevice(deviceId)) {
            return makeError(ErrorCode::NotFound, deviceId);
        }
        const QVariantMap& parameters = values.value(deviceId);
        if (!parameters.contains(key)) {
            return makeError(ErrorCode::NotFound, key);
        }
        return parameters.value(key);
    }

    Result<void> scriptWriteParameter(const QString& deviceId, const QString& key,
                                      const QVariant& value) override
    {
        if (!hasDevice(deviceId)) {
            return makeError(ErrorCode::NotFound, deviceId);
        }
        values[deviceId].insert(key, value);
        return success();
    }

    QString scriptCurrentState(const QString& deviceId) const override
    {
        return states.value(deviceId, QStringLiteral("Idle"));
    }

    Result<void> scriptPostEvent(const QString& deviceId, const QString& eventName) override
    {
        if (eventName == QStringLiteral("run")) {
            states[deviceId] = QStringLiteral("Running");
            return success();
        }
        return makeError(ErrorCode::NotFound, eventName);
    }

    Result<void> scriptForceState(const QString& deviceId, const QString& stateName) override
    {
        states[deviceId] = stateName;
        return success();
    }

    void scriptSetOnline(const QString& deviceId, bool online) override
    {
        onlineFlags[deviceId] = online;
    }

    Result<QString> scriptAddFault(const QString& deviceId, const QString& kind,
                                   const QVariantMap&) override
    {
        if (!hasDevice(deviceId)) {
            return makeError(ErrorCode::NotFound, deviceId);
        }
        const QString id = QStringLiteral("%1-%2").arg(kind).arg(++faultCounter);
        faults.append(id);
        return id;
    }

    bool scriptRemoveFault(const QString&, const QString& ruleId) override
    {
        return faults.removeOne(ruleId);
    }

    bool scriptArmFault(const QString&, const QString& ruleId) override
    {
        return faults.contains(ruleId);
    }

    void scriptSetFaultsEnabled(const QString&, bool enabled) override
    {
        faultsEnabled = enabled;
    }

    Result<void> scriptSendRaw(const QString& deviceId, const QByteArray& bytes) override
    {
        if (!hasDevice(deviceId)) {
            return makeError(ErrorCode::NotFound, deviceId);
        }
        sentBytes.append(bytes);
        return success();
    }

    QMap<QString, QVariantMap> values;
    QMap<QString, QString> states;
    QMap<QString, bool> onlineFlags;
    QSet<QString> started;
    QStringList faults;
    QList<QByteArray> sentBytes;
    bool faultsEnabled{true};
    int faultCounter{0};
};

} // namespace

class ScriptingTest : public QObject {
    Q_OBJECT

private:
    FakeHost host_;

private slots:
    void init()
    {
        Logger::instance().setAsynchronous(false);
        Logger::instance().setLevel(LogLevel::Critical);

        host_ = FakeHost{};
        host_.values.insert(QStringLiteral("dut"),
                            {{QStringLiteral("setpoint"), 100}, {QStringLiteral("temperature"), 25}});
    }

    void scriptSeesTheDeviceList()
    {
        ScriptEngine engine(host_);
        const auto outcome = engine.runSource(QStringLiteral(
            "hwsim.check(hwsim.devices().length === 1, 'one device');"
            "hwsim.check(hwsim.exists('dut'), 'dut exists');"));

        QVERIFY(outcome.completed);
        QVERIFY2(outcome.passed, qPrintable(outcome.failures.join(QLatin1Char('\n'))));
        QCOMPARE(outcome.checkCount, 2);
    }

    void scriptReadsAndWritesParameters()
    {
        ScriptEngine engine(host_);
        const auto outcome = engine.runSource(QStringLiteral(
            "hwsim.checkEqual(hwsim.read('dut', 'setpoint'), 100, 'initial setpoint');"
            "hwsim.write('dut', 'setpoint', 250);"
            "hwsim.checkEqual(hwsim.read('dut', 'setpoint'), 250, 'after write');"));

        QVERIFY(outcome.completed);
        QVERIFY2(outcome.passed, qPrintable(outcome.failures.join(QLatin1Char('\n'))));
        QCOMPARE(host_.values.value(QStringLiteral("dut")).value(QStringLiteral("setpoint")).toInt(),
                 250);
    }

    void checkEqualComparesNumbersAcrossTypes()
    {
        ScriptEngine engine(host_);
        // The register holds an int, the script literal is a double; a naive
        // QVariant comparison would fail here.
        const auto outcome = engine.runSource(
            QStringLiteral("hwsim.checkEqual(hwsim.read('dut', 'temperature'), 25.0);"));

        QVERIFY(outcome.completed);
        QVERIFY(outcome.passed);
    }

    void failedCheckIsReportedWithoutAbortingTheScript()
    {
        ScriptEngine engine(host_);
        const auto outcome = engine.runSource(QStringLiteral(
            "hwsim.check(false, 'deliberate failure');"
            "hwsim.check(true, 'still runs');"));

        QVERIFY(outcome.completed);
        QVERIFY(!outcome.passed);
        QCOMPARE(outcome.checkCount, 2);
        QCOMPARE(outcome.failures.size(), 1);
        QVERIFY(outcome.failures.first().contains(QStringLiteral("deliberate failure")));
    }

    void unknownDeviceBecomesAFailureNotACrash()
    {
        ScriptEngine engine(host_);
        const auto outcome = engine.runSource(QStringLiteral("hwsim.read('nope', 'x');"));

        QVERIFY(outcome.completed);
        QVERIFY(!outcome.passed);
        QVERIFY(outcome.failures.first().contains(QStringLiteral("nope")));
    }

    void syntaxErrorIsReportedWithLocation()
    {
        ScriptEngine engine(host_);
        const auto outcome = engine.runSource(QStringLiteral("this is not javascript"),
                                              QStringLiteral("broken.js"));

        QVERIFY(!outcome.completed);
        QVERIFY(!outcome.errorText.isEmpty());
        QVERIFY(outcome.summary().contains(QStringLiteral("中断")));
    }

    void stateMachineControlWorksFromScript()
    {
        ScriptEngine engine(host_);
        const auto outcome = engine.runSource(QStringLiteral(
            "hwsim.checkEqual(hwsim.state('dut'), 'Idle');"
            "hwsim.postEvent('dut', 'run');"
            "hwsim.checkEqual(hwsim.state('dut'), 'Running');"
            "hwsim.forceState('dut', 'Maintenance');"
            "hwsim.checkEqual(hwsim.state('dut'), 'Maintenance');"));

        QVERIFY(outcome.completed);
        QVERIFY2(outcome.passed, qPrintable(outcome.failures.join(QLatin1Char('\n'))));
    }

    void faultInjectionIsScriptable()
    {
        ScriptEngine engine(host_);
        const auto outcome = engine.runSource(QStringLiteral(
            "var id = hwsim.injectFault('dut', 'packet-loss', {probability: 1.0});"
            "hwsim.check(id.length > 0, 'fault id returned');"
            "hwsim.check(hwsim.armFault('dut', id), 'fault can be armed');"
            "hwsim.check(hwsim.clearFault('dut', id), 'fault can be cleared');"));

        QVERIFY(outcome.completed);
        QVERIFY2(outcome.passed, qPrintable(outcome.failures.join(QLatin1Char('\n'))));
        QVERIFY(host_.faults.isEmpty());
    }

    void rawSendAcceptsHexStrings()
    {
        ScriptEngine engine(host_);
        const auto outcome = engine.runSource(QStringLiteral(
            "hwsim.check(hwsim.sendRaw('dut', '01 03 00 00 00 0A'), 'raw send accepted');"
            "hwsim.check(!hwsim.sendRaw('dut', 'not hex'), 'garbage rejected');"));

        QVERIFY(outcome.completed);
        QVERIFY2(outcome.passed, qPrintable(outcome.failures.join(QLatin1Char('\n'))));
        QCOMPARE(host_.sentBytes.size(), 1);
        QCOMPARE(host_.sentBytes.first(), QByteArray::fromHex("01030000000A"));
    }

    void waitForValueTimesOutAndReportsIt()
    {
        ScriptEngine engine(host_);
        const auto outcome = engine.runSource(QStringLiteral(
            "hwsim.waitForValue('dut', 'setpoint', 9999, 100);"));

        QVERIFY(outcome.completed);
        QVERIFY(!outcome.passed);
        QVERIFY(outcome.failures.first().contains(QStringLiteral("timed out")));
    }

    void waitForValueSucceedsImmediatelyWhenAlreadyEqual()
    {
        ScriptEngine engine(host_);
        const auto outcome = engine.runSource(QStringLiteral(
            "hwsim.check(hwsim.waitForValue('dut', 'setpoint', 100, 500), 'already at value');"));

        QVERIFY(outcome.completed);
        QVERIFY2(outcome.passed, qPrintable(outcome.failures.join(QLatin1Char('\n'))));
    }

    void lifecycleControlWorksFromScript()
    {
        ScriptEngine engine(host_);
        const auto outcome = engine.runSource(QStringLiteral(
            "hwsim.check(hwsim.start('dut'), 'started');"
            "hwsim.check(hwsim.waitForLink('dut', 1, 200), 'link came up');"
            "hwsim.stop('dut');"
            "hwsim.checkEqual(hwsim.linkCount('dut'), 0, 'link gone');"));

        QVERIFY(outcome.completed);
        QVERIFY2(outcome.passed, qPrintable(outcome.failures.join(QLatin1Char('\n'))));
    }
};

QTEST_MAIN(ScriptingTest)
#include "tst_scripting.moc"
