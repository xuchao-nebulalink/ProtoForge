#pragma once

#include "ParameterStore.h"

#include <QJsonObject>
#include <QObject>
#include <QVector>

class QTimer;

namespace hwsim::simulator {

struct HWSIM_SIMULATOR_API StateDefinition {
    QString name;
    QString displayName;
    QString description;

    /// Whether the device answers protocol requests while in this state.
    /// A state with responsive = false is how "the device went dark" is
    /// modelled; DeviceStateGateMiddleware reads it and stops replying.
    bool responsive{true};

    /// Values forced onto parameters when this state is entered, e.g. driving
    /// an alarm word high on entering Fault.
    QVariantMap parameterOverrides;

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static core::Result<StateDefinition> fromJson(const QJsonObject& json);
};

struct HWSIM_SIMULATOR_API TransitionDefinition {
    /// Source state, or "*" to match any state.
    QString from;
    QString to;

    /// Named trigger delivered through postEvent(). Empty means the transition
    /// is driven by condition or timer alone.
    QString event;

    /// Parameter predicate, "key op value" with op in == != < <= > >=.
    /// Example: "temperature>85".
    QString condition;

    /// Fires once the machine has been in `from` for this long. 0 disables it.
    int afterMs{0};

    QString description;

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static core::Result<TransitionDefinition> fromJson(const QJsonObject& json);
};

/// Device operating-mode model.
///
/// Deliberately a small explicit machine rather than QStateMachine: the whole
/// definition has to round-trip through JSON so that a scenario can be saved,
/// diffed and replayed in CI, and the transition set has to be inspectable by
/// the UI.
class HWSIM_SIMULATOR_API DeviceStateMachine : public QObject {
    Q_OBJECT

public:
    explicit DeviceStateMachine(ParameterStore* store, QObject* parent = nullptr);
    ~DeviceStateMachine() override;

    void addState(StateDefinition state);
    void addTransition(TransitionDefinition transition);
    void clear();

    /// Enters `initialState`, or the first defined state when empty.
    [[nodiscard]] core::Result<void> start(const QString& initialState = {});
    void stop();
    [[nodiscard]] bool isRunning() const;

    [[nodiscard]] QString currentState() const { return currentState_; }
    [[nodiscard]] bool isResponsive() const;
    [[nodiscard]] qint64 timeInStateMs() const;

    /// Delivers a named trigger. Returns NotFound when no transition matches,
    /// which callers may legitimately ignore.
    [[nodiscard]] core::Result<void> postEvent(const QString& eventName);

    /// Jumps directly, bypassing the transition table. Used by the UI and by scripts.
    [[nodiscard]] core::Result<void> forceState(const QString& stateName, const QString& reason = {});

    /// Re-checks timed and condition-driven transitions. Called by the internal
    /// timer, or directly by tests with a synthetic clock.
    void evaluate(qint64 nowMs);

    [[nodiscard]] QVector<StateDefinition> states() const { return states_; }
    [[nodiscard]] QVector<TransitionDefinition> transitions() const { return transitions_; }
    [[nodiscard]] QStringList stateNames() const;
    [[nodiscard]] QStringList eventNames() const;

    [[nodiscard]] bool hasState(const QString& name) const;

    void setEvaluationIntervalMs(int milliseconds);

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] core::Result<void> loadJson(const QJsonObject& json);

    /// Exposed for testing the predicate parser directly.
    [[nodiscard]] core::Result<bool> evaluateCondition(const QString& expression) const;

signals:
    void stateChanged(const QString& fromState, const QString& toState, const QString& reason);

private:
    [[nodiscard]] const StateDefinition* findState(const QString& name) const;
    [[nodiscard]] core::Result<void> enterState(const QString& name, const QString& reason);
    [[nodiscard]] bool transitionMatches(const TransitionDefinition& transition,
                                         const QString& eventName, qint64 nowMs) const;

    ParameterStore* store_{nullptr};
    QVector<StateDefinition> states_;
    QVector<TransitionDefinition> transitions_;

    QString currentState_;
    QString initialState_;
    qint64 stateEnteredMs_{0};
    bool running_{false};

    QTimer* timer_{nullptr};
    int evaluationIntervalMs_{100};
};

} // namespace hwsim::simulator
