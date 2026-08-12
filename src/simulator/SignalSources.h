#pragma once

#include "ISignalSource.h"

#include <QStringList>
#include <QVector>

#include <random>

namespace hwsim::simulator {

/// Fixed value. Useful as the base of a binding stack, with noise added on top.
class HWSIM_SIMULATOR_API ConstantSource final : public ISignalSource {
public:
    [[nodiscard]] QString kind() const override { return QStringLiteral("constant"); }
    [[nodiscard]] double sample(qint64 elapsedMs) override;
    [[nodiscard]] core::ConfigSchema schema() const override;
};

class HWSIM_SIMULATOR_API SineSource final : public ISignalSource {
public:
    [[nodiscard]] QString kind() const override { return QStringLiteral("sine"); }
    [[nodiscard]] double sample(qint64 elapsedMs) override;
    [[nodiscard]] core::ConfigSchema schema() const override;
};

class HWSIM_SIMULATOR_API SquareSource final : public ISignalSource {
public:
    [[nodiscard]] QString kind() const override { return QStringLiteral("square"); }
    [[nodiscard]] double sample(qint64 elapsedMs) override;
    [[nodiscard]] core::ConfigSchema schema() const override;
};

class HWSIM_SIMULATOR_API TriangleSource final : public ISignalSource {
public:
    [[nodiscard]] QString kind() const override { return QStringLiteral("triangle"); }
    [[nodiscard]] double sample(qint64 elapsedMs) override;
    [[nodiscard]] core::ConfigSchema schema() const override;
};

class HWSIM_SIMULATOR_API SawtoothSource final : public ISignalSource {
public:
    [[nodiscard]] QString kind() const override { return QStringLiteral("sawtooth"); }
    [[nodiscard]] double sample(qint64 elapsedMs) override;
    [[nodiscard]] core::ConfigSchema schema() const override;
};

/// Holds `initialValue`, jumps to `finalValue` at `stepAtMs`. With `repeat`
/// enabled it alternates every `stepAtMs`, which models a device that toggles
/// between two operating points.
class HWSIM_SIMULATOR_API StepSource final : public ISignalSource {
public:
    [[nodiscard]] QString kind() const override { return QStringLiteral("step"); }
    [[nodiscard]] double sample(qint64 elapsedMs) override;
    [[nodiscard]] core::ConfigSchema schema() const override;
};

/// Linear sweep from `startValue` to `endValue` over `durationMs`.
class HWSIM_SIMULATOR_API RampSource final : public ISignalSource {
public:
    [[nodiscard]] QString kind() const override { return QStringLiteral("ramp"); }
    [[nodiscard]] double sample(qint64 elapsedMs) override;
    [[nodiscard]] core::ConfigSchema schema() const override;
};

/// Uniform or gaussian noise. Normally attached with combine=add on top of
/// another source, which is how a realistic sensor trace is built.
class HWSIM_SIMULATOR_API NoiseSource final : public ISignalSource {
public:
    NoiseSource();

    [[nodiscard]] QString kind() const override { return QStringLiteral("noise"); }
    [[nodiscard]] double sample(qint64 elapsedMs) override;
    [[nodiscard]] core::ConfigSchema schema() const override;
    [[nodiscard]] core::Result<void> configure(const QVariantMap& config) override;
    void reset() override;

private:
    std::mt19937_64 engine_;
    quint64 seed_{0};
};

/// Bounded random walk, for slowly drifting quantities.
class HWSIM_SIMULATOR_API RandomWalkSource final : public ISignalSource {
public:
    RandomWalkSource();

    [[nodiscard]] QString kind() const override { return QStringLiteral("random-walk"); }
    [[nodiscard]] double sample(qint64 elapsedMs) override;
    [[nodiscard]] core::ConfigSchema schema() const override;
    [[nodiscard]] core::Result<void> configure(const QVariantMap& config) override;
    void reset() override;

private:
    std::mt19937_64 engine_;
    double current_{0.0};
    qint64 lastSampleMs_{-1};
    bool started_{false};
};

/// Replays a column of a CSV file, which is how a captured field trace becomes
/// a reproducible regression input.
class HWSIM_SIMULATOR_API ReplaySource final : public ISignalSource {
public:
    [[nodiscard]] QString kind() const override { return QStringLiteral("replay"); }
    [[nodiscard]] double sample(qint64 elapsedMs) override;
    [[nodiscard]] core::ConfigSchema schema() const override;
    [[nodiscard]] core::Result<void> configure(const QVariantMap& config) override;
    void reset() override;

    [[nodiscard]] qsizetype sampleCount() const { return samples_.size(); }

private:
    [[nodiscard]] core::Result<void> load(const QString& path, int column, bool hasHeader);

    QVector<double> samples_;
    int intervalMs_{100};
    bool loop_{true};
};

} // namespace hwsim::simulator
