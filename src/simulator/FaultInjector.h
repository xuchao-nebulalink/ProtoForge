#pragma once

#include "FaultRules.h"

#include <protocol/ProtocolSession.h>

#include <QJsonArray>
#include <QObject>
#include <QVector>

#include <memory>

namespace hwsim::simulator {

struct HWSIM_SIMULATOR_API FaultRuleInfo {
    QString id;
    QString kind;
    QString displayName;
    bool enabled{true};
    QVariantMap configuration;
    FaultStatistics statistics;
};

/// Owns a device's fault rules and installs them on its protocol sessions.
///
/// The injector itself is the single byte filter each session sees; the
/// individual rules live behind it. That indirection buys two things: a global
/// on/off switch that does not disturb each rule's own enabled flag, and
/// attach/detach that stays valid as rules are added and removed at run time
/// from the UI or a script.
class HWSIM_SIMULATOR_API FaultInjector : public QObject {
    Q_OBJECT

public:
    explicit FaultInjector(QObject* parent = nullptr);
    ~FaultInjector() override;

    [[nodiscard]] core::Result<QString> addRule(const QString& kind,
                                                const QVariantMap& configuration = {});
    bool removeRule(const QString& id);
    bool setRuleEnabled(const QString& id, bool enabled);

    /// Arms a Manual-trigger rule for exactly one activation.
    bool armRule(const QString& id);

    [[nodiscard]] core::Result<void> reconfigureRule(const QString& id,
                                                     const QVariantMap& configuration);
    void clear();

    [[nodiscard]] QVector<FaultRuleInfo> rules() const;
    [[nodiscard]] bool isEmpty() const;
    void resetStatistics();

    void setGloballyEnabled(bool enabled);
    [[nodiscard]] bool isGloballyEnabled() const;

    /// Invoked by DisconnectFault, with the id of the link the fault fired on.
    /// The device runtime wires this to a link lookup.
    void setDisconnectRequest(FaultRuleBase::DisconnectRequest request);

    /// Installs the injector on both the inbound and the outbound chain. Each
    /// rule decides for itself which direction it cares about.
    void attachTo(protocol::ProtocolSession& session);
    void detachFrom(protocol::ProtocolSession& session);

    [[nodiscard]] QJsonArray toJson() const;
    [[nodiscard]] core::Result<void> loadJson(const QJsonArray& json);

    /// Schema for a rule kind, so the UI can build its editor.
    [[nodiscard]] static core::Result<core::ConfigSchema> schemaFor(const QString& kind);
    [[nodiscard]] static QStringList availableKinds();

signals:
    void faultActivated(const QString& ruleId, const QString& note);
    void rulesChanged();

private:
    /// The one filter a session sees; it fans out to the rules.
    class Dispatcher final : public protocol::IByteFilter {
    public:
        [[nodiscard]] QString name() const override { return QStringLiteral("fault-injector"); }
        [[nodiscard]] bool isEnabled() const override { return enabled; }
        [[nodiscard]] protocol::ByteFilterDecision apply(
            QByteArray& bytes, const protocol::ByteFilterContext& context) override;

        std::vector<FaultRulePtr> rules;
        bool enabled{true};
        std::function<void(QString, QString)> onActivated;
    };

    [[nodiscard]] FaultRulePtr findRule(const QString& id) const;

    std::shared_ptr<Dispatcher> dispatcher_;

    /// Kept so that rules added after the runtime wired itself up still get it.
    FaultRuleBase::DisconnectRequest disconnectRequest_;

    qint64 nextRuleId_{1};
};

} // namespace hwsim::simulator
