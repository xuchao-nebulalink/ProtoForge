#include "FaultRules.h"

#include <core/Clock.h>

#include <QHash>

using hwsim::core::ConfigField;
using hwsim::core::ConfigSchema;
using hwsim::core::Result;
using hwsim::protocol::ByteFilterContext;
using hwsim::protocol::ByteFilterDecision;

namespace hwsim::simulator {
namespace {

const QHash<FaultDirection, QString>& directionNames()
{
    static const QHash<FaultDirection, QString> names{
        {FaultDirection::Inbound, QStringLiteral("inbound")},
        {FaultDirection::Outbound, QStringLiteral("outbound")},
        {FaultDirection::Both, QStringLiteral("both")},
    };
    return names;
}

const QHash<FaultTrigger, QString>& triggerNames()
{
    static const QHash<FaultTrigger, QString> names{
        {FaultTrigger::Always, QStringLiteral("always")},
        {FaultTrigger::Probability, QStringLiteral("probability")},
        {FaultTrigger::EveryNth, QStringLiteral("every-nth")},
        {FaultTrigger::Manual, QStringLiteral("manual")},
    };
    return names;
}

} // namespace

QString faultDirectionName(FaultDirection direction)
{
    return directionNames().value(direction, QStringLiteral("outbound"));
}

FaultDirection faultDirectionFromName(const QString& name)
{
    for (auto it = directionNames().constBegin(); it != directionNames().constEnd(); ++it) {
        if (it.value() == name) {
            return it.key();
        }
    }
    return FaultDirection::Outbound;
}

QString faultTriggerName(FaultTrigger trigger)
{
    return triggerNames().value(trigger, QStringLiteral("probability"));
}

FaultTrigger faultTriggerFromName(const QString& name)
{
    for (auto it = triggerNames().constBegin(); it != triggerNames().constEnd(); ++it) {
        if (it.value() == name) {
            return it.key();
        }
    }
    return FaultTrigger::Probability;
}

// --- FaultRuleBase ---------------------------------------------------------

ConfigSchema FaultRuleBase::schema() const
{
    ConfigSchema schema(displayName());

    schema.add(ConfigField::enumeration(QStringLiteral("direction"), QStringLiteral("作用方向"),
                                        {QStringLiteral("inbound"), QStringLiteral("outbound"),
                                         QStringLiteral("both")},
                                        QStringLiteral("outbound"))
                   .withLabels({QStringLiteral("接收方向"), QStringLiteral("发送方向"),
                                QStringLiteral("双向")})
                   .inGroup(QStringLiteral("触发")));

    schema.add(ConfigField::enumeration(QStringLiteral("trigger"), QStringLiteral("触发方式"),
                                        {QStringLiteral("always"), QStringLiteral("probability"),
                                         QStringLiteral("every-nth"), QStringLiteral("manual")},
                                        QStringLiteral("probability"))
                   .withLabels({QStringLiteral("每帧"), QStringLiteral("按概率"),
                                QStringLiteral("每 N 帧"), QStringLiteral("手动触发")})
                   .inGroup(QStringLiteral("触发")));

    schema.add(ConfigField::number(QStringLiteral("probability"), QStringLiteral("触发概率"), 1.0)
                   .range(0.0, 1.0)
                   .inGroup(QStringLiteral("触发"))
                   .shownWhen(QStringLiteral("trigger==probability")));

    schema.add(ConfigField::integer(QStringLiteral("everyNth"), QStringLiteral("每 N 帧触发"), 2)
                   .range(1, 100000)
                   .inGroup(QStringLiteral("触发"))
                   .shownWhen(QStringLiteral("trigger==every-nth")));

    schema.merge(extraSchema(), QStringLiteral("参数"));
    return schema;
}

Result<void> FaultRuleBase::configure(const QVariantMap& config)
{
    const ConfigSchema definition = schema();
    if (const auto valid = definition.validate(config); valid.hasError()) {
        return valid;
    }

    config_ = definition.normalise(config);

    direction_ = faultDirectionFromName(text("direction", QStringLiteral("outbound")));
    trigger_ = faultTriggerFromName(text("trigger", QStringLiteral("probability")));
    probability_ = number("probability", 1.0);
    everyNth_ = qMax<qint64>(1, integer("everyNth", 2));
    matchCount_ = 0;

    return core::success();
}

double FaultRuleBase::number(const char* key, double fallback) const
{
    return config_.value(QString::fromLatin1(key), fallback).toDouble();
}

qint64 FaultRuleBase::integer(const char* key, qint64 fallback) const
{
    return config_.value(QString::fromLatin1(key), QVariant::fromValue(fallback)).toLongLong();
}

bool FaultRuleBase::flag(const char* key, bool fallback) const
{
    return config_.value(QString::fromLatin1(key), fallback).toBool();
}

QString FaultRuleBase::text(const char* key, const QString& fallback) const
{
    return config_.value(QString::fromLatin1(key), fallback).toString();
}

bool FaultRuleBase::directionMatches(transport::Direction direction) const
{
    if (direction_ == FaultDirection::Both) {
        return true;
    }
    return direction == transport::Direction::Inbound ? direction_ == FaultDirection::Inbound
                                                      : direction_ == FaultDirection::Outbound;
}

bool FaultRuleBase::shouldFire()
{
    ++matchCount_;

    if (armed_) {
        armed_ = false;
        return true;
    }

    if (trigger_ == FaultTrigger::Always) {
        return true;
    }
    if (trigger_ == FaultTrigger::Manual) {
        return false;  // only arm() fires this one
    }
    if (trigger_ == FaultTrigger::EveryNth) {
        return matchCount_ % everyNth_ == 0;
    }

    std::uniform_real_distribution<double> distribution(0.0, 1.0);
    return distribution(engine_) < probability_;
}

ByteFilterDecision FaultRuleBase::apply(QByteArray& bytes, const ByteFilterContext& context)
{
    if (!enabled_ || !directionMatches(context.direction)) {
        return ByteFilterDecision::pass();
    }

    ++statistics_.evaluations;

    const bool wasManuallyArmed = armed_;
    if (!shouldFire()) {
        return ByteFilterDecision::pass();
    }

    const ByteFilterDecision decision = applyFault(bytes, context);

    // Count only what actually happened. A rule may decline once it sees the
    // buffer (a checksum fault on a frame too short to carry one), and counting
    // that would overstate the injected fault rate and raise the activation
    // signal for a frame that went out untouched.
    const bool injected = !decision.deliver || decision.delayMs > 0 || !decision.note.isEmpty();
    if (injected) {
        ++statistics_.activations;
        statistics_.lastActivationMs = core::wallClockMs();
    } else if (wasManuallyArmed) {
        // shouldFire() consumed the arm, but the rule then declined this
        // particular buffer. Put the arm back so an explicit operator trigger
        // fires on the next eligible frame instead of vanishing.
        armed_ = true;
    }

    return decision;
}

// --- PacketLossFault -------------------------------------------------------

ByteFilterDecision PacketLossFault::applyFault(QByteArray& bytes, const ByteFilterContext& context)
{
    Q_UNUSED(bytes)
    Q_UNUSED(context)
    return ByteFilterDecision::drop(QStringLiteral("丢包"));
}

// --- TimeoutFault ----------------------------------------------------------

ByteFilterDecision TimeoutFault::applyFault(QByteArray& bytes, const ByteFilterContext& context)
{
    Q_UNUSED(bytes)
    Q_UNUSED(context)
    return ByteFilterDecision::drop(QStringLiteral("响应被抑制，对端将超时"));
}

// --- LatencyFault ----------------------------------------------------------

ByteFilterDecision LatencyFault::applyFault(QByteArray& bytes, const ByteFilterContext& context)
{
    Q_UNUSED(bytes)
    Q_UNUSED(context)

    const auto minimum = integer("minDelayMs", 100);
    const auto maximum = qMax(minimum, integer("maxDelayMs", 100));

    std::uniform_int_distribution<qint64> distribution(minimum, maximum);
    const qint64 delay = distribution(randomEngine());

    if (delay <= 0) {
        // Configured to add nothing, so nothing was injected. Returning a note
        // here would count an activation and annotate the packet view for a
        // frame that went out unchanged.
        return ByteFilterDecision::pass();
    }

    return ByteFilterDecision::delay(static_cast<int>(delay),
                                     QStringLiteral("延迟 %1 ms").arg(delay));
}

ConfigSchema LatencyFault::extraSchema() const
{
    ConfigSchema schema;
    schema.add(ConfigField::duration(QStringLiteral("minDelayMs"), QStringLiteral("最小延迟"), 100)
                   .range(0, 600000));
    schema.add(ConfigField::duration(QStringLiteral("maxDelayMs"), QStringLiteral("最大延迟"), 100)
                   .range(0, 600000));
    return schema;
}

// --- ChecksumErrorFault ----------------------------------------------------

ByteFilterDecision ChecksumErrorFault::applyFault(QByteArray& bytes, const ByteFilterContext& context)
{
    Q_UNUSED(context)

    const auto width = static_cast<qsizetype>(qMax<qint64>(1, integer("checksumBytes", 2)));
    if (bytes.size() < width) {
        // Declining, not injecting: an empty note keeps this out of the
        // activation count and out of the packet view annotation.
        return ByteFilterDecision::pass();
    }

    // XOR the trailing checksum so the frame stays structurally valid and the
    // peer is forced to reject it on the checksum alone.
    const auto mask = static_cast<char>(integer("xorMask", 0xFF) & 0xFF);
    for (qsizetype index = bytes.size() - width; index < bytes.size(); ++index) {
        bytes[index] = static_cast<char>(bytes.at(index) ^ mask);
    }

    return ByteFilterDecision{true, 0, QStringLiteral("校验字段已篡改 (%1 字节)").arg(width)};
}

ConfigSchema ChecksumErrorFault::extraSchema() const
{
    ConfigSchema schema;
    schema.add(ConfigField::integer(QStringLiteral("checksumBytes"), QStringLiteral("校验字节数"), 2)
                   .range(1, 8)
                   .describedAs(QStringLiteral("Modbus CRC 为 2，单字节和校验为 1")));
    schema.add(ConfigField::integer(QStringLiteral("xorMask"), QStringLiteral("异或掩码"), 0xFF)
                   .range(1, 255)
                   .asAdvanced());
    return schema;
}

// --- BitFlipFault ----------------------------------------------------------

ByteFilterDecision BitFlipFault::applyFault(QByteArray& bytes, const ByteFilterContext& context)
{
    Q_UNUSED(context)

    if (bytes.isEmpty()) {
        return ByteFilterDecision::pass();
    }

    const auto flips = qMax<qint64>(1, integer("bitCount", 1));
    std::uniform_int_distribution<qsizetype> byteDistribution(0, bytes.size() - 1);
    std::uniform_int_distribution<int> bitDistribution(0, 7);

    QStringList positions;
    for (qint64 i = 0; i < flips; ++i) {
        const qsizetype index = byteDistribution(randomEngine());
        const int bit = bitDistribution(randomEngine());
        bytes[index] = static_cast<char>(bytes.at(index) ^ (1 << bit));
        positions.append(QStringLiteral("%1.%2").arg(index).arg(bit));
    }

    return ByteFilterDecision{true, 0,
                              QStringLiteral("翻转 bit %1").arg(positions.join(QLatin1Char(',')))};
}

ConfigSchema BitFlipFault::extraSchema() const
{
    ConfigSchema schema;
    schema.add(ConfigField::integer(QStringLiteral("bitCount"), QStringLiteral("翻转位数"), 1)
                   .range(1, 64));
    return schema;
}

// --- TruncationFault -------------------------------------------------------

ByteFilterDecision TruncationFault::applyFault(QByteArray& bytes, const ByteFilterContext& context)
{
    Q_UNUSED(context)

    const auto keepAtLeast = static_cast<qsizetype>(qMax<qint64>(0, integer("keepBytes", 0)));
    const auto cut = static_cast<qsizetype>(qMax<qint64>(1, integer("cutBytes", 1)));

    qsizetype newSize = bytes.size() - cut;
    newSize = qMax(newSize, keepAtLeast);
    newSize = qMax<qsizetype>(newSize, 0);

    if (newSize >= bytes.size()) {
        return ByteFilterDecision::pass();
    }

    const qsizetype removed = bytes.size() - newSize;
    bytes.truncate(newSize);
    return ByteFilterDecision{true, 0, QStringLiteral("截断 %1 字节").arg(removed)};
}

ConfigSchema TruncationFault::extraSchema() const
{
    ConfigSchema schema;
    schema.add(ConfigField::integer(QStringLiteral("cutBytes"), QStringLiteral("截掉字节数"), 1)
                   .range(1, 1024));
    schema.add(ConfigField::integer(QStringLiteral("keepBytes"), QStringLiteral("至少保留"), 0)
                   .range(0, 1024)
                   .asAdvanced());
    return schema;
}

// --- GarbageFault ----------------------------------------------------------

ByteFilterDecision GarbageFault::applyFault(QByteArray& bytes, const ByteFilterContext& context)
{
    Q_UNUSED(context)

    const auto count = static_cast<qsizetype>(qMax<qint64>(1, integer("byteCount", 2)));
    std::uniform_int_distribution<int> distribution(0, 255);

    QByteArray junk(count, '\0');
    for (qsizetype index = 0; index < count; ++index) {
        junk[index] = static_cast<char>(distribution(randomEngine()));
    }

    if (flag("append", false)) {
        bytes.append(junk);
    } else {
        bytes.prepend(junk);
    }

    return ByteFilterDecision{true, 0, QStringLiteral("插入 %1 字节乱码").arg(count)};
}

ConfigSchema GarbageFault::extraSchema() const
{
    ConfigSchema schema;
    schema.add(ConfigField::integer(QStringLiteral("byteCount"), QStringLiteral("乱码字节数"), 2)
                   .range(1, 256));
    schema.add(ConfigField::boolean(QStringLiteral("append"), QStringLiteral("追加到帧尾"), false)
                   .describedAs(QStringLiteral("默认插在帧头，强制对端重新同步")));
    return schema;
}

// --- DisconnectFault -------------------------------------------------------

ByteFilterDecision DisconnectFault::applyFault(QByteArray& bytes, const ByteFilterContext& context)
{
    Q_UNUSED(bytes)

    if (request_) {
        request_(context.linkId, QStringLiteral("fault injection: %1").arg(name()));
    }
    return ByteFilterDecision::drop(QStringLiteral("主动断开链路"));
}

// --- Registry --------------------------------------------------------------

core::Registry<QString, FaultRuleBase>& faultRuleRegistry()
{
    static core::Registry<QString, FaultRuleBase> registry;
    return registry;
}

void registerBuiltinFaultRules()
{
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;

    auto& registry = faultRuleRegistry();
    registry.addType<PacketLossFault>(QStringLiteral("packet-loss"), QStringLiteral("丢包"));
    registry.addType<TimeoutFault>(QStringLiteral("timeout"), QStringLiteral("响应超时"));
    registry.addType<LatencyFault>(QStringLiteral("latency"), QStringLiteral("附加时延"));
    registry.addType<ChecksumErrorFault>(QStringLiteral("checksum-error"), QStringLiteral("校验错误"));
    registry.addType<BitFlipFault>(QStringLiteral("bit-flip"), QStringLiteral("误码"));
    registry.addType<TruncationFault>(QStringLiteral("truncation"), QStringLiteral("帧截断"));
    registry.addType<GarbageFault>(QStringLiteral("garbage"), QStringLiteral("插入乱码"));
    registry.addType<DisconnectFault>(QStringLiteral("disconnect"), QStringLiteral("断开链路"));
}

} // namespace hwsim::simulator
