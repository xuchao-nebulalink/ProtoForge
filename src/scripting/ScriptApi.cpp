#include "ScriptApi.h"

#include <core/Clock.h>
#include <core/HexUtils.h>
#include <core/Logger.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

namespace {
constexpr auto kLogCategory = "script";
constexpr int kPollIntervalMs = 10;
} // namespace

namespace hwsim::scripting {

ScriptApi::ScriptApi(IScriptHost& host, QObject* parent) : QObject(parent), host_(&host) {}

QStringList ScriptApi::devices() const
{
    return host_->scriptDeviceIds();
}

bool ScriptApi::exists(const QString& deviceId) const
{
    return host_->hasDevice(deviceId);
}

bool ScriptApi::start(const QString& deviceId)
{
    const auto started = host_->scriptStartDevice(deviceId);
    if (started.hasError()) {
        recordFailure(QStringLiteral("start('%1'): %2").arg(deviceId, started.error().toString()));
        return false;
    }
    return true;
}

void ScriptApi::stop(const QString& deviceId)
{
    host_->scriptStopDevice(deviceId);
}

int ScriptApi::linkCount(const QString& deviceId) const
{
    return host_->scriptLinkCount(deviceId);
}

bool ScriptApi::setOnline(const QString& deviceId, bool online)
{
    if (!host_->hasDevice(deviceId)) {
        recordFailure(QStringLiteral("setOnline: unknown device '%1'").arg(deviceId));
        return false;
    }
    host_->scriptSetOnline(deviceId, online);
    return true;
}

QVariant ScriptApi::read(const QString& deviceId, const QString& key)
{
    const auto value = host_->scriptReadParameter(deviceId, key);
    if (value.hasError()) {
        recordFailure(QStringLiteral("read('%1', '%2'): %3")
                          .arg(deviceId, key, value.error().toString()));
        return {};
    }
    return value.value();
}

bool ScriptApi::write(const QString& deviceId, const QString& key, const QVariant& value)
{
    const auto written = host_->scriptWriteParameter(deviceId, key, value);
    if (written.hasError()) {
        recordFailure(QStringLiteral("write('%1', '%2'): %3")
                          .arg(deviceId, key, written.error().toString()));
        return false;
    }
    return true;
}

QString ScriptApi::state(const QString& deviceId) const
{
    return host_->scriptCurrentState(deviceId);
}

bool ScriptApi::postEvent(const QString& deviceId, const QString& eventName)
{
    const auto posted = host_->scriptPostEvent(deviceId, eventName);
    if (posted.hasError()) {
        recordFailure(QStringLiteral("postEvent('%1', '%2'): %3")
                          .arg(deviceId, eventName, posted.error().toString()));
        return false;
    }
    return true;
}

bool ScriptApi::forceState(const QString& deviceId, const QString& stateName)
{
    const auto forced = host_->scriptForceState(deviceId, stateName);
    if (forced.hasError()) {
        recordFailure(QStringLiteral("forceState('%1', '%2'): %3")
                          .arg(deviceId, stateName, forced.error().toString()));
        return false;
    }
    return true;
}

QString ScriptApi::injectFault(const QString& deviceId, const QString& kind,
                               const QVariantMap& configuration)
{
    const auto added = host_->scriptAddFault(deviceId, kind, configuration);
    if (added.hasError()) {
        recordFailure(QStringLiteral("injectFault('%1', '%2'): %3")
                          .arg(deviceId, kind, added.error().toString()));
        return {};
    }
    HWSIM_LOG_INFO(kLogCategory) << "injected fault " << added.value() << " on " << deviceId;
    return added.value();
}

bool ScriptApi::clearFault(const QString& deviceId, const QString& ruleId)
{
    return host_->scriptRemoveFault(deviceId, ruleId);
}

bool ScriptApi::armFault(const QString& deviceId, const QString& ruleId)
{
    return host_->scriptArmFault(deviceId, ruleId);
}

void ScriptApi::setFaultsEnabled(const QString& deviceId, bool enabled)
{
    host_->scriptSetFaultsEnabled(deviceId, enabled);
}

bool ScriptApi::sendRaw(const QString& deviceId, const QString& hex)
{
    bool ok = false;
    const QByteArray bytes = core::hex::fromHex(hex, &ok);
    if (!ok) {
        recordFailure(QStringLiteral("sendRaw: '%1' is not valid hex").arg(hex));
        return false;
    }

    const auto sent = host_->scriptSendRaw(deviceId, bytes);
    if (sent.hasError()) {
        recordFailure(QStringLiteral("sendRaw('%1'): %2").arg(deviceId, sent.error().toString()));
        return false;
    }
    return true;
}

void ScriptApi::sleep(int milliseconds)
{
    // Processing events rather than blocking: the devices this script is
    // driving run on their own threads but their timers and queued deliveries
    // still need this thread's loop to turn.
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, kPollIntervalMs);
        QThread::msleep(1);
    }
}

