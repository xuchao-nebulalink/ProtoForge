#pragma once

// -----------------------------------------------------------------------------
// The simulated hexapod behind the responder role.
//
// Per-axis and per-leg quantities live in flat arrays indexed by AxisField /
// LegField, which is what lets the declarative command table point a command at
// a quantity without any dispatch code in between.
//
// Motion is instantaneous: a `$MOV` moves the actual pose to the target and
// reports on-target straight away. The platform has no simulation tick of its
// own, and an immediate move keeps request/reply behaviour deterministic for
// tests; the signal generators in the simulator module are the place to add
// movement over time.
// -----------------------------------------------------------------------------

#include "Sw6Commands.h"
#include "Sw6Messages.h"
#include "Sw6Types.h"

#include <QMap>
#include <QString>
#include <QStringList>

#include <array>
#include <optional>

namespace hwsim::protocol {
class IDeviceAccess;
}

namespace hwsim::plugins::sw6 {

/// Section 5.7: a functional coordinate system. `BASE` is not stored - it is
/// the implicit root every chain ends at.
enum class FrameKind : quint8 { Tool, Work };

struct Sw6Frame {
    std::array<double, kAxisCount> pose{};
    bool enabled{false};
    QString parentType{QStringLiteral("BASE")};
    QString parentName{QStringLiteral("BASE")};
};

[[nodiscard]] std::optional<FrameKind> frameKindFromName(QStringView name);
[[nodiscard]] QString frameKindName(FrameKind kind);

/// Section 5.2: `$MRT` and `$MRW` state their offset in a functional frame, so
/// the translation part turns with that frame's orientation before it becomes
/// a BASE target, while the rotation part adds directly. The rotation order is
/// Rz(W)·Ry(V)·Rx(U), which is how the U/V/W of a frame pose are read
/// everywhere else in this plugin.
[[nodiscard]] std::array<double, kAxisCount> offsetInFrame(
    const std::array<double, kAxisCount>& framePose,
    const std::array<double, kAxisCount>& offset);

/// Section 5.8 registry. Values are held as doubles and formatted back with
/// the declared kind, so `$PARq` answers `ADCHs,0d` and `HPRTs,130f`.
struct NamedParameterSpec {
    std::string_view name;
    ValueKind kind;
    double defaultValue;
};

inline constexpr auto kNamedParameters = std::to_array<NamedParameterSpec>({
    {"ADCH", ValueKind::Integer, 0.0},
    {"HPRT", ValueKind::Float, 130.0},
});

/// Section 5.8.1, read-only. Placeholder values in the shape the document
/// describes; replaced by the firmware's own constants once published.
struct KinematicScalar {
    std::string_view name;
    double value;
};

inline constexpr auto kKinematicScalars = std::to_array<KinematicScalar>({
    {"Ub", 57.22},
    {"Up", 45.0},
    {"Hp", 12.0},
    {"Hb", 17.1},
    {"H", 89.705},
    {"Lead", 5.0},
    {"t3InitOdd", 0.314159},
    {"t3InitEven", -0.314159},
});

/// Section 5.12: one buffered point. The axes a `$TGA` leaves out keep the
/// value of the point before it, which the controller is expected to fill in.
struct Sw6TrajectoryPoint {
    int interpolation{0};
    double speed{0.0};
    int dwellMs{0};
    std::array<double, kAxisCount> pose{};
};

/// Trajectory states as `$TGIq` reports them.
enum class TrajectoryState : quint8 {
    Empty = 0,
    Writing = 1,
    Ready = 2,
    Running = 3,
    Paused = 4,
    Completed = 5,
    Failed = 6,
    Stopped = 7,
};

struct Sw6Trajectory {
    /// Sparse on purpose: `$TGA` may write any point number, and `$TGF`
    /// reports a gap as reason 2 rather than the write being refused.
    QMap<int, Sw6TrajectoryPoint> points;
    TrajectoryState state{TrajectoryState::Empty};
    int loops{1};
    int currentPoint{0};
    int currentLoop{0};
};

/// Outcome of the `$TGF` check: reason 0 with point 0 means the trajectory
/// passed. The reason codes are the table in section 5.12.
struct Sw6TrajectoryCheck {
    int errorPoint{0};
    int reason{0};

