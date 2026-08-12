#pragma once

#include "SimulatorGlobal.h"

#include <core/ConfigSchema.h>
#include <core/Registry.h>
#include <protocol/IByteFilter.h>

#include <functional>
#include <memory>
#include <random>

namespace hwsim::simulator {

enum class FaultDirection {
    Inbound,
    Outbound,
    Both,
};

enum class FaultTrigger {
    /// Fires on every matching buffer.
    Always,
    /// Fires with the configured probability.
    Probability,
    /// Fires on every Nth matching buffer.
    EveryNth,
    /// Fires only when armed from the UI or a script.
    Manual,
};

struct HWSIM_SIMULATOR_API FaultStatistics {
    quint64 evaluations{0};
    quint64 activations{0};
    qint64 lastActivationMs{0};

    void reset() { *this = FaultStatistics{}; }
};

/// Base class for fault rules.
///
/// A fault rule is a byte filter, which means it operates on finished frames
/// after encoding and before transmission. That placement is deliberate: it
/// keeps every protocol plugin free of fault-injection branches, and it makes
/// faults work identically for any protocol over any transport.
///
/// This class owns the parts every rule shares - enable flag, direction
/// filtering, firing policy, statistics - so a concrete rule only implements
/// what it does to the bytes.
class HWSIM_SIMULATOR_API FaultRuleBase : public protocol::IByteFilter {
public:
    [[nodiscard]] QString name() const override { return id_; }
    void setId(QString id) { id_ = std::move(id); }

    /// Rule kind as registered, e.g. "checksum-error".
    [[nodiscard]] virtual QString kind() const = 0;
    [[nodiscard]] virtual QString displayName() const = 0;

    [[nodiscard]] bool isEnabled() const override { return enabled_; }
    void setEnabled(bool enabled) { enabled_ = enabled; }

    /// Arms a one-shot activation for Manual trigger mode.
    void arm() { armed_ = true; }
    [[nodiscard]] bool isArmed() const noexcept { return armed_; }

    [[nodiscard]] FaultStatistics statistics() const noexcept { return statistics_; }
    void resetStatistics() { statistics_.reset(); }

    /// Full schema: the shared trigger fields plus whatever the rule adds.
    [[nodiscard]] core::ConfigSchema schema() const;
    [[nodiscard]] virtual core::Result<void> configure(const QVariantMap& config);
    [[nodiscard]] const QVariantMap& configuration() const noexcept { return config_; }

    /// Only DisconnectFault uses this; the default is a no-op so the injector
    /// can hand it to every rule without asking what kind it is.
    ///
    /// The link id is passed at trigger time rather than bound up front,
    /// because a device can carry several links and the fault should drop the
    /// one the frame was travelling on.
    using DisconnectRequest = std::function<void(transport::LinkId, QString)>;

    virtual void setDisconnectRequest(DisconnectRequest request) { Q_UNUSED(request) }

    /// Template method: shared gating, then the rule's own effect.
    [[nodiscard]] protocol::ByteFilterDecision apply(QByteArray& bytes,
                                                     const protocol::ByteFilterContext& context) final;

protected:
    [[nodiscard]] virtual protocol::ByteFilterDecision applyFault(
        QByteArray& bytes, const protocol::ByteFilterContext& context) = 0;

    /// Rule-specific configuration fields, merged into schema().
    [[nodiscard]] virtual core::ConfigSchema extraSchema() const { return {}; }

    [[nodiscard]] double number(const char* key, double fallback) const;
    [[nodiscard]] qint64 integer(const char* key, qint64 fallback) const;
    [[nodiscard]] bool flag(const char* key, bool fallback) const;
    [[nodiscard]] QString text(const char* key, const QString& fallback = {}) const;

    [[nodiscard]] std::mt19937_64& randomEngine() { return engine_; }

private:
    [[nodiscard]] bool directionMatches(transport::Direction direction) const;
    [[nodiscard]] bool shouldFire();

    QString id_;
    bool enabled_{true};
    bool armed_{false};
    FaultDirection direction_{FaultDirection::Outbound};
    FaultTrigger trigger_{FaultTrigger::Probability};
    double probability_{1.0};
    qint64 everyNth_{2};
    qint64 matchCount_{0};

