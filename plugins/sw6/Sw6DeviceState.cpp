#include "Sw6DeviceState.h"

#include <protocol/IDeviceAccess.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace hwsim::plugins::sw6 {
namespace {

/// Nominal actuator length at the mid position, and the radius the legs are
/// mounted on. Both are placeholders until the firmware publishes its geometry
/// through `$KINq` / `$GEOq`.
constexpr double kNominalLegLength = 100.0;
constexpr double kJointRadius = 55.0;

/// Analog front end of section 5.9: a bipolar 10 V input read by a 16-bit
/// converter, and a sensor that maps one volt to one millimetre. Placeholders
/// in the same sense as the geometry above.
constexpr double kAnalogFullScale = 10.0;
constexpr double kAdcFullScale = 32767.0;
constexpr double kSensorScale = 1.0;

/// Width of the simulated alignment peak, in mm of platform translation.
constexpr double kSignalWidth = 5.0;

std::array<double, kAxisCount>& fieldOf(
    std::array<std::array<double, kAxisCount>, kAxisFieldCount>& fields, AxisField field)
{
    return fields[static_cast<std::size_t>(field)];
}

std::array<double, kLegCount>& fieldOf(
    std::array<std::array<double, kLegCount>, kLegFieldCount>& fields, LegField field)
{
    return fields[static_cast<std::size_t>(field)];
}

void fill(std::array<double, kAxisCount>& target, double translation, double rotation)
{
    target = {translation, translation, translation, rotation, rotation, rotation};
}

std::size_t slotOf(FrameKind kind)
{
    return kind == FrameKind::Tool ? 0u : 1u;
}

} // namespace

std::optional<FrameKind> frameKindFromName(QStringView name)
{
    if (name.compare(QLatin1StringView("TOOL"), Qt::CaseInsensitive) == 0) {
        return FrameKind::Tool;
    }
    if (name.compare(QLatin1StringView("WORK"), Qt::CaseInsensitive) == 0) {
        return FrameKind::Work;
    }
    return std::nullopt;
}

QString frameKindName(FrameKind kind)
{
    return kind == FrameKind::Tool ? QStringLiteral("TOOL") : QStringLiteral("WORK");
}

std::array<double, kAxisCount> offsetInFrame(const std::array<double, kAxisCount>& framePose,
                                             const std::array<double, kAxisCount>& offset)
{
    const double sinU = std::sin(framePose[3]);
    const double cosU = std::cos(framePose[3]);
    const double sinV = std::sin(framePose[4]);
    const double cosV = std::cos(framePose[4]);
    const double sinW = std::sin(framePose[5]);
    const double cosW = std::cos(framePose[5]);

    const double x = offset[0];
    const double y = offset[1];
    const double z = offset[2];

    return {
        cosW * cosV * x + (cosW * sinV * sinU - sinW * cosU) * y
            + (cosW * sinV * cosU + sinW * sinU) * z,
        sinW * cosV * x + (sinW * sinV * sinU + cosW * cosU) * y
            + (sinW * sinV * cosU - cosW * sinU) * z,
        -sinV * x + cosV * sinU * y + cosV * cosU * z,
        offset[3],
        offset[4],
        offset[5],
    };
}

Sw6DeviceState::Sw6DeviceState()
{
    resetToDefaults();
    saved_ = live_;
}

