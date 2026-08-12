#include "SignalSources.h"

#include <core/Clock.h>

#include <QFile>
#include <QTextStream>

#include <cmath>

using hwsim::core::ConfigField;
using hwsim::core::ConfigSchema;
using hwsim::core::ErrorCode;
using hwsim::core::makeError;
using hwsim::core::Result;

namespace hwsim::simulator {
namespace {

constexpr double kTwoPi = 6.283185307179586;

/// Position within one period, in [0, 1).
double phaseOf(qint64 elapsedMs, double frequencyHz, double phaseDegrees)
{
    if (frequencyHz <= 0.0) {
        return 0.0;
    }
    const double seconds = static_cast<double>(elapsedMs) / 1000.0;
    double phase = seconds * frequencyHz + phaseDegrees / 360.0;
    phase -= std::floor(phase);
    return phase;
}

ConfigSchema waveformSchema(const QString& title)
{
    ConfigSchema schema(title);
    schema.add(ConfigField::number(QStringLiteral("amplitude"), QStringLiteral("幅值"), 1.0));
    schema.add(ConfigField::number(QStringLiteral("offset"), QStringLiteral("直流偏置"), 0.0));
    schema.add(ConfigField::number(QStringLiteral("frequencyHz"), QStringLiteral("频率"), 1.0)
                   .range(0.0, 10000.0)
                   .withUnit(QStringLiteral("Hz")));
    schema.add(ConfigField::number(QStringLiteral("phaseDeg"), QStringLiteral("相位"), 0.0)
                   .range(-360.0, 360.0)
                   .withUnit(QStringLiteral("°")));
    return schema;
}

} // namespace

// --- Constant --------------------------------------------------------------

double ConstantSource::sample(qint64 elapsedMs)
{
    Q_UNUSED(elapsedMs)
    return number("value", 0.0);
}

ConfigSchema ConstantSource::schema() const
{
    ConfigSchema schema(QStringLiteral("恒定值"));
    schema.add(ConfigField::number(QStringLiteral("value"), QStringLiteral("数值"), 0.0));
    return schema;
}

// --- Sine ------------------------------------------------------------------

double SineSource::sample(qint64 elapsedMs)
{
    const double phase = phaseOf(elapsedMs, number("frequencyHz", 1.0), number("phaseDeg", 0.0));
    return number("offset", 0.0) + number("amplitude", 1.0) * std::sin(kTwoPi * phase);
}

ConfigSchema SineSource::schema() const
{
    return waveformSchema(QStringLiteral("正弦波"));
}

// --- Square ----------------------------------------------------------------

double SquareSource::sample(qint64 elapsedMs)
{
    const double phase = phaseOf(elapsedMs, number("frequencyHz", 1.0), number("phaseDeg", 0.0));
    const double duty = number("dutyCycle", 0.5);
    const double amplitude = number("amplitude", 1.0);
    return number("offset", 0.0) + (phase < duty ? amplitude : -amplitude);
}

ConfigSchema SquareSource::schema() const
{
    ConfigSchema schema = waveformSchema(QStringLiteral("方波"));
    schema.add(ConfigField::number(QStringLiteral("dutyCycle"), QStringLiteral("占空比"), 0.5)
                   .range(0.0, 1.0));
    return schema;
}

// --- Triangle --------------------------------------------------------------

double TriangleSource::sample(qint64 elapsedMs)
{
    const double phase = phaseOf(elapsedMs, number("frequencyHz", 1.0), number("phaseDeg", 0.0));
    const double normalised = phase < 0.5 ? (4.0 * phase - 1.0) : (3.0 - 4.0 * phase);
    return number("offset", 0.0) + number("amplitude", 1.0) * normalised;
}

ConfigSchema TriangleSource::schema() const
{
    return waveformSchema(QStringLiteral("三角波"));
}

// --- Sawtooth --------------------------------------------------------------

double SawtoothSource::sample(qint64 elapsedMs)
{
    const double phase = phaseOf(elapsedMs, number("frequencyHz", 1.0), number("phaseDeg", 0.0));
    return number("offset", 0.0) + number("amplitude", 1.0) * (2.0 * phase - 1.0);
}

ConfigSchema SawtoothSource::schema() const
{
    return waveformSchema(QStringLiteral("锯齿波"));
}

// --- Step ------------------------------------------------------------------

double StepSource::sample(qint64 elapsedMs)
{
    const qint64 stepAt = integer("stepAtMs", 5000);
    const double before = number("initialValue", 0.0);
    const double after = number("finalValue", 1.0);

    if (stepAt <= 0) {
        return after;
    }

    if (!flag("repeat", false)) {
        return elapsedMs < stepAt ? before : after;
    }

    // Repeating: alternate between the two levels every stepAtMs.
    const qint64 halfPeriods = elapsedMs / stepAt;
    return (halfPeriods % 2 == 0) ? before : after;
}

ConfigSchema StepSource::schema() const
{
    ConfigSchema schema(QStringLiteral("阶跃"));
    schema.add(ConfigField::number(QStringLiteral("initialValue"), QStringLiteral("初始值"), 0.0));
    schema.add(ConfigField::number(QStringLiteral("finalValue"), QStringLiteral("阶跃后"), 1.0));
    schema.add(ConfigField::duration(QStringLiteral("stepAtMs"), QStringLiteral("阶跃时刻"), 5000)
                   .range(0, 3600000));
    schema.add(ConfigField::boolean(QStringLiteral("repeat"), QStringLiteral("往复切换"), false)
                   .describedAs(QStringLiteral("开启后每隔一个阶跃时刻在两个电平间来回切换")));
    return schema;
}

// --- Ramp ------------------------------------------------------------------

double RampSource::sample(qint64 elapsedMs)
{
    const double start = number("startValue", 0.0);
    const double end = number("endValue", 100.0);
    const qint64 duration = integer("durationMs", 10000);

    if (duration <= 0) {
        return end;
    }

    qint64 position = elapsedMs;
    if (flag("repeat", true)) {
        position = elapsedMs % duration;
    } else if (position > duration) {
        position = duration;
    }

    const double progress = static_cast<double>(position) / static_cast<double>(duration);
    return start + (end - start) * progress;
}

ConfigSchema RampSource::schema() const
{
    ConfigSchema schema(QStringLiteral("斜坡"));
    schema.add(ConfigField::number(QStringLiteral("startValue"), QStringLiteral("起始值"), 0.0));
    schema.add(ConfigField::number(QStringLiteral("endValue"), QStringLiteral("终止值"), 100.0));
    schema.add(ConfigField::duration(QStringLiteral("durationMs"), QStringLiteral("爬升时长"), 10000)
                   .range(1, 3600000));
    schema.add(ConfigField::boolean(QStringLiteral("repeat"), QStringLiteral("循环"), true));
    return schema;
}

// --- Noise -----------------------------------------------------------------

NoiseSource::NoiseSource() : engine_(std::random_device{}()) {}

double NoiseSource::sample(qint64 elapsedMs)
{
    Q_UNUSED(elapsedMs)

    const double amplitude = number("amplitude", 1.0);
    const double centre = number("centre", 0.0);

    if (text("distribution", QStringLiteral("uniform")) == QStringLiteral("gaussian")) {
        std::normal_distribution<double> distribution(centre, amplitude);
        return distribution(engine_);
    }

    std::uniform_real_distribution<double> distribution(centre - amplitude, centre + amplitude);
    return distribution(engine_);
}

ConfigSchema NoiseSource::schema() const
{
    ConfigSchema schema(QStringLiteral("随机噪声"));
    schema.add(ConfigField::enumeration(QStringLiteral("distribution"), QStringLiteral("分布"),
                                        {QStringLiteral("uniform"), QStringLiteral("gaussian")},
                                        QStringLiteral("uniform"))
                   .withLabels({QStringLiteral("均匀分布"), QStringLiteral("高斯分布")}));
    schema.add(ConfigField::number(QStringLiteral("amplitude"), QStringLiteral("幅度"), 1.0)
                   .describedAs(QStringLiteral("均匀分布为半宽，高斯分布为标准差")));
    schema.add(ConfigField::number(QStringLiteral("centre"), QStringLiteral("中心值"), 0.0));
    schema.add(ConfigField::integer(QStringLiteral("seed"), QStringLiteral("随机种子"), 0)
                   .describedAs(QStringLiteral("0 表示每次运行都不同；非 0 用于可复现的回归测试"))
                   .asAdvanced());
    return schema;
}

Result<void> NoiseSource::configure(const QVariantMap& config)
{
    if (const auto base = ISignalSource::configure(config); base.hasError()) {
        return base;
    }
    seed_ = static_cast<quint64>(integer("seed", 0));
    reset();
    return core::success();
}

void NoiseSource::reset()
{
    // A fixed seed makes a fault-injection regression test deterministic.
    if (seed_ != 0) {
        engine_.seed(seed_);
    }
}

// --- Random walk -----------------------------------------------------------

RandomWalkSource::RandomWalkSource() : engine_(std::random_device{}()) {}

double RandomWalkSource::sample(qint64 elapsedMs)
{
    if (!started_) {
        current_ = number("startValue", 0.0);
        started_ = true;
        lastSampleMs_ = elapsedMs;
        return current_;
    }

    // Scale the step by how much time actually passed, so the drift rate does
    // not depend on the engine's tick interval.
    const qint64 delta = qMax<qint64>(elapsedMs - lastSampleMs_, 0);
    lastSampleMs_ = elapsedMs;

    const double stepPerSecond = number("stepPerSecond", 1.0);
    const double scale = stepPerSecond * static_cast<double>(delta) / 1000.0;

    // Two samples inside the same millisecond give delta == 0, and a normal
    // distribution requires a positive sigma.
    if (scale <= 0.0) {
        return current_;
    }

    std::normal_distribution<double> distribution(0.0, scale);
    current_ += distribution(engine_);

    current_ = qBound(number("minimum", -1e9), current_, number("maximum", 1e9));
    return current_;
}

ConfigSchema RandomWalkSource::schema() const
{
    ConfigSchema schema(QStringLiteral("随机游走"));
    schema.add(ConfigField::number(QStringLiteral("startValue"), QStringLiteral("起始值"), 0.0));
    // Lower bound of zero: a negative drift rate is not a mirrored walk, it is
    // an invalid sigma that would freeze the signal with no diagnostic.
    schema.add(ConfigField::number(QStringLiteral("stepPerSecond"), QStringLiteral("每秒漂移"), 1.0)
                   .range(0.0, 1e9));
    schema.add(ConfigField::number(QStringLiteral("minimum"), QStringLiteral("下限"), -1000.0));
    schema.add(ConfigField::number(QStringLiteral("maximum"), QStringLiteral("上限"), 1000.0));
    schema.add(ConfigField::integer(QStringLiteral("seed"), QStringLiteral("随机种子"), 0).asAdvanced());
    return schema;
}

Result<void> RandomWalkSource::configure(const QVariantMap& config)
{
    if (const auto base = ISignalSource::configure(config); base.hasError()) {
        return base;
    }
    if (const auto seed = static_cast<quint64>(integer("seed", 0)); seed != 0) {
        engine_.seed(seed);
    }
    reset();
    return core::success();
}

void RandomWalkSource::reset()
{
    started_ = false;
    lastSampleMs_ = -1;
    current_ = number("startValue", 0.0);
}

// --- Replay ----------------------------------------------------------------

double ReplaySource::sample(qint64 elapsedMs)
{
    if (samples_.isEmpty()) {
        return 0.0;
    }

    qint64 index = intervalMs_ > 0 ? elapsedMs / intervalMs_ : 0;
    if (loop_) {
        index %= samples_.size();
    } else {
        index = qMin<qint64>(index, samples_.size() - 1);
    }
    return samples_.at(static_cast<qsizetype>(index));
}

ConfigSchema ReplaySource::schema() const
{
    ConfigSchema schema(QStringLiteral("CSV 回放"));
    schema.add(ConfigField::filePath(QStringLiteral("path"), QStringLiteral("CSV 文件")));
    schema.add(ConfigField::integer(QStringLiteral("column"), QStringLiteral("列索引"), 0)
                   .range(0, 1024));
    schema.add(ConfigField::boolean(QStringLiteral("hasHeader"), QStringLiteral("首行为表头"), true));
    schema.add(ConfigField::duration(QStringLiteral("intervalMs"), QStringLiteral("采样间隔"), 100)
                   .range(1, 3600000));
    schema.add(ConfigField::boolean(QStringLiteral("loop"), QStringLiteral("循环播放"), true));
    return schema;
}

Result<void> ReplaySource::configure(const QVariantMap& config)
{
    if (const auto base = ISignalSource::configure(config); base.hasError()) {
        return base;
    }

    intervalMs_ = static_cast<int>(integer("intervalMs", 100));
    loop_ = flag("loop", true);

    return load(text("path"), static_cast<int>(integer("column", 0)), flag("hasHeader", true));
}

void ReplaySource::reset()
{
    // Sample position is derived from elapsed time, so there is no cursor to rewind.
}

Result<void> ReplaySource::load(const QString& path, int column, bool hasHeader)
{
    samples_.clear();

    if (path.isEmpty()) {
        return makeError(ErrorCode::ConfigInvalid, QStringLiteral("replay source needs a file path"));
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return makeError(ErrorCode::IoError, file.errorString(), path);
    }

    QTextStream stream(&file);
    bool first = true;

    while (!stream.atEnd()) {
        const QString line = stream.readLine().trimmed();
        if (line.isEmpty()) {
            continue;
        }
        if (first && hasHeader) {
            first = false;
            continue;
        }
        first = false;

        const QStringList fields = line.split(QLatin1Char(','));
        if (column >= fields.size()) {
            continue;
        }

        bool ok = false;
        const double value = fields.at(column).trimmed().toDouble(&ok);
        if (ok) {
            samples_.append(value);
        }
    }

    if (samples_.isEmpty()) {
        return makeError(ErrorCode::ConfigInvalid,
                         QStringLiteral("no numeric samples found in column %1").arg(column), path);
    }
    return core::success();
}

} // namespace hwsim::simulator