    [[nodiscard]] bool passed() const noexcept { return reason == 0; }
};

/// Section 5.15: a named alignment process. The document leaves the state
/// numbering to the firmware, so this device reports 0 = 未启动, 1 = 运行中,
/// 2 = 暂停, 3 = 已停止, and keeps the last result across a stop.
struct Sw6Process {
    /// `FDG`, `FDR` or `FDL`, so `$FRPq` and the search itself can tell a
    /// gradient search from a raster one.
    QString definedBy;
    int scanAxis{0};
    int stepAxis{0};
    /// Window the search may move within. Zero means unbounded, which is what
    /// a gradient search that was given no range gets.
    double scanRange{0.0};
    double stepRange{0.0};
    double scanCentre{0.0};
    double stepCentre{0.0};
    QString coupled;
    int state{0};
    /// Scan axis position, step axis position and the signal that was found.
    std::array<double, 3> result{};
};

inline constexpr int kProcessResultCount = 3;

/// Section 5.15 `$SIC`. The calculation types are firmware-defined, so the
/// device keeps what it was told and only acts on the one it can: type 1
/// normalises the input, everything else reads the channel as it is.
struct Sw6InputCalculation {
    int type{0};
    Sw6Values parameters;
};

/// Section 5.15 `$WFR`: the sweep an axis was asked to run. The response data
/// itself is firmware-defined, so `$WFRq` echoes the sweep.
struct Sw6FrequencyResponse {
    int mode{-1};
    double amplitude{0.0};
    double lowFrequency{0.0};
    double highFrequency{0.0};
};

class Sw6DeviceState {
public:
    Sw6DeviceState();

    [[nodiscard]] std::array<double, kAxisCount>& axis(AxisField field);
    [[nodiscard]] const std::array<double, kAxisCount>& axis(AxisField field) const;
    [[nodiscard]] std::array<double, kLegCount>& leg(LegField field);
    [[nodiscard]] const std::array<double, kLegCount>& leg(LegField field) const;

    // --- Analog inputs and I/O lines (sections 5.9 and 5.10) ---------------

    /// Stand-in for the optical or probe signal the alignment scans of section
    /// 5.15 chase: a Gaussian peak at the user zero pose, so moving away from
    /// it weakens the signal. Range 0..1.
    [[nodiscard]] double signalStrength() const;

    /// Channel 0 carries that signal; the remaining inputs read whatever was
    /// written to them, which for an unwired simulator is 0 V.
    [[nodiscard]] double analogVoltage(int channel) const;

    /// Value of one element of a per-channel quantity. The analog fields are
    /// derived from the input voltage rather than stored, so that the raw
    /// count, the normalised value and the scaled position cannot drift apart.
    [[nodiscard]] double channel(ChannelField field, int element) const;
    void setChannel(ChannelField field, int element, double value);

    /// Section 5.10: `$CTO` / `$CTI` parameters of one trigger line.
    [[nodiscard]] QMap<int, double> triggerConfig(ChannelField field, int line) const;
    void setTriggerParameter(ChannelField field, int line, int parameter, double value);

    [[nodiscard]] int averageCount() const noexcept { return live_.averageCount; }
    void setAverageCount(int count) noexcept { live_.averageCount = count; }

    // --- Identity (section 5.1) --------------------------------------------

    [[nodiscard]] QString model() const { return model_; }
    [[nodiscard]] QString dateCode() const { return dateCode_; }
    [[nodiscard]] QString serialNumber() const { return serialNumber_; }
    [[nodiscard]] QString firmwareVersion() const { return firmwareVersion_; }
    void setIdentity(QString model, QString dateCode, QString serialNumber, QString firmware);

    // --- Status ------------------------------------------------------------

    [[nodiscard]] quint32 errorCode() const noexcept { return errorCode_; }
    void setErrorCode(quint32 code) noexcept { errorCode_ = code; }

    [[nodiscard]] bool emergencyStopped() const noexcept { return emergencyStopped_; }
    [[nodiscard]] bool moving() const noexcept { return moving_; }
    [[nodiscard]] bool allReferenced() const;

    /// Section 4.8, assembled from the flags above.
    [[nodiscard]] quint32 statusWord() const;