void Sw6DeviceState::resetToDefaults()
{
    live_ = Storage{};

    auto& axes = live_.axisFields;
    fill(fieldOf(axes, AxisField::Velocity), 10.0, 0.1);
    fill(fieldOf(axes, AxisField::Acceleration), 20.0, 0.5);
    fill(fieldOf(axes, AxisField::Deceleration), 20.0, 0.5);
    fill(fieldOf(axes, AxisField::MaxVelocity), 100.0, 1.0);
    fill(fieldOf(axes, AxisField::MaxAcceleration), 50.0, 1.0);
    fieldOf(axes, AxisField::RangeMin) = {-50.0, -50.0, -25.0, -0.2, -0.2, -0.4};
    fieldOf(axes, AxisField::RangeMax) = {50.0, 50.0, 25.0, 0.2, 0.2, 0.4};
    fieldOf(axes, AxisField::SoftLimitMin) = fieldOf(axes, AxisField::RangeMin);
    fieldOf(axes, AxisField::SoftLimitMax) = fieldOf(axes, AxisField::RangeMax);
    fill(fieldOf(axes, AxisField::SoftLimitEnabled), 1.0, 1.0);
    fill(fieldOf(axes, AxisField::OnTarget), 1.0, 1.0);

    auto& legs = live_.legFields;
    fieldOf(legs, LegField::Enabled).fill(1.0);
    fieldOf(legs, LegField::BrakeReleased).fill(1.0);
    fieldOf(legs, LegField::TravelMin).fill(kNominalLegLength - 40.0);
    fieldOf(legs, LegField::TravelMax).fill(kNominalLegLength + 40.0);

    for (const NamedParameterSpec& parameter : kNamedParameters) {
        live_.parameters.insert(
            QString::fromUtf8(parameter.name.data(), static_cast<qsizetype>(parameter.name.size())),
            parameter.defaultValue);
    }

    errorCode_ = err::kSuccess;
    emergencyStopped_ = false;
    moving_ = false;
    streamEnabled_ = true;
    timer_ = 0.0;

    updateLegLengths();
}

std::array<double, kAxisCount>& Sw6DeviceState::axis(AxisField field)
{
    return fieldOf(live_.axisFields, field);
}

const std::array<double, kAxisCount>& Sw6DeviceState::axis(AxisField field) const
{
    return live_.axisFields[static_cast<std::size_t>(field)];
}

std::array<double, kLegCount>& Sw6DeviceState::leg(LegField field)
{
    return fieldOf(live_.legFields, field);
}

const std::array<double, kLegCount>& Sw6DeviceState::leg(LegField field) const
{
    return live_.legFields[static_cast<std::size_t>(field)];
}

double Sw6DeviceState::signalStrength() const
{
    const auto& pose = axis(AxisField::ActualPose);
    const double distance =
        pose[0] * pose[0] + pose[1] * pose[1] + pose[2] * pose[2];
    return std::exp(-distance / (2.0 * kSignalWidth * kSignalWidth));
}

double Sw6DeviceState::analogVoltage(int channel) const
{
    if (channel < 0 || channel >= kAnalogChannelCount) {
        return 0.0;
    }
    if (channel == 0) {
        return signalStrength() * kAnalogFullScale;
    }
    return live_.channelFields[static_cast<std::size_t>(ChannelField::AnalogVoltage)]
                              [static_cast<std::size_t>(channel)];
}

double Sw6DeviceState::channel(ChannelField field, int element) const
{
    switch (field) {
    case ChannelField::AnalogVoltage:
        return analogVoltage(element);
    case ChannelField::AnalogRaw:
        return std::round(analogVoltage(element) / kAnalogFullScale * kAdcFullScale);
    case ChannelField::AnalogNormalised:
        return analogVoltage(element) / kAnalogFullScale;
    case ChannelField::SensorPosition:
        return analogVoltage(element) * kSensorScale;
    default:
        break;
    }

    const ChannelRange range = channelRangeOf(field);
    return live_.channelFields[static_cast<std::size_t>(field)]
                              [static_cast<std::size_t>(element - range.first)];
}

void Sw6DeviceState::setChannel(ChannelField field, int element, double value)
{
    const ChannelRange range = channelRangeOf(field);
    live_.channelFields[static_cast<std::size_t>(field)]
                       [static_cast<std::size_t>(element - range.first)] = value;
}

QMap<int, double> Sw6DeviceState::triggerConfig(ChannelField field, int line) const
{
    const auto& lines = field == ChannelField::TriggerOutputConfig ? live_.triggerOutputs
                                                                   : live_.triggerInputs;
    return lines[static_cast<std::size_t>(line - 1)];
}

void Sw6DeviceState::setTriggerParameter(ChannelField field, int line, int parameter, double value)
{
    auto& lines = field == ChannelField::TriggerOutputConfig ? live_.triggerOutputs
                                                             : live_.triggerInputs;
    lines[static_cast<std::size_t>(line - 1)].insert(parameter, value);
}

void Sw6DeviceState::setIdentity(QString model, QString dateCode, QString serialNumber,
                                 QString firmware)
{
    model_ = std::move(model);
    dateCode_ = std::move(dateCode);
    serialNumber_ = std::move(serialNumber);
    firmwareVersion_ = std::move(firmware);
}

