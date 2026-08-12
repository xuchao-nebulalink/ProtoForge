#pragma once

#include "IScriptHost.h"

#include <QObject>
#include <QStringList>
#include <QVariant>

#include <functional>

namespace hwsim::scripting {

/// The `hwsim` object a scenario script sees.
///
/// Every method is deliberately non-throwing and returns a plain value: a
/// script that mistypes a device name gets a recorded failure and keeps going,
/// rather than aborting the run and leaving the devices half configured.
/// Failures are collected and reported at the end, so one script run yields one
/// pass/fail verdict with all the detail.
class HWSIM_SCRIPTING_API ScriptApi : public QObject {
    Q_OBJECT

public:
    explicit ScriptApi(IScriptHost& host, QObject* parent = nullptr);

    // --- Discovery ---

    Q_INVOKABLE QStringList devices() const;
    Q_INVOKABLE bool exists(const QString& deviceId) const;

    // --- Lifecycle ---

    Q_INVOKABLE bool start(const QString& deviceId);
    Q_INVOKABLE void stop(const QString& deviceId);
    Q_INVOKABLE int linkCount(const QString& deviceId) const;
    Q_INVOKABLE bool setOnline(const QString& deviceId, bool online);

    // --- Parameters ---

    Q_INVOKABLE QVariant read(const QString& deviceId, const QString& key);
    Q_INVOKABLE bool write(const QString& deviceId, const QString& key, const QVariant& value);

    // --- State machine ---

    Q_INVOKABLE QString state(const QString& deviceId) const;
    Q_INVOKABLE bool postEvent(const QString& deviceId, const QString& eventName);
    Q_INVOKABLE bool forceState(const QString& deviceId, const QString& stateName);

    // --- Fault injection ---

    Q_INVOKABLE QString injectFault(const QString& deviceId, const QString& kind,
                                    const QVariantMap& configuration = {});
    Q_INVOKABLE bool clearFault(const QString& deviceId, const QString& ruleId);
    Q_INVOKABLE bool armFault(const QString& deviceId, const QString& ruleId);
    Q_INVOKABLE void setFaultsEnabled(const QString& deviceId, bool enabled);

    // --- Traffic ---

    Q_INVOKABLE bool sendRaw(const QString& deviceId, const QString& hex);

    // --- Timing ---

    /// Spins the event loop so devices keep running while the script waits.
    Q_INVOKABLE void sleep(int milliseconds);

    /// Polls until `key` equals `expected` or the timeout expires.
    Q_INVOKABLE bool waitForValue(const QString& deviceId, const QString& key,
                                  const QVariant& expected, int timeoutMs = 5000);

    /// Polls until the device reports `stateName`.
    Q_INVOKABLE bool waitForState(const QString& deviceId, const QString& stateName,
                                  int timeoutMs = 5000);

    /// Polls until at least `count` links are up, e.g. after starting a server.
    Q_INVOKABLE bool waitForLink(const QString& deviceId, int count = 1, int timeoutMs = 5000);

    // --- Assertions and reporting ---

    Q_INVOKABLE bool check(bool condition, const QString& message);
    Q_INVOKABLE bool checkEqual(const QVariant& actual, const QVariant& expected,
                                const QString& message = {});
    Q_INVOKABLE void log(const QString& message);
    Q_INVOKABLE void fail(const QString& message);

    [[nodiscard]] QStringList failures() const { return failures_; }
    [[nodiscard]] int checkCount() const noexcept { return checkCount_; }
    [[nodiscard]] bool passed() const { return failures_.isEmpty(); }
    void resetResults();

signals:
    void checkFailed(const QString& message);
    void messageLogged(const QString& message);

private:
    /// Runs `predicate` every 10 ms until it succeeds or the deadline passes,
    /// keeping the event loop alive in between.
    bool pollUntil(const std::function<bool()>& predicate, int timeoutMs);

    void recordFailure(const QString& message);

    IScriptHost* host_{nullptr};
    QStringList failures_;
    int checkCount_{0};
};

} // namespace hwsim::scripting
