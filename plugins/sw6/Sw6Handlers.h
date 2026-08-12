#pragma once

// -----------------------------------------------------------------------------
// One handler per message shape, plus the registration that binds every command
// name in the tables to them.
//
// A handler reads the spec belonging to the command name it was given and acts
// on the quantity the spec points at, so the command set grows by editing
// Sw6Commands.h alone.
// -----------------------------------------------------------------------------

#include "Sw6DeviceState.h"
#include "Sw6Messages.h"

#include <protocol/CommandRegistry.h>
#include <protocol/ExecutionContext.h>
#include <protocol/IUnsolicitedSource.h>

#include <memory>

namespace hwsim::plugins::sw6 {

using hwsim::protocol::CommandRegistry;
using hwsim::protocol::ExecutionContext;

class AxisCommandHandler {
public:
    explicit AxisCommandHandler(std::shared_ptr<Sw6DeviceState> state);
    [[nodiscard]] Result<MessagePtr> handle(const AxisCommand& command, ExecutionContext& context);

private:
    /// `$MRT` / `$MRW`, which move on axes the request never named because the
    /// frame's orientation redistributes the offset.
    [[nodiscard]] Result<MessagePtr> moveInFrame(const AxisCommandSpec& spec,
                                                 const AxisCommand& command,
                                                 ExecutionContext& context);

    std::shared_ptr<Sw6DeviceState> state_;
};

class LegCommandHandler {
public:
    explicit LegCommandHandler(std::shared_ptr<Sw6DeviceState> state);
    [[nodiscard]] Result<MessagePtr> handle(const LegCommand& command, ExecutionContext& context);

private:
    std::shared_ptr<Sw6DeviceState> state_;
};

class ChannelCommandHandler {
public:
    explicit ChannelCommandHandler(std::shared_ptr<Sw6DeviceState> state);
    [[nodiscard]] Result<MessagePtr> handle(const ChannelCommand& command,
                                            ExecutionContext& context);

private:
    std::shared_ptr<Sw6DeviceState> state_;
};

class SystemCommandHandler {
public:
    explicit SystemCommandHandler(std::shared_ptr<Sw6DeviceState> state);
    [[nodiscard]] Result<MessagePtr> handle(const SystemCommand& command, ExecutionContext& context);

private:
    std::shared_ptr<Sw6DeviceState> state_;
};

class CoordinateCommandHandler {
public:
    explicit CoordinateCommandHandler(std::shared_ptr<Sw6DeviceState> state);
    [[nodiscard]] Result<MessagePtr> handle(const CoordinateCommand& command,
                                            ExecutionContext& context);

private:
    std::shared_ptr<Sw6DeviceState> state_;
};

class NamedCommandHandler {
public:
    explicit NamedCommandHandler(std::shared_ptr<Sw6DeviceState> state);
    [[nodiscard]] Result<MessagePtr> handle(const NamedCommand& command,
                                            ExecutionContext& context);

private:
    std::shared_ptr<Sw6DeviceState> state_;
};

class TrajectoryCommandHandler {
public:
    explicit TrajectoryCommandHandler(std::shared_ptr<Sw6DeviceState> state);
    [[nodiscard]] Result<MessagePtr> handle(const TrajectoryCommand& command,
                                            ExecutionContext& context);

private:
    std::shared_ptr<Sw6DeviceState> state_;
};

class AlignmentCommandHandler {
public:
    explicit AlignmentCommandHandler(std::shared_ptr<Sw6DeviceState> state);
    [[nodiscard]] Result<MessagePtr> handle(const AlignmentCommand& command,
                                            ExecutionContext& context);

private:
    /// `$FDG` / `$FDR` / `$FDL`, which differ only in whether the axes come
    /// with a range.
    [[nodiscard]] Result<MessagePtr> defineSearch(const AlignmentCommandSpec& spec,
                                                  const AlignmentCommand& command);

    /// The one- and two-axis scans, which all move towards the signal peak.
    [[nodiscard]] Result<MessagePtr> runScan(const AlignmentCommandSpec& spec,
                                             const AlignmentCommand& command);

    /// `$FSF`, whose second argument is a force rather than a scan range.
    [[nodiscard]] Result<MessagePtr> detectSurface(const AlignmentCommand& command);

    std::shared_ptr<Sw6DeviceState> state_;
};

/// Answers `0x03000001` instead of leaving the master to time out.
class UnknownCommandHandler {
public:
    [[nodiscard]] Result<MessagePtr> handle(const UnknownCommand& command,
                                            ExecutionContext& context);
};

/// Initiator side of the 0x81 stream: folds each frame into a mirror of the
/// device state, which is what a test master wants to assert on.
class RealtimeStreamHandler {
public:
    explicit RealtimeStreamHandler(std::shared_ptr<Sw6DeviceState> mirror);
    [[nodiscard]] Result<MessagePtr> handle(const RealtimeFrame& frame, ExecutionContext& context);

    [[nodiscard]] Sw6StreamSample lastSample() const { return lastSample_; }
    [[nodiscard]] quint64 frameCount() const noexcept { return frameCount_; }

private:
    std::shared_ptr<Sw6DeviceState> mirror_;
    Sw6StreamSample lastSample_;
    quint64 frameCount_{0};
};

/// Responder side of the 0x81 stream: the device pushes it on its own, which
/// no handler can express, so it is published through the session's
/// unsolicited-source timer instead.
class Sw6StreamSource final : public protocol::IUnsolicitedSource {
public:
    Sw6StreamSource(std::shared_ptr<Sw6DeviceState> state, int intervalMs);

    [[nodiscard]] QString name() const override { return QStringLiteral("sw6.realtime"); }
    [[nodiscard]] int intervalMs() const override { return intervalMs_; }
    [[nodiscard]] std::vector<MessagePtr> poll(qint64 nowMs) override;

private:
    std::shared_ptr<Sw6DeviceState> state_;
    int intervalMs_;
};

/// Fills the registry for one device. The responder decodes commands, answers
/// them and pushes the realtime stream every `streamIntervalMs` (0 disables);
/// the initiator encodes commands, decodes replies and consumes the stream.
[[nodiscard]] Result<void> registerSw6Commands(CommandRegistry& registry,
                                               const std::shared_ptr<Sw6DeviceState>& state,
                                               bool initiator, int streamIntervalMs = 0);

} // namespace hwsim::plugins::sw6