bool Sw6DeviceState::allReferenced() const
{
    const auto& referenced = leg(LegField::Referenced);
    return std::all_of(referenced.begin(), referenced.end(),
                       [](double value) { return value != 0.0; });
}

quint32 Sw6DeviceState::statusWord() const
{
    quint32 word = 0;
    if (moving_) {
        word |= status::kMoving;
    }
    const auto& onTarget = axis(AxisField::OnTarget);
    if (std::all_of(onTarget.begin(), onTarget.end(), [](double value) { return value != 0.0; })) {
        word |= status::kOnTarget;
    }
    if (allReferenced()) {
        word |= status::kReferenced;
    }
    if (errorCode_ != err::kSuccess) {
        word |= status::kError;
    }
    if (emergencyStopped_) {
        word |= status::kEmergencyStop;
    }
    if (alignmentRunning()) {
        word |= status::kAlignmentRunning;
    }
    return word;
}

quint32 Sw6DeviceState::checkMotionAllowed(int axisIndex) const
{
    if (emergencyStopped_) {
        return err::kEmergencyStopped;
    }
    if (axis(AxisField::ServoOn)[static_cast<std::size_t>(axisIndex)] == 0.0) {
        return err::kServoOff;
    }
    if (!allReferenced()) {
        return err::kNotReferenced;
    }
    return err::kSuccess;
}

quint32 Sw6DeviceState::checkSoftLimit(int axisIndex, double target) const
{
    const auto index = static_cast<std::size_t>(axisIndex);
    if (axis(AxisField::SoftLimitEnabled)[index] == 0.0) {
        return err::kSuccess;
    }
    if (target < axis(AxisField::SoftLimitMin)[index]
        || target > axis(AxisField::SoftLimitMax)[index]) {
        return err::kSoftLimitExceeded;
    }
    return err::kSuccess;
}

void Sw6DeviceState::applyPose(int axisIndex, double target)
{
    const auto index = static_cast<std::size_t>(axisIndex);
    axis(AxisField::TargetPose)[index] = target;
    axis(AxisField::ActualPose)[index] = target;
    axis(AxisField::OnTarget)[index] = 1.0;
    updateLegLengths();
}

void Sw6DeviceState::jog(int axisIndex, double speed)
{
    axis(AxisField::JogSpeed)[static_cast<std::size_t>(axisIndex)] = speed;
    refreshMoving();
}

void Sw6DeviceState::refreshMoving()
{
    const auto& speeds = axis(AxisField::JogSpeed);
    moving_ = trajectoryRunning()
              || std::any_of(speeds.begin(), speeds.end(),
                             [](double value) { return value != 0.0; });
}

void Sw6DeviceState::reference(int legNumber)
{
    auto& referenced = leg(LegField::Referenced);
    if (legNumber == 0) {
        referenced.fill(1.0);
    } else {
        referenced[static_cast<std::size_t>(legNumber - 1)] = 1.0;
    }
}

void Sw6DeviceState::halt()
{
    axis(AxisField::JogSpeed).fill(0.0);
    for (Sw6Trajectory& entry : live_.trajectories) {
        if (entry.state == TrajectoryState::Running || entry.state == TrajectoryState::Paused) {
            entry.state = TrajectoryState::Stopped;
        }
    }
    refreshMoving();
}

void Sw6DeviceState::stop(bool latchFault)
{
    halt();
    emergencyStopped_ = true;
    if (latchFault) {
        errorCode_ = err::kEmergencyStopped;
    }
}

void Sw6DeviceState::clearFaults()
{
    emergencyStopped_ = false;
    errorCode_ = err::kSuccess;
}

// --- Coordinate systems ----------------------------------------------------

const Sw6Frame* Sw6DeviceState::frame(FrameKind kind, const QString& name) const
{
    const auto& frames = live_.frames[slotOf(kind)];
    const auto it = frames.constFind(name);
    return it == frames.constEnd() ? nullptr : &it.value();
}

const Sw6Frame* Sw6DeviceState::enabledFrame(FrameKind kind) const
{
    const auto& frames = live_.frames[slotOf(kind)];
    for (auto it = frames.constBegin(); it != frames.constEnd(); ++it) {
        if (it.value().enabled) {
            return &it.value();
        }
    }
    return nullptr;
}

