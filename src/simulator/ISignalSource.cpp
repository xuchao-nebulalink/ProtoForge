#include "ISignalSource.h"

#include "SignalSources.h"

namespace hwsim::simulator {

core::Result<void> ISignalSource::configure(const QVariantMap& config)
{
    const core::ConfigSchema definition = schema();
    if (const auto valid = definition.validate(config); valid.hasError()) {
        return valid;
    }
    storeConfiguration(definition.normalise(config));
    return core::success();
}

double ISignalSource::number(const char* key, double fallback) const
{
    return config_.value(QString::fromLatin1(key), fallback).toDouble();
}

qint64 ISignalSource::integer(const char* key, qint64 fallback) const
{
    return config_.value(QString::fromLatin1(key), QVariant::fromValue(fallback)).toLongLong();
}

bool ISignalSource::flag(const char* key, bool fallback) const
{
    return config_.value(QString::fromLatin1(key), fallback).toBool();
}

QString ISignalSource::text(const char* key, const QString& fallback) const
{
    return config_.value(QString::fromLatin1(key), fallback).toString();
}

core::Registry<QString, ISignalSource>& signalSourceRegistry()
{
    static core::Registry<QString, ISignalSource> registry;
    return registry;
}

void registerBuiltinSignalSources()
{
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;

    auto& registry = signalSourceRegistry();
    registry.addType<ConstantSource>(QStringLiteral("constant"), QStringLiteral("恒定值"));
    registry.addType<SineSource>(QStringLiteral("sine"), QStringLiteral("正弦波"));
    registry.addType<SquareSource>(QStringLiteral("square"), QStringLiteral("方波"));
    registry.addType<TriangleSource>(QStringLiteral("triangle"), QStringLiteral("三角波"));
    registry.addType<SawtoothSource>(QStringLiteral("sawtooth"), QStringLiteral("锯齿波"));
    registry.addType<StepSource>(QStringLiteral("step"), QStringLiteral("阶跃"));
    registry.addType<RampSource>(QStringLiteral("ramp"), QStringLiteral("斜坡"));
    registry.addType<NoiseSource>(QStringLiteral("noise"), QStringLiteral("随机噪声"));
    registry.addType<RandomWalkSource>(QStringLiteral("random-walk"), QStringLiteral("随机游走"));
    registry.addType<ReplaySource>(QStringLiteral("replay"), QStringLiteral("CSV 回放"));
}

} // namespace hwsim::simulator
