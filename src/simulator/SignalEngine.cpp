#include "SignalEngine.h"

#include <core/Clock.h>
#include <core/Logger.h>

#include <QJsonObject>
#include <QTimer>

#include <algorithm>

namespace {
constexpr auto kLogCategory = "simulator.signals";
}

using hwsim::core::ErrorCode;
using hwsim::core::makeError;
using hwsim::core::Result;

namespace hwsim::simulator {

QString combineModeName(CombineMode mode)
{
    static const QHash<CombineMode, QString> names{
        {CombineMode::Replace, QStringLiteral("replace")},
        {CombineMode::Add, QStringLiteral("add")},
        {CombineMode::Multiply, QStringLiteral("multiply")},
    };
    return names.value(mode, QStringLiteral("replace"));
}

CombineMode combineModeFromName(const QString& name)
{
    if (name == QStringLiteral("add")) return CombineMode::Add;
    if (name == QStringLiteral("multiply")) return CombineMode::Multiply;
    return CombineMode::Replace;
}

SignalEngine::SignalEngine(ParameterStore* store, QObject* parent) : QObject(parent), store_(store)
{
    registerBuiltinSignalSources();

    timer_ = new QTimer(this);
    timer_->setInterval(tickIntervalMs_);
    connect(timer_, &QTimer::timeout, this, &SignalEngine::onTimeout);
}

SignalEngine::~SignalEngine() = default;

Result<QString> SignalEngine::addBinding(const QString& parameterKey, const QString& sourceKind,
                                         const QVariantMap& configuration, int intervalMs,
                                         CombineMode combine)
{
    if (store_ == nullptr) {
        return makeError(ErrorCode::NotReady, QStringLiteral("no parameter store attached"));
    }
    if (!store_->contains(parameterKey)) {
        return makeError(ErrorCode::NotFound,
                         QStringLiteral("no parameter named '%1'").arg(parameterKey));
    }

    auto created = signalSourceRegistry().create(sourceKind);
    if (created.hasError()) {
        return created.error();
    }

    auto source = std::move(created).value();
    if (const auto configured = source->configure(configuration); configured.hasError()) {
        return configured.error();
    }

    auto binding = std::make_unique<Binding>();
    binding->id = QStringLiteral("sig-%1").arg(nextBindingId_++);
    binding->parameterKey = parameterKey;
    binding->source = std::move(source);
    binding->combine = combine;
    binding->intervalMs = qMax(1, intervalMs);
    binding->startedAtMs = core::monotonicMs();

    const QString id = binding->id;
    bindings_.push_back(std::move(binding));

    HWSIM_LOG_DEBUG(kLogCategory) << "bound " << sourceKind << " to " << parameterKey
                                  << " as " << id;
    return id;
}

bool SignalEngine::removeBinding(const QString& id)
{
    const auto removed = std::erase_if(
        bindings_, [&id](const std::unique_ptr<Binding>& binding) { return binding->id == id; });
    return removed > 0;
}

bool SignalEngine::setBindingEnabled(const QString& id, bool enabled)
{
    for (const auto& binding : bindings_) {
        if (binding->id == id) {
            binding->enabled = enabled;
            return true;
        }
    }
    return false;
}

Result<void> SignalEngine::reconfigureBinding(const QString& id, const QVariantMap& configuration)
{
    for (const auto& binding : bindings_) {
        if (binding->id != id) {
            continue;
        }
        if (const auto configured = binding->source->configure(configuration);
            configured.hasError()) {
            return configured;
        }
        binding->source->reset();
        binding->startedAtMs = core::monotonicMs();
        return core::success();
    }
    return makeError(ErrorCode::NotFound, QStringLiteral("no binding with id '%1'").arg(id));
}

void SignalEngine::clearBindings()
{
    bindings_.clear();
}

QVector<SignalBindingInfo> SignalEngine::bindings() const
{
    QVector<SignalBindingInfo> result;
    result.reserve(static_cast<qsizetype>(bindings_.size()));

    for (const auto& binding : bindings_) {
        SignalBindingInfo info;
        info.id = binding->id;
        info.parameterKey = binding->parameterKey;
        info.sourceKind = binding->source->kind();
        info.combine = binding->combine;
        info.intervalMs = binding->intervalMs;
        info.enabled = binding->enabled;
        info.configuration = binding->source->configuration();
        info.lastValue = binding->lastValue;
        result.append(std::move(info));
    }
    return result;
}

bool SignalEngine::hasBindingFor(const QString& parameterKey) const
{
    return std::any_of(bindings_.begin(), bindings_.end(),
                       [&parameterKey](const std::unique_ptr<Binding>& binding) {
                           return binding->parameterKey == parameterKey;
                       });
}

void SignalEngine::start()
{
    resetTimeBase();
    timer_->start();
}

void SignalEngine::stop()
{
    timer_->stop();
}

bool SignalEngine::isRunning() const
{
    return timer_ != nullptr && timer_->isActive();
}

void SignalEngine::resetTimeBase()
{
    const qint64 now = core::monotonicMs();
    for (const auto& binding : bindings_) {
        binding->source->reset();
        binding->startedAtMs = now;
        binding->lastSampleMs = -1;
    }
}

void SignalEngine::setTickIntervalMs(int milliseconds)
{
    tickIntervalMs_ = qMax(1, milliseconds);
    timer_->setInterval(tickIntervalMs_);
}

void SignalEngine::onTimeout()
{
    tick(core::monotonicMs());
}

QStringList SignalEngine::dueParameterKeys(qint64 nowMs) const
{
    QStringList keys;
    for (const auto& binding : bindings_) {
        if (!binding->enabled) {
            continue;
        }
        const bool due = binding->lastSampleMs < 0
                         || (nowMs - binding->lastSampleMs) >= binding->intervalMs;
        if (due && !keys.contains(binding->parameterKey)) {
            keys.append(binding->parameterKey);
        }
    }
    return keys;
}

void SignalEngine::tick(qint64 nowMs)
{
    if (store_ == nullptr) {
        return;
    }

    // A parameter is written once per tick using every binding attached to it,
    // so a "sine plus noise" pair produces one coherent value rather than two
    // competing writes.
    for (const QString& parameterKey : dueParameterKeys(nowMs)) {
        double value = 0.0;
        bool first = true;

        for (const auto& binding : bindings_) {
            if (!binding->enabled || binding->parameterKey != parameterKey) {
                continue;
            }

            const qint64 elapsed = nowMs - binding->startedAtMs;
            const double sample = binding->source->sample(elapsed);

            binding->lastValue = sample;
            binding->lastSampleMs = nowMs;

            if (first) {
                // The first contributor always establishes the base value,
                // whatever its combine mode says.
                value = sample;
                first = false;
                continue;
            }

            if (binding->combine == CombineMode::Add) {
                value += sample;
            } else if (binding->combine == CombineMode::Multiply) {
                value *= sample;
            } else {
                value = sample;
            }
        }

        if (first) {
            continue;
        }

        if (const auto written = store_->write(parameterKey, value, WriteOrigin::SignalSource);
            written.hasError()) {
            HWSIM_LOG_DEBUG(kLogCategory)
                << "signal write to " << parameterKey << " failed: " << written.error().toString();
            continue;
        }

        emit valueGenerated(parameterKey, value);
    }
}

QJsonArray SignalEngine::toJson() const
{
    QJsonArray array;
    for (const auto& binding : bindings_) {
        QJsonObject item;
        item.insert(QStringLiteral("parameter"), binding->parameterKey);
        item.insert(QStringLiteral("source"), binding->source->kind());
        item.insert(QStringLiteral("combine"), combineModeName(binding->combine));
        item.insert(QStringLiteral("intervalMs"), binding->intervalMs);
        item.insert(QStringLiteral("enabled"), binding->enabled);
        item.insert(QStringLiteral("config"),
                    QJsonObject::fromVariantMap(binding->source->configuration()));
        array.append(item);
    }
    return array;
}

Result<void> SignalEngine::loadJson(const QJsonArray& json)
{
    clearBindings();

    for (const QJsonValue& value : json) {
        const QJsonObject item = value.toObject();

        const auto added = addBinding(
            item.value(QStringLiteral("parameter")).toString(),
            item.value(QStringLiteral("source")).toString(),
            item.value(QStringLiteral("config")).toObject().toVariantMap(),
            item.value(QStringLiteral("intervalMs")).toInt(100),
            combineModeFromName(item.value(QStringLiteral("combine")).toString()));

        if (added.hasError()) {
            return added.error();
        }
        if (!item.value(QStringLiteral("enabled")).toBool(true)) {
            setBindingEnabled(added.value(), false);
        }
    }
    return core::success();
}

} // namespace hwsim::simulator