Sw6Frame& Sw6DeviceState::defineFrame(FrameKind kind, const QString& name)
{
    return live_.frames[slotOf(kind)][name];
}

bool Sw6DeviceState::removeFrame(FrameKind kind, const QString& name)
{
    return live_.frames[slotOf(kind)].remove(name) > 0;
}

QStringList Sw6DeviceState::frameNames(FrameKind kind) const
{
    return live_.frames[slotOf(kind)].keys();
}

void Sw6DeviceState::setFrameEnabled(FrameKind kind, const QString& name, bool enabled)
{
    auto& frames = live_.frames[slotOf(kind)];
    for (auto it = frames.begin(); it != frames.end(); ++it) {
        if (it.key() == name) {
            it.value().enabled = enabled;
        } else if (enabled) {
            it.value().enabled = false;
        }
    }
}

bool Sw6DeviceState::linkWouldCycle(FrameKind kind, const QString& name, const QString& parentType,
                                    const QString& parentName) const
{
    QString type = parentType;
    QString current = parentName;

    // Walk up to BASE; meeting the child on the way means the chain closes.
    for (int depth = 0; depth <= 2 * kAxisCount; ++depth) {
        const auto parentKind = frameKindFromName(type);
        if (!parentKind.has_value()) {
            return false; // reached BASE
        }
        if (*parentKind == kind && current == name) {
            return true;
        }

        const Sw6Frame* parent = frame(*parentKind, current);
        if (parent == nullptr) {
            return false;
        }
        type = parent->parentType;
        current = parent->parentName;
    }

    // Deeper than the number of frames that can exist without repeating one.
    return true;
}

// --- Alignment and identification ------------------------------------------

const Sw6Process* Sw6DeviceState::process(const QString& name) const
{
    const auto it = live_.processes.constFind(name);
    return it == live_.processes.constEnd() ? nullptr : &it.value();
}

Sw6Process& Sw6DeviceState::defineProcess(const QString& name)
{
    return live_.processes[name];
}

QStringList Sw6DeviceState::processNames() const
{
    return live_.processes.keys();
}

bool Sw6DeviceState::alignmentRunning() const
{
    return std::any_of(live_.processes.constBegin(), live_.processes.constEnd(),
                       [](const Sw6Process& entry) {
                           return entry.state == 1 || entry.state == 2;
                       });
}

double Sw6DeviceState::scanTowardsPeak(int axisIndex, double range, double centre)
{
    const auto slot = static_cast<std::size_t>(axisIndex);
    const double current = axis(AxisField::ActualPose)[slot];

    // The simulated peak sits at the scan centre, which `$FGC` moves and which
    // is the user zero pose until it does. A scan only reaches it if it lies
    // inside the window it was given.
    double target = centre;
    if (range != 0.0) {
        const double half = std::abs(range) / 2.0;
        target = std::clamp(target, current - half, current + half);
    }
    if (axis(AxisField::SoftLimitEnabled)[slot] != 0.0) {
        target = std::clamp(target, axis(AxisField::SoftLimitMin)[slot],
                            axis(AxisField::SoftLimitMax)[slot]);
    }

    applyPose(axisIndex, target);
    axis(AxisField::ScanResult)[slot] = target;
    return target;
}

Sw6FrequencyResponse& Sw6DeviceState::frequencyResponse(int axisIndex)
{
    return live_.sweeps[static_cast<std::size_t>(axisIndex)];
}

const Sw6FrequencyResponse& Sw6DeviceState::frequencyResponse(int axisIndex) const
{
    return live_.sweeps[static_cast<std::size_t>(axisIndex)];
}

std::optional<Sw6InputCalculation> Sw6DeviceState::inputCalculation(int input) const
{
    const auto it = live_.inputs.constFind(input);
    return it == live_.inputs.constEnd() ? std::nullopt
                                         : std::optional<Sw6InputCalculation>(it.value());
}

void Sw6DeviceState::setInputCalculation(int input, Sw6InputCalculation calculation)
{
    live_.inputs.insert(input, std::move(calculation));
}

QList<int> Sw6DeviceState::configuredInputs() const
{
    return live_.inputs.keys();
}

double Sw6DeviceState::inputValue(int input) const
{
    const double volts = analogVoltage(input - 1);
    const auto it = live_.inputs.constFind(input);
    if (it != live_.inputs.constEnd() && it->type == 1) {
        return volts / kAnalogFullScale;
    }
    return volts;
}