bool ScriptApi::pollUntil(const std::function<bool()>& predicate, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();

    while (timer.elapsed() < timeoutMs) {
        if (predicate()) {
            return true;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, kPollIntervalMs);
        QThread::msleep(1);
    }
    return predicate();
}

bool ScriptApi::waitForValue(const QString& deviceId, const QString& key, const QVariant& expected,
                             int timeoutMs)
{
    const bool satisfied = pollUntil(
        [this, &deviceId, &key, &expected] {
            const auto value = host_->scriptReadParameter(deviceId, key);
            return value.hasValue() && value.value() == expected;
        },
        timeoutMs);

    if (!satisfied) {
        recordFailure(QStringLiteral("waitForValue('%1', '%2', %3) timed out after %4 ms")
                          .arg(deviceId, key, expected.toString())
                          .arg(timeoutMs));
    }
    return satisfied;
}

bool ScriptApi::waitForState(const QString& deviceId, const QString& stateName, int timeoutMs)
{
    const bool satisfied = pollUntil(
        [this, &deviceId, &stateName] { return host_->scriptCurrentState(deviceId) == stateName; },
        timeoutMs);

    if (!satisfied) {
        recordFailure(QStringLiteral("waitForState('%1', '%2') timed out; still in '%3'")
                          .arg(deviceId, stateName, host_->scriptCurrentState(deviceId)));
    }
    return satisfied;
}

bool ScriptApi::waitForLink(const QString& deviceId, int count, int timeoutMs)
{
    const bool satisfied = pollUntil(
        [this, &deviceId, count] { return host_->scriptLinkCount(deviceId) >= count; }, timeoutMs);

    if (!satisfied) {
        recordFailure(QStringLiteral("waitForLink('%1', %2) timed out with %3 link(s)")
                          .arg(deviceId)
                          .arg(count)
                          .arg(host_->scriptLinkCount(deviceId)));
    }
    return satisfied;
}

bool ScriptApi::check(bool condition, const QString& message)
{
    ++checkCount_;
    if (!condition) {
        recordFailure(message);
    }
    return condition;
}

bool ScriptApi::checkEqual(const QVariant& actual, const QVariant& expected, const QString& message)
{
    ++checkCount_;

    // Numeric comparison first: a script literal is a double while a register
    // value may be an integer, and JavaScript users expect 5 == 5.0.
    bool actualIsNumber = false;
    bool expectedIsNumber = false;
    const double actualNumber = actual.toDouble(&actualIsNumber);
    const double expectedNumber = expected.toDouble(&expectedIsNumber);

    const bool equal = (actualIsNumber && expectedIsNumber)
                           ? qFuzzyCompare(actualNumber + 1.0, expectedNumber + 1.0)
                           : actual == expected;

    if (!equal) {
        recordFailure(QStringLiteral("%1expected %2, got %3")
                          .arg(message.isEmpty() ? QString{} : message + QStringLiteral(": "),
                               expected.toString(), actual.toString()));
    }
    return equal;
}

void ScriptApi::log(const QString& message)
{
    HWSIM_LOG_INFO(kLogCategory) << message;
    emit messageLogged(message);
}

void ScriptApi::fail(const QString& message)
{
    ++checkCount_;
    recordFailure(message);
}

void ScriptApi::recordFailure(const QString& message)
{
    failures_.append(message);
    HWSIM_LOG_ERROR(kLogCategory) << "FAIL: " << message;
    emit checkFailed(message);
}

void ScriptApi::resetResults()
{
    failures_.clear();
    checkCount_ = 0;
}

} // namespace hwsim::scripting