    [[nodiscard]] quint8 streamMask() const noexcept { return live_.streamMask; }
    void setStreamMask(quint8 mask) noexcept { live_.streamMask = mask; }

    /// `$RTO` prepares the controller for power-off, which includes stopping
    /// the realtime stream; everything else leaves it running.
    [[nodiscard]] bool streamEnabled() const noexcept { return streamEnabled_; }
    void setStreamEnabled(bool enabled) noexcept { streamEnabled_ = enabled; }

    [[nodiscard]] double timer() const noexcept { return timer_; }
    void setTimer(double value) noexcept { timer_ = value; }

    [[nodiscard]] double maxAngularRate() const noexcept { return maxAngularRate_; }

    /// Section 4.6 uses `$VLSq` as its example of a global single value: the
    /// speed the platform moves its TCP at, independent of the per-axis `$VEL`.
    [[nodiscard]] double platformSpeed() const noexcept { return live_.platformSpeed; }
    void setPlatformSpeed(double speed) noexcept { live_.platformSpeed = speed; }

    // --- Operations --------------------------------------------------------

    /// Preconditions shared by every motion command: no latched fault, servo
    /// on, platform referenced. Returns err::kSuccess when the move may start.
    [[nodiscard]] quint32 checkMotionAllowed(int axisIndex) const;

    /// Section 5.6: rejects a target outside the enabled soft limits, so a
    /// multi-axis move can be validated in full before any of it is applied.
    [[nodiscard]] quint32 checkSoftLimit(int axisIndex, double target) const;

    /// Moves one degree of freedom and recomputes the leg lengths.
    void applyPose(int axisIndex, double target);

    void jog(int axisIndex, double speed);

    /// `$FRF`: references one leg, or all six when `leg` is 0.
    void reference(int leg);

    /// `$HLT` / `$STP` / `$STF` / `$CLR`.
    void halt();
    void stop(bool latchFault);
    void clearFaults();

    // --- Coordinate systems (section 5.7) ----------------------------------

    [[nodiscard]] const Sw6Frame* frame(FrameKind kind, const QString& name) const;

    /// The single active frame of its kind, or nullptr when none is enabled -
    /// which is what `$MRT` / `$MRW` answer 0x04000006 to.
    [[nodiscard]] const Sw6Frame* enabledFrame(FrameKind kind) const;

    Sw6Frame& defineFrame(FrameKind kind, const QString& name);
    bool removeFrame(FrameKind kind, const QString& name);
    [[nodiscard]] QStringList frameNames(FrameKind kind) const;

    /// Enabling one frame disables the others of its kind: the protocol has a
    /// single active tool and a single active work system.
    void setFrameEnabled(FrameKind kind, const QString& name, bool enabled);

    /// True when making `<kind>,<name>` a child of the given parent would
    /// close a loop, which section 5.7 rejects with 0x0300000C.
    [[nodiscard]] bool linkWouldCycle(FrameKind kind, const QString& name,
                                      const QString& parentType, const QString& parentName) const;

    // --- Trajectories (section 5.12) ---------------------------------------

    /// Trajectories are numbered 1..kTrajectoryCount; the handler rejects
    /// anything else before it gets here.
    [[nodiscard]] const Sw6Trajectory& trajectory(int number) const;
    [[nodiscard]] Sw6Trajectory& trajectory(int number);

    /// The `$TGF` check. Runs against the platform's own limits, so a
    /// trajectory that passed once can fail later if the soft limits moved.
    [[nodiscard]] Sw6TrajectoryCheck checkTrajectory(int number) const;

    /// `$TGS`. Motion is instantaneous here, so the platform lands on the last
    /// point straight away; a trajectory set to loop forever stays in Running
    /// until `$TST,<n>d,0d` stops it.
    void runTrajectory(int number);

    /// `$TST`: 0 stops, 1 pauses, 2 resumes. False when the trajectory is not
    /// in a state that transition can be made from.
    bool controlTrajectory(int number, qint64 action);

    [[nodiscard]] bool trajectoryRunning() const;

    [[nodiscard]] int trajectoryPeriod() const noexcept { return live_.trajectoryPeriod; }
    void setTrajectoryPeriod(int servoCycles) noexcept { live_.trajectoryPeriod = servoCycles; }

