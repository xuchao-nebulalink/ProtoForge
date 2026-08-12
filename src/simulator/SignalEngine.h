#pragma once

#include "ISignalSource.h"
#include "ParameterStore.h"

#include <QJsonArray>
#include <QObject>
#include <QVector>

#include <memory>

class QTimer;

namespace hwsim::simulator {

/// How a binding's output combines with the ones before it on the same parameter.
enum class CombineMode {
    Replace,
    Add,
    Multiply,
};

[[nodiscard]] HWSIM_SIMULATOR_API QString combineModeName(CombineMode mode);
[[nodiscard]] HWSIM_SIMULATOR_API CombineMode combineModeFromName(const QString& name);

struct HWSIM_SIMULATOR_API SignalBindingInfo {
    QString id;
    QString parameterKey;
    QString sourceKind;
    CombineMode combine{CombineMode::Replace};
    int intervalMs{100};
    bool enabled{true};
    QVariantMap configuration;
    double lastValue{0.0};
};

/// Drives parameter values from waveform generators.
///
/// One timer for the whole device rather than one per signal: a device with
/// fifty live channels would otherwise carry fifty timers, and their drift
/// would make the generated data non-reproducible.
///
/// Several bindings may target the same parameter and are combined in order,
/// which is how "sine plus noise" or "ramp times a gain" is expressed without a
/// composite-source type.
class HWSIM_SIMULATOR_API SignalEngine : public QObject {
    Q_OBJECT

public:
    explicit SignalEngine(ParameterStore* store, QObject* parent = nullptr);
    ~SignalEngine() override;

    /// Returns the binding id.
    [[nodiscard]] core::Result<QString> addBinding(const QString& parameterKey,
                                                   const QString& sourceKind,
                                                   const QVariantMap& configuration,
                                                   int intervalMs = 100,
                                                   CombineMode combine = CombineMode::Replace);

    bool removeBinding(const QString& id);
    bool setBindingEnabled(const QString& id, bool enabled);
    [[nodiscard]] core::Result<void> reconfigureBinding(const QString& id,
                                                        const QVariantMap& configuration);
    void clearBindings();

    [[nodiscard]] QVector<SignalBindingInfo> bindings() const;
    [[nodiscard]] bool hasBindingFor(const QString& parameterKey) const;

    void start();
    void stop();
    [[nodiscard]] bool isRunning() const;

    /// Restarts every source's time base, so a scenario can be replayed from zero.
    void resetTimeBase();

    void setTickIntervalMs(int milliseconds);
    [[nodiscard]] int tickIntervalMs() const noexcept { return tickIntervalMs_; }

    /// Advances the engine explicitly. Tests call this with a synthetic clock
    /// instead of waiting for the timer.
    void tick(qint64 nowMs);

    [[nodiscard]] QJsonArray toJson() const;
    [[nodiscard]] core::Result<void> loadJson(const QJsonArray& json);

signals:
    void valueGenerated(const QString& parameterKey, double value);

private:
    struct Binding {
        QString id;
        QString parameterKey;
        SignalSourcePtr source;
        CombineMode combine{CombineMode::Replace};
        int intervalMs{100};
        bool enabled{true};
        qint64 startedAtMs{0};
        qint64 lastSampleMs{-1};
        double lastValue{0.0};
    };

    void onTimeout();
    [[nodiscard]] QStringList dueParameterKeys(qint64 nowMs) const;

    ParameterStore* store_{nullptr};
    QTimer* timer_{nullptr};
    int tickIntervalMs_{50};
    qint64 nextBindingId_{1};
    std::vector<std::unique_ptr<Binding>> bindings_;
};

} // namespace hwsim::simulator