// --- Named parameters and read-only registries -----------------------------

std::optional<double> Sw6DeviceState::parameter(const QString& name) const
{
    const auto it = live_.parameters.constFind(name);
    return it == live_.parameters.constEnd() ? std::nullopt : std::optional<double>(it.value());
}

void Sw6DeviceState::setParameter(const QString& name, double value)
{
    live_.parameters.insert(name, value);
}

std::optional<std::array<double, kAxisCount>> Sw6DeviceState::geometryRow(const QString& row) const
{
    if (row.size() != 2) {
        return std::nullopt;
    }

    const QChar plate = row.at(0).toUpper();
    const int index = row.at(1).digitValue();
    if ((plate != QLatin1Char('B') && plate != QLatin1Char('P')) || index < 1
        || index > kLegCount) {
        return std::nullopt;
    }

    // Placeholder hinge layout: three pairs 120° apart, the two joints of a
    // pair split by a fixed separation, which is the usual Stewart arrangement.
    // The base plate reproduces the `$GEOq,B1s` example of section 5.8.1.
    const bool base = plate == QLatin1Char('B');
    const double radius = base ? 57.22 : 45.0;
    const double height = base ? 17.1 : 12.0;
    const double separation = (base ? 13.0 : 47.0) * std::numbers::pi / 180.0;

    const int joint = index - 1;
    const double pair = (2.0 * std::numbers::pi / 3.0) * static_cast<double>(joint / 2);
    const double angle = pair + (joint % 2 == 0 ? separation : -separation);

    return std::array<double, kAxisCount>{radius * std::cos(angle), radius * std::sin(angle),
                                          height,
                                          std::numbers::pi / 2.0,
                                          0.314159,
                                          angle + std::numbers::pi / 2.0};
}

std::array<double, kLegCount> Sw6DeviceState::legLengthsFor(
    const std::array<double, kAxisCount>& pose) const
{
    // Placeholder for the Stewart inverse kinematics: the legs are spaced 60°
    // apart, so Z lifts all six equally while X/Y and the tilts extend them by
    // the projection onto each leg's mounting direction. Enough for the
    // realtime stream to move plausibly; replace once the real geometry
    // constants are available through `$KINq` / `$GEOq`.
    std::array<double, kLegCount> lengths{};
    for (std::size_t index = 0; index < kLegCount; ++index) {
        const double angle = (std::numbers::pi / 3.0) * static_cast<double>(index);
        lengths[index] = kNominalLegLength + pose[2] + pose[0] * std::cos(angle)
                         + pose[1] * std::sin(angle)
                         + kJointRadius * (pose[3] * std::sin(angle) - pose[4] * std::cos(angle));
    }
    return lengths;
}

void Sw6DeviceState::updateLegLengths()
{
    const std::array<double, kLegCount> lengths = legLengthsFor(axis(AxisField::ActualPose));
    const auto& compensation = leg(LegField::Compensation);

    auto& actual = leg(LegField::ActualLength);
    auto& theoretical = leg(LegField::TheoreticalLength);

    for (std::size_t index = 0; index < kLegCount; ++index) {
        theoretical[index] = lengths[index];
        actual[index] = lengths[index] + compensation[index];
    }
}

// --- Trajectories ----------------------------------------------------------

const Sw6Trajectory& Sw6DeviceState::trajectory(int number) const
{
    return live_.trajectories[static_cast<std::size_t>(number - 1)];
}

Sw6Trajectory& Sw6DeviceState::trajectory(int number)
{
    return live_.trajectories[static_cast<std::size_t>(number - 1)];
}