    // --- Alignment and identification (section 5.15) -----------------------

    [[nodiscard]] const Sw6Process* process(const QString& name) const;
    Sw6Process& defineProcess(const QString& name);
    [[nodiscard]] QStringList processNames() const;

    /// Status word bit 6 of section 4.8.
    [[nodiscard]] bool alignmentRunning() const;

    /// Moves one axis towards the signal peak without leaving the window the
    /// scan was given (`range` 0 = no window), and records where it stopped as
    /// that axis's `$FSNq` result. Returns the position it reached.
    double scanTowardsPeak(int axisIndex, double range, double centre);

    [[nodiscard]] Sw6FrequencyResponse& frequencyResponse(int axisIndex);
    [[nodiscard]] const Sw6FrequencyResponse& frequencyResponse(int axisIndex) const;

    [[nodiscard]] std::optional<Sw6InputCalculation> inputCalculation(int input) const;
    void setInputCalculation(int input, Sw6InputCalculation calculation);
    [[nodiscard]] QList<int> configuredInputs() const;

    /// Value `$TCIq` reports for one input, after its calculation.
    [[nodiscard]] double inputValue(int input) const;

    // --- Named parameters and read-only registries (section 5.8) -----------

    [[nodiscard]] std::optional<double> parameter(const QString& name) const;
    void setParameter(const QString& name, double value);

    /// Six axis values of one hinge row, `B1`..`B6` / `P1`..`P6`.
    [[nodiscard]] std::optional<std::array<double, kAxisCount>> geometryRow(
        const QString& row) const;

    void save() { saved_ = live_; }
    void restore() { live_ = saved_; }
    void resetToDefaults();

    [[nodiscard]] Sw6StreamSample sample() const;

    /// Mirrors the headline values into the device parameter tree so the UI
    /// panel and the scripting layer see them. Best effort: a profile that does
    /// not declare these parameters simply gets nothing.
    void publishTo(protocol::IDeviceAccess* device) const;

private:
    /// Everything `$SAV` snapshots and `$RST` puts back.
    struct Storage {
        std::array<std::array<double, kAxisCount>, kAxisFieldCount> axisFields{};
        std::array<std::array<double, kLegCount>, kLegFieldCount> legFields{};
        std::array<std::array<double, kChannelSlotCount>, kChannelFieldCount> channelFields{};
        std::array<QMap<int, double>, kTriggerLineCount> triggerOutputs;
        std::array<QMap<int, double>, kTriggerLineCount> triggerInputs;
        std::array<QMap<QString, Sw6Frame>, 2> frames;
        std::array<Sw6Trajectory, kTrajectoryCount> trajectories;
        std::array<Sw6FrequencyResponse, kAxisCount> sweeps;
        QMap<QString, Sw6Process> processes;
        QMap<int, Sw6InputCalculation> inputs;
        QMap<QString, double> parameters;
        quint8 streamMask{0};
        int averageCount{1};
        int trajectoryPeriod{1};
        double platformSpeed{10.0};
    };

    /// Status word bit 0: a jog or an endless trajectory is what keeps the
    /// platform moving, since every other move finishes before its reply does.
    void refreshMoving();

    /// Placeholder inverse kinematics, see the .cpp for what it does and does
    /// not model.
    void updateLegLengths();

    /// The same kinematics applied to a pose the platform is not at, which is
    /// what the `$TGF` leg travel check needs.
    [[nodiscard]] std::array<double, kLegCount> legLengthsFor(
        const std::array<double, kAxisCount>& pose) const;

    Storage live_;
    Storage saved_;

    QString model_{QStringLiteral("SW6_HEXAPOD_V2")};
    QString dateCode_{QStringLiteral("2509")};
    QString serialNumber_{QStringLiteral("001")};
    QString firmwareVersion_{QStringLiteral("2.0.0")};

    quint32 errorCode_{err::kSuccess};
    bool emergencyStopped_{false};
    bool moving_{false};
    bool streamEnabled_{true};
    double timer_{0.0};
    double maxAngularRate_{0.5};
};

} // namespace hwsim::plugins::sw6
