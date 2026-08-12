#include "DeviceStateMachine.h"

#include <core/Clock.h>
#include <core/Logger.h>

#include <QJsonArray>
#include <QRegularExpression>
#include <QTimer>

#include <algorithm>

namespace {
constexpr auto kLogCategory = "simulator.state";
constexpr auto kAnyState = "*";
} // namespace

using hwsim::core::ErrorCode;
using hwsim::core::makeError;
using hwsim::core::Result;

namespace hwsim::simulator {

// --- Definitions -----------------------------------------------------------

QJsonObject StateDefinition::toJson() const
{
    QJsonObject json;
    json.insert(QStringLiteral("name"), name);
    if (!displayName.isEmpty() && displayName != name) {
        json.insert(QStringLiteral("displayName"), displayName);
    }
    if (!description.isEmpty()) json.insert(QStringLiteral("description"), description);
    if (!responsive) json.insert(QStringLiteral("responsive"), false);
    if (!parameterOverrides.isEmpty()) {
        json.insert(QStringLiteral("onEnter"), QJsonObject::fromVariantMap(parameterOverrides));
    }
    return json;
}

Result<StateDefinition> StateDefinition::fromJson(const QJsonObject& json)
{
    StateDefinition state;
    state.name = json.value(QStringLiteral("name")).toString();
    if (state.name.isEmpty()) {
        return makeError(ErrorCode::ConfigInvalid, QStringLiteral("state is missing 'name'"));
    }
    state.displayName = json.value(QStringLiteral("displayName")).toString(state.name);
    state.description = json.value(QStringLiteral("description")).toString();
    state.responsive = json.value(QStringLiteral("responsive")).toBool(true);
    state.parameterOverrides = json.value(QStringLiteral("onEnter")).toObject().toVariantMap();
    return state;
}

QJsonObject TransitionDefinition::toJson() const
{
    QJsonObject json;
    json.insert(QStringLiteral("from"), from);
    json.insert(QStringLiteral("to"), to);
    if (!event.isEmpty()) json.insert(QStringLiteral("event"), event);
    if (!condition.isEmpty()) json.insert(QStringLiteral("condition"), condition);
    if (afterMs > 0) json.insert(QStringLiteral("afterMs"), afterMs);
    if (!description.isEmpty()) json.insert(QStringLiteral("description"), description);
    return json;
}

Result<TransitionDefinition> TransitionDefinition::fromJson(const QJsonObject& json)
{
    TransitionDefinition transition;
    transition.from = json.value(QStringLiteral("from")).toString(QLatin1String(kAnyState));
    transition.to = json.value(QStringLiteral("to")).toString();
    if (transition.to.isEmpty()) {
        return makeError(ErrorCode::ConfigInvalid, QStringLiteral("transition is missing 'to'"));
    }
    transition.event = json.value(QStringLiteral("event")).toString();
    transition.condition = json.value(QStringLiteral("condition")).toString();
    transition.afterMs = json.value(QStringLiteral("afterMs")).toInt(0);
    transition.description = json.value(QStringLiteral("description")).toString();

    if (transition.event.isEmpty() && transition.condition.isEmpty() && transition.afterMs <= 0) {
        return makeError(ErrorCode::ConfigInvalid,
                         QStringLiteral("transition to '%1' has no trigger").arg(transition.to));
    }
    return transition;
}

// --- Machine ---------------------------------------------------------------

DeviceStateMachine::DeviceStateMachine(ParameterStore* store, QObject* parent)
    : QObject(parent), store_(store)
{
    timer_ = new QTimer(this);
    timer_->setInterval(evaluationIntervalMs_);
    connect(timer_, &QTimer::timeout, this, [this] { evaluate(core::monotonicMs()); });
}

DeviceStateMachine::~DeviceStateMachine() = default;

void DeviceStateMachine::addState(StateDefinition state)
{
    const auto existing = std::find_if(states_.begin(), states_.end(),
                                       [&state](const StateDefinition& candidate) {
                                           return candidate.name == state.name;
                                       });
    if (existing != states_.end()) {
        *existing = std::move(state);
    } else {
        states_.append(std::move(state));
    }
}

void DeviceStateMachine::addTransition(TransitionDefinition transition)
{
    transitions_.append(std::move(transition));
}

void DeviceStateMachine::clear()
{
    stop();
    states_.clear();
    transitions_.clear();
    currentState_.clear();
    initialState_.clear();
}

Result<void> DeviceStateMachine::start(const QString& initialState)
{
    if (states_.isEmpty()) {
        return makeError(ErrorCode::ConfigInvalid,
                         QStringLiteral("state machine has no states defined"));
    }

    QString target = initialState;
    if (target.isEmpty()) {
        target = initialState_.isEmpty() ? states_.first().name : initialState_;
    }
    if (const auto entered = enterState(target, QStringLiteral("start")); entered.hasError()) {
        return entered;
    }

    running_ = true;
    timer_->start();
    return core::success();
}

void DeviceStateMachine::stop()
{
    running_ = false;
    if (timer_ != nullptr) {
        timer_->stop();
    }
}

bool DeviceStateMachine::isRunning() const
{
    return running_;
}

bool DeviceStateMachine::isResponsive() const
{
    // A machine that was never started must not block traffic, otherwise every
    // device without an explicit state model would be silent.
    if (!running_ || currentState_.isEmpty()) {
        return true;
    }
    const StateDefinition* state = findState(currentState_);
    return state == nullptr || state->responsive;
}

qint64 DeviceStateMachine::timeInStateMs() const
{
    return stateEnteredMs_ == 0 ? 0 : core::monotonicMs() - stateEnteredMs_;
}

QStringList DeviceStateMachine::stateNames() const
{
    QStringList names;
    names.reserve(states_.size());
    for (const StateDefinition& state : states_) {
        names.append(state.name);
    }
    return names;
}

QStringList DeviceStateMachine::eventNames() const
{
    QStringList names;
    for (const TransitionDefinition& transition : transitions_) {
        if (!transition.event.isEmpty() && !names.contains(transition.event)) {
            names.append(transition.event);
        }
    }
    return names;
}

bool DeviceStateMachine::hasState(const QString& name) const
{
    return findState(name) != nullptr;
}

void DeviceStateMachine::setEvaluationIntervalMs(int milliseconds)
{
    evaluationIntervalMs_ = qMax(1, milliseconds);
    timer_->setInterval(evaluationIntervalMs_);
}

const StateDefinition* DeviceStateMachine::findState(const QString& name) const
{
    const auto it = std::find_if(states_.begin(), states_.end(),
                                 [&name](const StateDefinition& state) { return state.name == name; });
    return it == states_.end() ? nullptr : &(*it);
}

Result<void> DeviceStateMachine::enterState(const QString& name, const QString& reason)
{
    const StateDefinition* state = findState(name);
    if (state == nullptr) {
        return makeError(ErrorCode::NotFound, QStringLiteral("no state named '%1'").arg(name));
    }

    const QString previous = currentState_;
    currentState_ = name;
    stateEnteredMs_ = core::monotonicMs();

    if (store_ != nullptr) {
        for (auto it = state->parameterOverrides.constBegin();
             it != state->parameterOverrides.constEnd(); ++it) {
            if (const auto written = store_->write(it.key(), it.value(), WriteOrigin::StateMachine);
                written.hasError()) {
                HWSIM_LOG_WARNING(kLogCategory)
                    << "entering '" << name << "': " << written.error().toString();
            }
        }
    }

    HWSIM_LOG_INFO(kLogCategory) << "state " << (previous.isEmpty() ? QStringLiteral("<none>") : previous)
                                 << " -> " << name << " (" << reason << ')';
    emit stateChanged(previous, currentState_, reason);
    return core::success();
}

Result<void> DeviceStateMachine::forceState(const QString& stateName, const QString& reason)
{
    return enterState(stateName, reason.isEmpty() ? QStringLiteral("forced") : reason);
}

Result<void> DeviceStateMachine::postEvent(const QString& eventName)
{
    if (!running_) {
        return makeError(ErrorCode::NotReady, QStringLiteral("state machine is not running"));
    }

    const qint64 now = core::monotonicMs();
    for (const TransitionDefinition& transition : transitions_) {
        if (transition.event != eventName) {
            continue;
        }
        if (!transitionMatches(transition, eventName, now)) {
            continue;
        }
        return enterState(transition.to, QStringLiteral("event '%1'").arg(eventName));
    }

    return makeError(ErrorCode::NotFound,
                     QStringLiteral("no transition from '%1' for event '%2'")
                         .arg(currentState_, eventName));
}

void DeviceStateMachine::evaluate(qint64 nowMs)
{
    if (!running_) {
        return;
    }

    for (const TransitionDefinition& transition : transitions_) {
        // Event-driven transitions only fire through postEvent().
        if (!transition.event.isEmpty()) {
            continue;
        }
        if (!transitionMatches(transition, {}, nowMs)) {
            continue;
        }

        const QString reason = transition.afterMs > 0
                                   ? QStringLiteral("after %1 ms").arg(transition.afterMs)
                                   : QStringLiteral("condition '%1'").arg(transition.condition);
        if (const auto entered = enterState(transition.to, reason); entered.hasError()) {
            HWSIM_LOG_WARNING(kLogCategory) << entered.error().toString();
        }
        // Only one transition per evaluation, so a chain resolves one step at a
        // time and stays observable.
        return;
    }
}

bool DeviceStateMachine::transitionMatches(const TransitionDefinition& transition,
                                           const QString& eventName, qint64 nowMs) const
{
    if (transition.from != QLatin1String(kAnyState) && transition.from != currentState_) {
        return false;
    }
    if (!transition.event.isEmpty() && transition.event != eventName) {
        return false;
    }

    // Self-transitions are allowed only when an event triggers them, so that
    // re-running a state's parameterOverrides stays possible on demand.
    //
    // Timed and condition-driven self-transitions are rejected. stateEnteredMs_
    // is shared by every transition out of the current state, and re-entering
    // resets it: a {A -> A, afterMs: 100} rule would keep pushing the clock
    // back and a {A -> B, afterMs: 5000} rule alongside it could never reach
    // its deadline, trapping the machine in A forever.
    if (transition.to == currentState_ && transition.event.isEmpty()) {
        return false;
    }

    if (transition.afterMs > 0) {
        const qint64 elapsed = nowMs - stateEnteredMs_;
        if (elapsed < transition.afterMs) {
            return false;
        }
    }

    if (!transition.condition.isEmpty()) {
        const auto satisfied = evaluateCondition(transition.condition);
        if (satisfied.hasError() || !satisfied.value()) {
            return false;
        }
    }
    return true;
}

Result<bool> DeviceStateMachine::evaluateCondition(const QString& expression) const
{
    if (store_ == nullptr) {
        return makeError(ErrorCode::NotReady, QStringLiteral("no parameter store attached"));
    }

    static const QRegularExpression pattern(
        QStringLiteral("^\\s*([A-Za-z_][A-Za-z0-9_.\\-]*)\\s*(==|!=|>=|<=|>|<)\\s*(.+?)\\s*$"));

    const QRegularExpressionMatch match = pattern.match(expression);
    if (!match.hasMatch()) {
        return makeError(ErrorCode::ConfigInvalid,
                         QStringLiteral("cannot parse condition '%1'").arg(expression));
    }

    const QString key = match.captured(1);
    const QString op = match.captured(2);
    const QString literal = match.captured(3);

    const auto current = store_->read(key);
    if (current.hasError()) {
        return current.error();
    }

    bool leftIsNumber = false;
    bool rightIsNumber = false;
    const double left = current.value().toDouble(&leftIsNumber);
    const double right = literal.toDouble(&rightIsNumber);

    if (leftIsNumber && rightIsNumber) {
        if (op == QStringLiteral("==")) return qFuzzyCompare(left, right);
        if (op == QStringLiteral("!=")) return !qFuzzyCompare(left, right);
        if (op == QStringLiteral(">")) return left > right;
        if (op == QStringLiteral(">=")) return left >= right;
        if (op == QStringLiteral("<")) return left < right;
        if (op == QStringLiteral("<=")) return left <= right;
    }

    // Fall back to string comparison for enumerated or textual parameters.
    const QString leftText = current.value().toString();
    if (op == QStringLiteral("==")) return leftText == literal;
    if (op == QStringLiteral("!=")) return leftText != literal;

    return makeError(ErrorCode::ConfigInvalid,
                     QStringLiteral("operator '%1' needs numeric operands in '%2'")
                         .arg(op, expression));
}

// --- Persistence -----------------------------------------------------------

QJsonObject DeviceStateMachine::toJson() const
{
    QJsonArray stateArray;
    for (const StateDefinition& state : states_) {
        stateArray.append(state.toJson());
    }

    QJsonArray transitionArray;
    for (const TransitionDefinition& transition : transitions_) {
        transitionArray.append(transition.toJson());
    }

    QJsonObject json;
    json.insert(QStringLiteral("states"), stateArray);
    json.insert(QStringLiteral("transitions"), transitionArray);
    if (!states_.isEmpty()) {
        // The configured initial state, not merely the first one defined:
        // writing the latter would silently change where a reloaded machine
        // starts.
        json.insert(QStringLiteral("initial"),
                    initialState_.isEmpty() ? states_.first().name : initialState_);
    }
    return json;
}

Result<void> DeviceStateMachine::loadJson(const QJsonObject& json)
{
    clear();

    const QJsonArray stateArray = json.value(QStringLiteral("states")).toArray();
    for (const QJsonValue& value : stateArray) {
        const auto state = StateDefinition::fromJson(value.toObject());
        if (state.hasError()) {
            return state.error();
        }
        addState(state.value());
    }

    const QJsonArray transitionArray = json.value(QStringLiteral("transitions")).toArray();
    for (const QJsonValue& value : transitionArray) {
        const auto transition = TransitionDefinition::fromJson(value.toObject());
        if (transition.hasError()) {
            return transition.error();
        }
        if (!hasState(transition.value().to)) {
            return makeError(ErrorCode::ConfigInvalid,
                             QStringLiteral("transition targets undefined state '%1'")
                                 .arg(transition.value().to));
        }
        addTransition(transition.value());
    }

    initialState_ = json.value(QStringLiteral("initial")).toString();
    if (!initialState_.isEmpty() && !hasState(initialState_)) {
        return makeError(ErrorCode::ConfigInvalid,
                         QStringLiteral("initial state '%1' is not defined").arg(initialState_));
    }

    return core::success();
}

} // namespace hwsim::simulator