Sw6TrajectoryCheck Sw6DeviceState::checkTrajectory(int number) const
{
    const Sw6Trajectory& target = trajectory(number);
    if (target.points.isEmpty()) {
        return {0, 1};
    }
    if (!allReferenced()) {
        return {0, 8};
    }
    const auto& servo = axis(AxisField::ServoOn);
    if (std::any_of(servo.begin(), servo.end(), [](double value) { return value == 0.0; })) {
        return {0, 9};
    }

    const auto& maxVelocity = axis(AxisField::MaxVelocity);
    const double fastest = *std::max_element(maxVelocity.begin(), maxVelocity.end());

    for (int index = 0; index < static_cast<int>(target.points.size()); ++index) {
        const auto point = target.points.constFind(index);
        if (point == target.points.constEnd()) {
            return {index, 2};
        }
        if (point->interpolation != 0 && point->interpolation != 1) {
            return {index, 7};
        }
        if (point->speed < 0.0 || point->speed > fastest) {
            return {index, 6};
        }
        for (int axisIndex = 0; axisIndex < kAxisCount; ++axisIndex) {
            if (checkSoftLimit(axisIndex, point->pose[static_cast<std::size_t>(axisIndex)])
                != err::kSuccess) {
                return {index, 3};
            }
        }

        // Reason 5, an unsolvable inverse kinematics, cannot come out of the
        // placeholder model above: every pose maps to six lengths.
        const std::array<double, kLegCount> lengths = legLengthsFor(point->pose);
        for (std::size_t legIndex = 0; legIndex < kLegCount; ++legIndex) {
            if (lengths[legIndex] < leg(LegField::TravelMin)[legIndex]
                || lengths[legIndex] > leg(LegField::TravelMax)[legIndex]) {
                return {index, 4};
            }
        }
    }
    return {};
}

void Sw6DeviceState::runTrajectory(int number)
{
    Sw6Trajectory& target = trajectory(number);

    for (auto it = target.points.constBegin(); it != target.points.constEnd(); ++it) {
        for (int axisIndex = 0; axisIndex < kAxisCount; ++axisIndex) {
            applyPose(axisIndex, it->pose[static_cast<std::size_t>(axisIndex)]);
        }
        target.currentPoint = it.key();
    }

    // Loop count 0 means "forever", which an instantaneous device can only
    // express by staying in Running until it is told to stop.
    target.currentLoop = target.loops == 0 ? 1 : target.loops;
    target.state = target.loops == 0 ? TrajectoryState::Running : TrajectoryState::Completed;
    refreshMoving();
}

bool Sw6DeviceState::controlTrajectory(int number, qint64 action)
{
    Sw6Trajectory& target = trajectory(number);
    switch (action) {
    case 0:
        target.state = TrajectoryState::Stopped;
        break;
    case 1:
        if (target.state != TrajectoryState::Running) {
            return false;
        }
        target.state = TrajectoryState::Paused;
        break;
    case 2:
        if (target.state != TrajectoryState::Paused) {
            return false;
        }
        target.state = TrajectoryState::Running;
        break;
    default:
        return false;
    }

    refreshMoving();
    return true;
}

bool Sw6DeviceState::trajectoryRunning() const
{
    return std::any_of(live_.trajectories.begin(), live_.trajectories.end(),
                       [](const Sw6Trajectory& entry) {
                           return entry.state == TrajectoryState::Running
                                  || entry.state == TrajectoryState::Paused;
                       });
}

Sw6StreamSample Sw6DeviceState::sample() const
{
    Sw6StreamSample sample;
    sample.mask = live_.streamMask & stream::kMaskKnownBits;
    sample.actualPose = axis(AxisField::ActualPose);
    sample.theoreticalPose = axis(AxisField::TargetPose);
    sample.actualLength = leg(LegField::ActualLength);
    sample.theoreticalLength = leg(LegField::TheoreticalLength);
    sample.legSpeed = leg(LegField::Speed);
    for (int index = 0; index < kAnalogChannelCount; ++index) {
        sample.analog[static_cast<std::size_t>(index)] = analogVoltage(index);
    }
    return sample;
}

void Sw6DeviceState::publishTo(protocol::IDeviceAccess* device) const
{
    if (device == nullptr) {
        return;
    }

    const auto& pose = axis(AxisField::ActualPose);
    for (int index = 0; index < kAxisCount; ++index) {
        const QString key = QStringLiteral("pose.%1").arg(axisName(index).toLower());
        (void)device->writeParameter(key, pose[static_cast<std::size_t>(index)]);
    }

    const auto& lengths = leg(LegField::ActualLength);
    for (int index = 0; index < kLegCount; ++index) {
        const QString key = QStringLiteral("leg.%1.length").arg(index + 1);
        (void)device->writeParameter(key, lengths[static_cast<std::size_t>(index)]);
    }

    (void)device->writeParameter(QStringLiteral("status.word"), statusWord());
}

} // namespace hwsim::plugins::sw6
