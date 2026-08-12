#include "FaultInjector.h"

#include <core/Logger.h>

#include <QJsonObject>

#include <algorithm>

namespace {
constexpr auto kLogCategory = "simulator.faults";
}

using hwsim::core::ConfigSchema;
using hwsim::core::ErrorCode;
using hwsim::core::makeError;
using hwsim::core::Result;
using hwsim::protocol::ByteFilterContext;
using hwsim::protocol::ByteFilterDecision;

namespace hwsim::simulator {

// --- Dispatcher ------------------------------------------------------------

ByteFilterDecision FaultInjector::Dispatcher::apply(QByteArray& bytes,
                                                    const ByteFilterContext& context)
{
    ByteFilterDecision merged;
    QStringList notes;

    for (const FaultRulePtr& rule : rules) {
        const ByteFilterDecision decision = rule->apply(bytes, context);

        if (!decision.note.isEmpty()) {
            notes.append(decision.note);
            if (onActivated) {
                onActivated(rule->name(), decision.note);
            }
        }
        merged.delayMs += decision.delayMs;

        if (!decision.deliver) {
            merged.deliver = false;
            merged.note = notes.join(QStringLiteral("; "));
            return merged;
        }
    }

    merged.note = notes.join(QStringLiteral("; "));
    return merged;
}

// --- FaultInjector ---------------------------------------------------------

FaultInjector::FaultInjector(QObject* parent)
    : QObject(parent), dispatcher_(std::make_shared<Dispatcher>())
{
    registerBuiltinFaultRules();

    dispatcher_->onActivated = [this](const QString& ruleId, const QString& note) {
        emit faultActivated(ruleId, note);
    };
}

FaultInjector::~FaultInjector()
{
    // Break the callback so a queued emission cannot reach a half-destroyed
    // QObject through the shared dispatcher, which sessions may still hold.
    dispatcher_->onActivated = nullptr;
    dispatcher_->rules.clear();
}

Result<QString> FaultInjector::addRule(const QString& kind, const QVariantMap& configuration)
{
    auto created = faultRuleRegistry().create(kind);
    if (created.hasError()) {
        return created.error();
    }

    std::shared_ptr<FaultRuleBase> rule = std::move(created).value();
    rule->setId(QStringLiteral("%1-%2").arg(kind).arg(nextRuleId_++));

    if (const auto configured = rule->configure(configuration); configured.hasError()) {
        return configured.error();
    }

    // Rules can be added at any time, including from a script mid-run, so the
    // disconnect hook is handed to each new rule rather than only to the set
    // that existed when the runtime wired itself up.
    if (disconnectRequest_) {
        rule->setDisconnectRequest(disconnectRequest_);
    }

    const QString id = rule->name();
    dispatcher_->rules.push_back(std::move(rule));

    HWSIM_LOG_INFO(kLogCategory) << "added fault rule " << id;
    emit rulesChanged();
    return id;
}

bool FaultInjector::removeRule(const QString& id)
{
    const auto removed = std::erase_if(dispatcher_->rules,
                                       [&id](const FaultRulePtr& rule) { return rule->name() == id; });
    if (removed > 0) {
        emit rulesChanged();
    }
    return removed > 0;
}

FaultRulePtr FaultInjector::findRule(const QString& id) const
{
    const auto it = std::find_if(dispatcher_->rules.begin(), dispatcher_->rules.end(),
                                 [&id](const FaultRulePtr& rule) { return rule->name() == id; });
    return it == dispatcher_->rules.end() ? nullptr : *it;
}

bool FaultInjector::setRuleEnabled(const QString& id, bool enabled)
{
    const FaultRulePtr rule = findRule(id);
    if (!rule) {
        return false;
    }
    rule->setEnabled(enabled);
    emit rulesChanged();
    return true;
}

bool FaultInjector::armRule(const QString& id)
{
    const FaultRulePtr rule = findRule(id);
    if (!rule) {
        return false;
    }
    rule->arm();
    return true;
}

Result<void> FaultInjector::reconfigureRule(const QString& id, const QVariantMap& configuration)
{
    const FaultRulePtr rule = findRule(id);
    if (!rule) {
        return makeError(ErrorCode::NotFound, QStringLiteral("no fault rule with id '%1'").arg(id));
    }
    if (const auto configured = rule->configure(configuration); configured.hasError()) {
        return configured;
    }
    emit rulesChanged();
    return core::success();
}

void FaultInjector::clear()
{
    dispatcher_->rules.clear();
    emit rulesChanged();
}

QVector<FaultRuleInfo> FaultInjector::rules() const
{
    QVector<FaultRuleInfo> result;
    result.reserve(static_cast<qsizetype>(dispatcher_->rules.size()));

    for (const FaultRulePtr& rule : dispatcher_->rules) {
        FaultRuleInfo info;
        info.id = rule->name();
        info.kind = rule->kind();
        info.displayName = rule->displayName();
        info.enabled = rule->isEnabled();
        info.configuration = rule->configuration();
        info.statistics = rule->statistics();
        result.append(std::move(info));
    }
    return result;
}

bool FaultInjector::isEmpty() const
{
    return dispatcher_->rules.empty();
}

void FaultInjector::resetStatistics()
{
    for (const FaultRulePtr& rule : dispatcher_->rules) {
        rule->resetStatistics();
    }
}

void FaultInjector::setGloballyEnabled(bool enabled)
{
    dispatcher_->enabled = enabled;
    HWSIM_LOG_INFO(kLogCategory) << "fault injection " << (enabled ? "enabled" : "disabled");
}

bool FaultInjector::isGloballyEnabled() const
{
    return dispatcher_->enabled;
}

void FaultInjector::setDisconnectRequest(FaultRuleBase::DisconnectRequest request)
{
    disconnectRequest_ = std::move(request);
    for (const FaultRulePtr& rule : dispatcher_->rules) {
        rule->setDisconnectRequest(disconnectRequest_);
    }
}

void FaultInjector::attachTo(protocol::ProtocolSession& session)
{
    session.inboundFilters().add(dispatcher_);
    session.outboundFilters().add(dispatcher_);
}

void FaultInjector::detachFrom(protocol::ProtocolSession& session)
{
    session.inboundFilters().remove(dispatcher_->name());
    session.outboundFilters().remove(dispatcher_->name());
}

QJsonArray FaultInjector::toJson() const
{
    QJsonArray array;
    for (const FaultRulePtr& rule : dispatcher_->rules) {
        QJsonObject item;
        item.insert(QStringLiteral("kind"), rule->kind());
        item.insert(QStringLiteral("enabled"), rule->isEnabled());
        item.insert(QStringLiteral("config"), QJsonObject::fromVariantMap(rule->configuration()));
        array.append(item);
    }
    return array;
}

Result<void> FaultInjector::loadJson(const QJsonArray& json)
{
    clear();

    for (const QJsonValue& value : json) {
        const QJsonObject item = value.toObject();

        const auto added = addRule(item.value(QStringLiteral("kind")).toString(),
                                   item.value(QStringLiteral("config")).toObject().toVariantMap());
        if (added.hasError()) {
            return added.error();
        }
        if (!item.value(QStringLiteral("enabled")).toBool(true)) {
            setRuleEnabled(added.value(), false);
        }
    }
    return core::success();
}

Result<ConfigSchema> FaultInjector::schemaFor(const QString& kind)
{
    registerBuiltinFaultRules();

    auto created = faultRuleRegistry().create(kind);
    if (created.hasError()) {
        return created.error();
    }
    return created.value()->schema();
}

QStringList FaultInjector::availableKinds()
{
    registerBuiltinFaultRules();

    QStringList kinds;
    for (const QString& key : faultRuleRegistry().keys()) {
        kinds.append(key);
    }
    kinds.sort();
    return kinds;
}

} // namespace hwsim::simulator