    QVariantMap config_;
    FaultStatistics statistics_;
    std::mt19937_64 engine_{std::random_device{}()};
};

using FaultRulePtr = std::shared_ptr<FaultRuleBase>;

// --- Concrete rules --------------------------------------------------------

/// Discards the buffer. On the inbound side the request never arrives; on the
/// outbound side the master sees a timeout.
class HWSIM_SIMULATOR_API PacketLossFault final : public FaultRuleBase {
public:
    [[nodiscard]] QString kind() const override { return QStringLiteral("packet-loss"); }
    [[nodiscard]] QString displayName() const override { return QStringLiteral("丢包"); }

protected:
    [[nodiscard]] protocol::ByteFilterDecision applyFault(
        QByteArray& bytes, const protocol::ByteFilterContext& context) override;
};

/// Withholds the reply so the master's request times out. Distinct from packet
/// loss only in intent and in what the annotation says, which matters when
/// reading a captured session.
class HWSIM_SIMULATOR_API TimeoutFault final : public FaultRuleBase {
public:
    [[nodiscard]] QString kind() const override { return QStringLiteral("timeout"); }
    [[nodiscard]] QString displayName() const override { return QStringLiteral("响应超时"); }

protected:
    [[nodiscard]] protocol::ByteFilterDecision applyFault(
        QByteArray& bytes, const protocol::ByteFilterContext& context) override;
};

/// Delays delivery, for exercising a master's timeout margin rather than
/// blowing past it.
class HWSIM_SIMULATOR_API LatencyFault final : public FaultRuleBase {
public:
    [[nodiscard]] QString kind() const override { return QStringLiteral("latency"); }
    [[nodiscard]] QString displayName() const override { return QStringLiteral("附加时延"); }

protected:
    [[nodiscard]] protocol::ByteFilterDecision applyFault(
        QByteArray& bytes, const protocol::ByteFilterContext& context) override;
    [[nodiscard]] core::ConfigSchema extraSchema() const override;
};

/// Corrupts the trailing checksum bytes, leaving the rest of the frame intact.
/// The peer therefore has to detect the error through its own CRC check, which
/// is the behaviour under test.
class HWSIM_SIMULATOR_API ChecksumErrorFault final : public FaultRuleBase {
public:
    [[nodiscard]] QString kind() const override { return QStringLiteral("checksum-error"); }
    [[nodiscard]] QString displayName() const override { return QStringLiteral("校验错误"); }

protected:
    [[nodiscard]] protocol::ByteFilterDecision applyFault(
        QByteArray& bytes, const protocol::ByteFilterContext& context) override;
    [[nodiscard]] core::ConfigSchema extraSchema() const override;
};

/// Flips random bits anywhere in the frame, modelling line noise. Usually
/// detected by the checksum, which is the point.
class HWSIM_SIMULATOR_API BitFlipFault final : public FaultRuleBase {
public:
    [[nodiscard]] QString kind() const override { return QStringLiteral("bit-flip"); }
    [[nodiscard]] QString displayName() const override { return QStringLiteral("误码"); }

protected:
    [[nodiscard]] protocol::ByteFilterDecision applyFault(
        QByteArray& bytes, const protocol::ByteFilterContext& context) override;
    [[nodiscard]] core::ConfigSchema extraSchema() const override;
};

/// Cuts the frame short, which exercises the peer's reassembly and resync path
/// rather than its checksum path.
class HWSIM_SIMULATOR_API TruncationFault final : public FaultRuleBase {
public:
    [[nodiscard]] QString kind() const override { return QStringLiteral("truncation"); }
    [[nodiscard]] QString displayName() const override { return QStringLiteral("帧截断"); }

protected:
    [[nodiscard]] protocol::ByteFilterDecision applyFault(
        QByteArray& bytes, const protocol::ByteFilterContext& context) override;
    [[nodiscard]] core::ConfigSchema extraSchema() const override;
};

/// Prepends junk bytes, so the peer must resynchronise before it finds the
/// real frame.
class HWSIM_SIMULATOR_API GarbageFault final : public FaultRuleBase {
public:
    [[nodiscard]] QString kind() const override { return QStringLiteral("garbage"); }
    [[nodiscard]] QString displayName() const override { return QStringLiteral("插入乱码"); }

protected:
    [[nodiscard]] protocol::ByteFilterDecision applyFault(
        QByteArray& bytes, const protocol::ByteFilterContext& context) override;
    [[nodiscard]] core::ConfigSchema extraSchema() const override;
};

/// Drops the link entirely, through a callback the injector supplies.
class HWSIM_SIMULATOR_API DisconnectFault final : public FaultRuleBase {
public:
    [[nodiscard]] QString kind() const override { return QStringLiteral("disconnect"); }
    [[nodiscard]] QString displayName() const override { return QStringLiteral("断开链路"); }

    void setDisconnectRequest(DisconnectRequest request) override
    {
        request_ = std::move(request);
    }

protected:
    [[nodiscard]] protocol::ByteFilterDecision applyFault(
        QByteArray& bytes, const protocol::ByteFilterContext& context) override;

private:
    DisconnectRequest request_;
};

[[nodiscard]] HWSIM_SIMULATOR_API core::Registry<QString, FaultRuleBase>& faultRuleRegistry();
HWSIM_SIMULATOR_API void registerBuiltinFaultRules();

[[nodiscard]] HWSIM_SIMULATOR_API QString faultDirectionName(FaultDirection direction);
[[nodiscard]] HWSIM_SIMULATOR_API FaultDirection faultDirectionFromName(const QString& name);
[[nodiscard]] HWSIM_SIMULATOR_API QString faultTriggerName(FaultTrigger trigger);
[[nodiscard]] HWSIM_SIMULATOR_API FaultTrigger faultTriggerFromName(const QString& name);

} // namespace hwsim::simulator
