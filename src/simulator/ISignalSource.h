#pragma once

#include "SimulatorGlobal.h"

#include <core/ConfigSchema.h>
#include <core/Registry.h>
#include <core/Result.h>

#include <memory>

namespace hwsim::simulator {

/// Generates the time-varying value behind a parameter.
///
/// Sources are pure functions of elapsed time wherever possible, which keeps
/// them reproducible: a test can sample a waveform at chosen instants without
/// waiting for wall-clock time to pass. Stateful sources (random walk, replay)
/// say so by overriding reset().
class HWSIM_SIMULATOR_API ISignalSource {
public:
    virtual ~ISignalSource() = default;

    [[nodiscard]] virtual QString kind() const = 0;

    /// Value at `elapsedMs` milliseconds after the source started.
    [[nodiscard]] virtual double sample(qint64 elapsedMs) = 0;

    [[nodiscard]] virtual core::ConfigSchema schema() const = 0;

    [[nodiscard]] virtual core::Result<void> configure(const QVariantMap& config);

    virtual void reset() {}

    [[nodiscard]] const QVariantMap& configuration() const noexcept { return config_; }

protected:
    /// Stores the normalised map; subclasses read their fields from it.
    void storeConfiguration(QVariantMap config) { config_ = std::move(config); }

    [[nodiscard]] double number(const char* key, double fallback) const;
    [[nodiscard]] qint64 integer(const char* key, qint64 fallback) const;
    [[nodiscard]] bool flag(const char* key, bool fallback) const;
    [[nodiscard]] QString text(const char* key, const QString& fallback = {}) const;

private:
    QVariantMap config_;
};

using SignalSourcePtr = std::unique_ptr<ISignalSource>;

/// Kind name -> source. Registering a new waveform is one call here plus the
/// class itself; nothing in the engine or the UI needs to change, because the
/// configuration panel is generated from schema().
[[nodiscard]] HWSIM_SIMULATOR_API core::Registry<QString, ISignalSource>& signalSourceRegistry();

/// Registers the built-in waveforms. Idempotent; called by SignalEngine.
HWSIM_SIMULATOR_API void registerBuiltinSignalSources();

} // namespace hwsim::simulator
