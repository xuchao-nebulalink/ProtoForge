#include "Sw6Handlers.h"

#include <core/Logger.h>

#include <QStringList>

#include <cmath>

using hwsim::core::ErrorCode;
using hwsim::core::makeError;

namespace {
constexpr auto kLogCategory = "plugin.sw6";
} // namespace

namespace hwsim::plugins::sw6 {
namespace {

/// Section 4.5: leaving the element out means "all of them". `limit` is the
/// number of axes the command accepts, which is three for `$SPI`.
QVector<int> requestedAxes(const Sw6Elements& elements, int limit)
{
    QVector<int> axes;
    if (elements.isEmpty()) {
        axes.reserve(limit);
        for (int index = 0; index < limit; ++index) {
            axes.append(index);
        }
        return axes;
    }

    axes.reserve(elements.size());
    for (const Sw6Element& element : elements) {
        axes.append(element.index);
    }
    return axes;
}

QVector<int> requestedLegs(const Sw6Elements& elements)
{
    QVector<int> legs;
    if (elements.isEmpty()) {
        legs.reserve(kLegCount);
        for (int number = 1; number <= kLegCount; ++number) {
            legs.append(number);
        }
        return legs;
    }

    legs.reserve(elements.size());
    for (const Sw6Element& element : elements) {
        legs.append(element.index);
    }
    return legs;
}

/// Same rule for the analog channels and I/O lines, whose numbering starts
/// where their range says rather than always at one.
QVector<int> requestedElements(const Sw6Elements& elements, ChannelRange range)
{
    QVector<int> requested;
    if (elements.isEmpty()) {
        requested.reserve(range.count);
        for (int offset = 0; offset < range.count; ++offset) {
            requested.append(range.first + offset);
        }
        return requested;
    }

    requested.reserve(elements.size());
    for (const Sw6Element& element : elements) {
        requested.append(element.index);
    }
    return requested;
}

/// Section 4.1 caps a frame at 256 bytes, and a reply that lists an open-ended
/// number of groups - every command name, every configured trigger parameter -
/// can outgrow it. Such a reply drops whole groups from its tail instead of
/// producing a frame the codec would refuse to send. `stride` is how many
/// values make one group, `reserved` what the caller still puts in front.
void fitToFrame(const QString& command, Sw6Values& values, qsizetype stride,
                qsizetype reserved = 0)
{
    // `$<Cmd>` + `,0x00000000` + `;XX`.
    qsizetype used = 1 + command.size() + 11 + 3 + reserved;
    qsizetype kept = 0;

    for (qsizetype start = 0; start + stride <= values.size(); start += stride) {
        qsizetype cost = 0;
        for (qsizetype offset = 0; offset < stride; ++offset) {
            cost += values.at(start + offset).toToken().size() + 1;
        }
        if (used + cost > static_cast<qsizetype>(kMaxAsciiFrameBytes)) {
            break;
        }
        used += cost;
        kept = start + stride;
    }
    values.resize(kept);
}

/// Value the request gave for one axis, or 0 when it named the axis without a
/// value (`$GOH,Xs`) or did not name it at all (`$GOH`).
double valueFor(const Sw6Elements& elements, int index)
{
    for (const Sw6Element& element : elements) {
        if (element.index == index && !element.values.isEmpty()) {
            return element.values.first().asDouble();
        }
    }
    return 0.0;
}

Sw6Value quantity(double value, ValueKind kind)
{
    return kind == ValueKind::Integer ? Sw6Value::ofInt(std::llround(value))
                                      : Sw6Value::ofFloat(value);
}

QString displayNameOf(std::string_view summary)
{
    return QString::fromUtf8(summary.data(), static_cast<qsizetype>(summary.size()));
}

QString textAt(const Sw6Values& values, qsizetype index)
{
    return index < values.size() && values.at(index).isText() ? values.at(index).label : QString{};
}

/// Numeric argument at `index`, or nothing when it is missing or a string.
std::optional<double> numberAt(const Sw6Values& values, qsizetype index)
{
    if (index >= values.size() || values.at(index).isText()) {
        return std::nullopt;
    }
    return values.at(index).asDouble();
}

/// Reads `<轴>s,<值>f` pairs into a pose, starting at `from`.
quint32 readAxisPairs(const Sw6Values& values, qsizetype from,
                      std::array<double, kAxisCount>& pose)
{
    for (qsizetype cursor = from; cursor < values.size(); cursor += 2) {
        const int axis = values.at(cursor).isText() ? axisIndex(values.at(cursor).label) : -1;
        if (axis < 0) {
            return err::kBadArgumentValue;
        }
        if (cursor + 1 >= values.size()) {
            return err::kMissingArgument;
        }
        if (values.at(cursor + 1).isText()) {
            return err::kArgumentTypeMismatch;
        }
        pose[static_cast<std::size_t>(axis)] = values.at(cursor + 1).asDouble();
    }
    return err::kSuccess;
}

/// `<name>s,Xs,<x>f,...,Ws,<w>f`, the reply shape of `$KSTq` and `$GEOq`.
Sw6Values poseReply(const QString& label, const std::array<double, kAxisCount>& pose)
{
    Sw6Values values;
    values.reserve(1 + 2 * kAxisCount);
    values.append(Sw6Value::ofText(label));
    for (int axis = 0; axis < kAxisCount; ++axis) {
        values.append(Sw6Value::ofText(axisName(axis)));
        values.append(Sw6Value::ofFloat(pose[static_cast<std::size_t>(axis)]));
    }
    return values;
}

} // namespace

// --- Axis commands ---------------------------------------------------------

AxisCommandHandler::AxisCommandHandler(std::shared_ptr<Sw6DeviceState> state)
    : state_(std::move(state))
{
}

Result<MessagePtr> AxisCommandHandler::handle(const AxisCommand& command, ExecutionContext& context)
{
    const AxisCommandSpec* spec = findAxisCommand(commandOpcodeOf(command.command));
    if (spec == nullptr) {
        return Sw6Reply::failure(command.command, err::kUnknownCommandName);
    }
    if (command.argumentError != err::kSuccess) {
        return Sw6Reply::failure(command.command, command.argumentError);
    }

    const int limit = axisLimitFor(spec->field);
    const QVector<int> axes = requestedAxes(command.elements, limit);
    for (const int index : axes) {
        if (index >= limit) {
            return Sw6Reply::failure(command.command, err::kBadArgumentValue);
        }
    }

    if (spec->op == AxisOp::Query) {
        // Most queries answer one value per axis; the ones whose set command
        // writes several - `$FSFq` returns the three `$FSF` forces - read that
        // many consecutive fields.
        Sw6Values values;
        values.reserve(axes.size() * (1 + spec->valuesPerElement));
        for (const int index : axes) {
            values.append(Sw6Value::ofText(axisName(index)));
            for (quint8 offset = 0; offset < spec->valuesPerElement; ++offset) {
                const auto field = static_cast<AxisField>(static_cast<quint8>(spec->field) + offset);
                values.append(quantity(state_->axis(field)[static_cast<std::size_t>(index)],
                                       spec->valueKind));
            }
        }
        return Sw6Reply::data(command.command, std::move(values));
    }

    if (spec->op == AxisOp::MoveRelativeTool || spec->op == AxisOp::MoveRelativeWork) {
        return moveInFrame(*spec, command, context);
    }

    // Everything below writes something, so leaving the axis out is only
    // allowed where the protocol defines it as "all six".
    const bool needsValues = spec->valuesPerElement > 0;
    if (needsValues && command.elements.isEmpty()) {
        return Sw6Reply::failure(command.command, err::kMissingArgument);
    }
    for (const Sw6Element& element : command.elements) {
        if (needsValues && element.values.isEmpty()) {
            return Sw6Reply::failure(command.command, err::kMissingArgument);
        }
    }

    // Two passes: the request is validated and turned into concrete targets
    // first, so a rejected axis cannot leave a half-applied multi-axis move
    // behind.
    QVector<double> targets;
    targets.reserve(axes.size());

    for (const int index : axes) {
        const auto slot = static_cast<std::size_t>(index);
        const double value = valueFor(command.elements, index);

        switch (spec->op) {
        case AxisOp::Set:
            if (const auto cap = capFieldFor(spec->field);
                cap.has_value() && std::abs(value) > state_->axis(*cap)[slot]) {
                return Sw6Reply::failure(command.command, err::kBadArgumentValue);
            }
            targets.append(value);
            break;

        case AxisOp::MoveAbsolute:
        case AxisOp::MoveRelative:
        case AxisOp::MoveToLimit:
        case AxisOp::GoHome: {
            if (const quint32 code = state_->checkMotionAllowed(index); code != err::kSuccess) {
                return Sw6Reply::failure(command.command, code);
            }
            double target = value;
            if (spec->op == AxisOp::MoveRelative) {
                target += state_->axis(AxisField::TargetPose)[slot];
            } else if (spec->op == AxisOp::GoHome) {
                target = 0.0;
            } else if (spec->op == AxisOp::MoveToLimit) {
                target = state_->axis(spec->field)[slot];
            }
            if (spec->op != AxisOp::MoveToLimit || limitMoveIsSoft(spec->field)) {
                if (const quint32 code = state_->checkSoftLimit(index, target);
                    code != err::kSuccess) {
                    return Sw6Reply::failure(command.command, code);
                }
            }
            targets.append(target);
            break;
        }

        case AxisOp::Jog:
        case AxisOp::Excite:
        case AxisOp::Optimise:
            // An excitation or a tuning run drives the platform, so it needs
            // the same servo and reference state a move does.
            if (const quint32 code = state_->checkMotionAllowed(index); code != err::kSuccess) {
                return Sw6Reply::failure(command.command, code);
            }
            targets.append(value);
            break;

        // Handled before this loop, and listed so the switch stays exhaustive.
        case AxisOp::MoveRelativeTool:
        case AxisOp::MoveRelativeWork:
        case AxisOp::Query:
            targets.append(value);
            break;
        }
    }

    for (qsizetype position = 0; position < axes.size(); ++position) {
        const int index = axes.at(position);
        const double target = targets.at(position);

        switch (spec->op) {
        case AxisOp::Set:
            state_->axis(spec->field)[static_cast<std::size_t>(index)] = target;
            break;
        case AxisOp::MoveAbsolute:
        case AxisOp::MoveRelative:
        case AxisOp::MoveToLimit:
        case AxisOp::GoHome:
            state_->applyPose(index, target);
            break;
        case AxisOp::Jog:
            state_->jog(index, target);
            break;
        case AxisOp::Excite:
            // What `$IMPq` / `$STEq` report back, per section 4.5 rule 2.
            state_->axis(spec->field)[static_cast<std::size_t>(index)] = target;
            break;
        case AxisOp::Optimise:
            // `$DPO` retunes inside the firmware; there is nothing here to
            // model beyond the precondition check the request already passed.
            break;
        case AxisOp::MoveRelativeTool:
        case AxisOp::MoveRelativeWork:
        case AxisOp::Query:
            break;
        }
    }

    state_->publishTo(context.device());
    return Sw6Reply::ack(command.command);
}

Result<MessagePtr> AxisCommandHandler::moveInFrame(const AxisCommandSpec& spec,
                                                   const AxisCommand& command,
                                                   ExecutionContext& context)
{
    if (command.elements.isEmpty()) {
        return Sw6Reply::failure(command.command, err::kMissingArgument);
    }
    for (const Sw6Element& element : command.elements) {
        if (element.values.isEmpty()) {
            return Sw6Reply::failure(command.command, err::kMissingArgument);
        }
    }

    const FrameKind kind =
        spec.op == AxisOp::MoveRelativeTool ? FrameKind::Tool : FrameKind::Work;
    const Sw6Frame* active = state_->enabledFrame(kind);
    if (active == nullptr) {
        return Sw6Reply::failure(command.command, err::kNoSuchEntry);
    }

    std::array<double, kAxisCount> offset{};
    for (const Sw6Element& element : command.elements) {
        offset[static_cast<std::size_t>(element.index)] = element.values.first().asDouble();
    }

    // A rotated frame turns a single-axis request into a move on several axes,
    // so every axis the transform touches is validated before any of it runs.
    const std::array<double, kAxisCount> rotated = offsetInFrame(active->pose, offset);

    std::array<double, kAxisCount> targets{};
    for (std::size_t index = 0; index < kAxisCount; ++index) {
        targets[index] = state_->axis(AxisField::TargetPose)[index] + rotated[index];
        if (rotated[index] == 0.0) {
            continue;
        }
        const int axis = static_cast<int>(index);
        if (const quint32 code = state_->checkMotionAllowed(axis); code != err::kSuccess) {
            return Sw6Reply::failure(command.command, code);
        }
        if (const quint32 code = state_->checkSoftLimit(axis, targets[index]);
            code != err::kSuccess) {
            return Sw6Reply::failure(command.command, code);
        }
    }

    for (std::size_t index = 0; index < kAxisCount; ++index) {
        if (rotated[index] != 0.0) {
            state_->applyPose(static_cast<int>(index), targets[index]);
        }
    }

    state_->publishTo(context.device());
    return Sw6Reply::ack(command.command);
}

// --- Leg commands ----------------------------------------------------------

LegCommandHandler::LegCommandHandler(std::shared_ptr<Sw6DeviceState> state)
    : state_(std::move(state))
{
}

Result<MessagePtr> LegCommandHandler::handle(const LegCommand& command, ExecutionContext& context)
{
    const LegCommandSpec* spec = findLegCommand(commandOpcodeOf(command.command));
    if (spec == nullptr) {
        return Sw6Reply::failure(command.command, err::kUnknownCommandName);
    }
    if (command.argumentError != err::kSuccess) {
        return Sw6Reply::failure(command.command, command.argumentError);
    }

    const QVector<int> legs = requestedLegs(command.elements);

    switch (spec->op) {
    case LegOp::Query: {
        Sw6Values values;
        values.reserve(legs.size() * (1 + spec->valuesPerElement));
        for (const int leg : legs) {
            values.append(Sw6Value::ofInt(leg));
            for (quint8 offset = 0; offset < spec->valuesPerElement; ++offset) {
                const auto field = static_cast<LegField>(static_cast<quint8>(spec->field) + offset);
                values.append(quantity(state_->leg(field)[static_cast<std::size_t>(leg - 1)],
                                       spec->valueKind));
            }
        }
        return Sw6Reply::data(command.command, std::move(values));
    }

    case LegOp::Reference:
        if (state_->emergencyStopped()) {
            return Sw6Reply::failure(command.command, err::kEmergencyStopped);
        }
        for (const int leg : legs) {
            state_->reference(leg);
        }
        break;

    case LegOp::Set:
        if (command.elements.isEmpty()) {
            return Sw6Reply::failure(command.command, err::kMissingArgument);
        }
        for (const Sw6Element& element : command.elements) {
            if (element.values.size() < spec->valuesPerElement) {
                return Sw6Reply::failure(command.command, err::kMissingArgument);
            }
        }
        for (const Sw6Element& element : command.elements) {
            for (quint8 offset = 0; offset < spec->valuesPerElement; ++offset) {
                const auto field = static_cast<LegField>(static_cast<quint8>(spec->field) + offset);
                state_->leg(field)[static_cast<std::size_t>(element.index - 1)] =
                    element.values.at(offset).asDouble();
            }
        }
        break;
    }

    state_->publishTo(context.device());
    return Sw6Reply::ack(command.command);
}

// --- Analog inputs and I/O lines -------------------------------------------

ChannelCommandHandler::ChannelCommandHandler(std::shared_ptr<Sw6DeviceState> state)
    : state_(std::move(state))
{
}

Result<MessagePtr> ChannelCommandHandler::handle(const ChannelCommand& command,
                                                 ExecutionContext& context)
{
    const ChannelCommandSpec* spec = findChannelCommand(commandOpcodeOf(command.command));
    if (spec == nullptr) {
        return Sw6Reply::failure(command.command, err::kUnknownCommandName);
    }
    if (command.argumentError != err::kSuccess) {
        return Sw6Reply::failure(command.command, command.argumentError);
    }

    const ChannelRange range = channelRangeOf(spec->field);
    const QVector<int> elements = requestedElements(command.elements, range);

    switch (spec->op) {
    case ChannelOp::Query: {
        Sw6Values values;
        values.reserve(elements.size() * 2);
        for (const int element : elements) {
            values.append(Sw6Value::ofInt(element));
            values.append(quantity(state_->channel(spec->field, element), spec->valueKind));
        }
        return Sw6Reply::data(command.command, std::move(values));
    }

    case ChannelOp::QueryConfig: {
        // Only the parameters that were actually configured come back: the
        // protocol does not define the parameter set, so the device has
        // nothing to report for a line nobody has written to.
        Sw6Values values;
        for (const int line : elements) {
            const QMap<int, double> parameters = state_->triggerConfig(spec->field, line);
            for (auto it = parameters.constBegin(); it != parameters.constEnd(); ++it) {
                values.append(Sw6Value::ofInt(line));
                values.append(Sw6Value::ofInt(it.key()));
                values.append(Sw6Value::ofFloat(it.value()));
            }
        }
        fitToFrame(command.command, values, 3);
        return Sw6Reply::data(command.command, std::move(values));
    }

    case ChannelOp::Set:
        if (command.elements.isEmpty()) {
            return Sw6Reply::failure(command.command, err::kMissingArgument);
        }
        // Every writable line quantity is a logic level, which section 4.5
        // rule 6 fixes at 0 or 1.
        for (const Sw6Element& element : command.elements) {
            const qint64 level = element.values.first().asInt();
            if (level != 0 && level != 1) {
                return Sw6Reply::failure(command.command, err::kBadArgumentValue);
            }
        }
        for (const Sw6Element& element : command.elements) {
            state_->setChannel(spec->field, element.index,
                               static_cast<double>(element.values.first().asInt()));
        }
        break;

    case ChannelOp::SetConfig:
        if (command.elements.isEmpty()) {
            return Sw6Reply::failure(command.command, err::kMissingArgument);
        }
        for (const Sw6Element& element : command.elements) {
            const qint64 parameter = element.values.first().asInt();
            if (parameter < 1 || parameter > kTriggerParameterCount) {
                return Sw6Reply::failure(command.command, err::kBadArgumentValue);
            }
        }
        for (const Sw6Element& element : command.elements) {
            state_->setTriggerParameter(spec->field, element.index,
                                        static_cast<int>(element.values.first().asInt()),
                                        element.values.at(1).asDouble());
        }
        break;
    }

    state_->publishTo(context.device());
    return Sw6Reply::ack(command.command);
}

// --- Coordinate systems ----------------------------------------------------

CoordinateCommandHandler::CoordinateCommandHandler(std::shared_ptr<Sw6DeviceState> state)
    : state_(std::move(state))
{
}

Result<MessagePtr> CoordinateCommandHandler::handle(const CoordinateCommand& command,
                                                    ExecutionContext& context)
{
    const CoordinateCommandSpec* spec = findCoordinateCommand(commandOpcodeOf(command.command));
    if (spec == nullptr) {
        return Sw6Reply::failure(command.command, err::kUnknownCommandName);
    }
    if (command.argumentError != err::kSuccess) {
        return Sw6Reply::failure(command.command, command.argumentError);
    }

    const QString& name = command.command;
    const Sw6Values& args = command.values;

    // `KST`/`KSW` carry the frame name first; every other command names the
    // frame type first and the frame second.
    const bool typeFirst =
        spec->op != CoordinateOp::DefineTool && spec->op != CoordinateOp::QueryTool
        && spec->op != CoordinateOp::DefineWork && spec->op != CoordinateOp::QueryWork;

    FrameKind kind = FrameKind::Tool;
    QString frameName;
    if (typeFirst) {
        const auto parsed = frameKindFromName(textAt(args, 0));
        if (!parsed.has_value()) {
            return Sw6Reply::failure(name, err::kBadArgumentValue);
        }
        kind = *parsed;
        frameName = textAt(args, 1);
    } else {
        kind = (spec->op == CoordinateOp::DefineTool || spec->op == CoordinateOp::QueryTool)
                   ? FrameKind::Tool
                   : FrameKind::Work;
        frameName = textAt(args, 0);
    }

    const bool needsFrameName = spec->op != CoordinateOp::List;
    if (needsFrameName && frameName.isEmpty()) {
        return Sw6Reply::failure(name, err::kBadArgumentValue);
    }

    switch (spec->op) {
    case CoordinateOp::DefineTool:
    case CoordinateOp::DefineWork: {
        // Modifying an existing frame keeps the axes the request left out.
        std::array<double, kAxisCount> pose{};
        if (const Sw6Frame* existing = state_->frame(kind, frameName); existing != nullptr) {
            pose = existing->pose;
        }
        if (const quint32 code = readAxisPairs(args, 1, pose); code != err::kSuccess) {
            return Sw6Reply::failure(name, code);
        }
        state_->defineFrame(kind, frameName).pose = pose;
        break;
    }

    case CoordinateOp::QueryTool:
    case CoordinateOp::QueryWork: {
        const Sw6Frame* existing = state_->frame(kind, frameName);
        if (existing == nullptr) {
            return Sw6Reply::failure(name, err::kNoSuchEntry);
        }
        return Sw6Reply::data(name, poseReply(frameName, existing->pose));
    }

    case CoordinateOp::DefineFromPose:
        state_->defineFrame(kind, frameName).pose = state_->axis(AxisField::ActualPose);
        break;

    case CoordinateOp::Enable:
        if (state_->frame(kind, frameName) == nullptr) {
            return Sw6Reply::failure(name, err::kNoSuchEntry);
        }
        state_->setFrameEnabled(kind, frameName, args.at(2).asInt() != 0);
        break;

    case CoordinateOp::QueryEnabled: {
        const Sw6Frame* existing = state_->frame(kind, frameName);
        if (existing == nullptr) {
            return Sw6Reply::failure(name, err::kNoSuchEntry);
        }
        return Sw6Reply::data(name, {Sw6Value::ofText(frameKindName(kind)),
                                     Sw6Value::ofText(frameName),
                                     Sw6Value::ofInt(existing->enabled ? 1 : 0)});
    }

    case CoordinateOp::Remove: {
        const Sw6Frame* existing = state_->frame(kind, frameName);
        if (existing == nullptr) {
            return Sw6Reply::failure(name, err::kNoSuchEntry);
        }
        if (existing->enabled) {
            return Sw6Reply::failure(name, err::kBadArgumentValue);
        }
        state_->removeFrame(kind, frameName);
        break;
    }

    case CoordinateOp::Copy: {
        const Sw6Frame* source = state_->frame(kind, frameName);
        const QString target = textAt(args, 2);
        if (source == nullptr) {
            return Sw6Reply::failure(name, err::kNoSuchEntry);
        }
        if (target.isEmpty()) {
            return Sw6Reply::failure(name, err::kBadArgumentValue);
        }

        // A copy is a new, inactive frame: two enabled tool systems would be
        // ambiguous.
        Sw6Frame copy = *source;
        copy.enabled = false;
        state_->defineFrame(kind, target) = copy;
        break;
    }

    case CoordinateOp::List: {
        Sw6Values values{Sw6Value::ofText(frameKindName(kind))};
        for (const QString& entry : state_->frameNames(kind)) {
            values.append(Sw6Value::ofText(entry));
        }
        return Sw6Reply::data(name, std::move(values));
    }

    case CoordinateOp::Link: {
        if (state_->frame(kind, frameName) == nullptr) {
            return Sw6Reply::failure(name, err::kNoSuchEntry);
        }

        const QString parentType = textAt(args, 2).toUpper();
        const QString parentName = textAt(args, 3);
        if (parentType.isEmpty() || parentName.isEmpty()) {
            return Sw6Reply::failure(name, err::kBadArgumentValue);
        }

        const auto parentKind = frameKindFromName(parentType);
        if (!parentKind.has_value()) {
            // Anything that is not TOOL or WORK has to be BASE, which is the
            // root and means "unlink".
            if (parentType != QStringLiteral("BASE")) {
                return Sw6Reply::failure(name, err::kBadArgumentValue);
            }
            Sw6Frame& target = state_->defineFrame(kind, frameName);
            target.parentType = QStringLiteral("BASE");
            target.parentName = QStringLiteral("BASE");
            break;
        }

        if (*parentKind == kind && parentName == frameName) {
            return Sw6Reply::failure(name, err::kBadArgumentValue);
        }
        if (state_->frame(*parentKind, parentName) == nullptr) {
            return Sw6Reply::failure(name, err::kNoSuchEntry);
        }
        if (state_->linkWouldCycle(kind, frameName, parentType, parentName)) {
            return Sw6Reply::failure(name, err::kBadArgumentValue);
        }

        Sw6Frame& target = state_->defineFrame(kind, frameName);
        target.parentType = parentType;
        target.parentName = parentName;
        break;
    }

    case CoordinateOp::QueryLink: {
        const Sw6Frame* existing = state_->frame(kind, frameName);
        if (existing == nullptr) {
            return Sw6Reply::failure(name, err::kNoSuchEntry);
        }
        return Sw6Reply::data(name, {Sw6Value::ofText(frameKindName(kind)),
                                     Sw6Value::ofText(frameName),
                                     Sw6Value::ofText(existing->parentType),
                                     Sw6Value::ofText(existing->parentName)});
    }
    }

    state_->publishTo(context.device());
    return Sw6Reply::ack(name);
}

// --- Named parameters ------------------------------------------------------

NamedCommandHandler::NamedCommandHandler(std::shared_ptr<Sw6DeviceState> state)
    : state_(std::move(state))
{
}

Result<MessagePtr> NamedCommandHandler::handle(const NamedCommand& command,
                                               ExecutionContext& context)
{
    const NamedCommandSpec* spec = findNamedCommand(commandOpcodeOf(command.command));
    if (spec == nullptr) {
        return Sw6Reply::failure(command.command, err::kUnknownCommandName);
    }
    if (command.argumentError != err::kSuccess) {
        return Sw6Reply::failure(command.command, command.argumentError);
    }

    const QString& name = command.command;
    const Sw6Values& args = command.values;

    const auto specOf = [](const QString& parameter) -> const NamedParameterSpec* {
        for (const NamedParameterSpec& entry : kNamedParameters) {
            if (parameter.compare(QString::fromUtf8(entry.name.data(),
                                                    static_cast<qsizetype>(entry.name.size())),
                                  Qt::CaseSensitive)
                == 0) {
                return &entry;
            }
        }
        return nullptr;
    };

    switch (spec->op) {
    case NamedOp::SetParameter: {
        if (args.size() % 2 != 0) {
            return Sw6Reply::failure(name, err::kMissingArgument);
        }
        for (qsizetype cursor = 0; cursor < args.size(); cursor += 2) {
            if (!args.at(cursor).isText()) {
                return Sw6Reply::failure(name, err::kBadArgumentValue);
            }
            const NamedParameterSpec* parameter = specOf(args.at(cursor).label);
            if (parameter == nullptr) {
                return Sw6Reply::failure(name, err::kNoSuchEntry);
            }

            // Section 5.8: the suffix has to match the registered type.
            const Sw6Value& value = args.at(cursor + 1);
            const bool matches = parameter->kind == ValueKind::Integer
                                     ? value.kind == Sw6Value::Kind::Integer
                                     : value.kind == Sw6Value::Kind::Float;
            if (!matches) {
                return Sw6Reply::failure(name, err::kArgumentTypeMismatch);
            }
        }
        for (qsizetype cursor = 0; cursor < args.size(); cursor += 2) {
            state_->setParameter(args.at(cursor).label, args.at(cursor + 1).asDouble());
        }
        break;
    }

    case NamedOp::QueryParameter: {
        Sw6Values values;
        if (args.isEmpty()) {
            for (const NamedParameterSpec& parameter : kNamedParameters) {
                const QString parameterName = QString::fromUtf8(
                    parameter.name.data(), static_cast<qsizetype>(parameter.name.size()));
                values.append(Sw6Value::ofText(parameterName));
                values.append(quantity(state_->parameter(parameterName).value_or(
                                           parameter.defaultValue),
                                       parameter.kind));
            }
            return Sw6Reply::data(name, std::move(values));
        }

        for (const Sw6Value& requested : args) {
            const NamedParameterSpec* parameter =
                requested.isText() ? specOf(requested.label) : nullptr;
            if (parameter == nullptr) {
                return Sw6Reply::failure(name, err::kNoSuchEntry);
            }
            values.append(Sw6Value::ofText(requested.label));
            values.append(quantity(state_->parameter(requested.label)
                                       .value_or(parameter->defaultValue),
                                   parameter->kind));
        }
        return Sw6Reply::data(name, std::move(values));
    }

    case NamedOp::QueryKinematic: {
        const auto scalarOf = [](const QString& scalar) -> const KinematicScalar* {
            for (const KinematicScalar& entry : kKinematicScalars) {
                if (scalar
                    == QString::fromUtf8(entry.name.data(),
                                         static_cast<qsizetype>(entry.name.size()))) {
                    return &entry;
                }
            }
            return nullptr;
        };

        Sw6Values values;
        if (args.isEmpty()) {
            for (const KinematicScalar& scalar : kKinematicScalars) {
                values.append(Sw6Value::ofText(QString::fromUtf8(
                    scalar.name.data(), static_cast<qsizetype>(scalar.name.size()))));
                values.append(Sw6Value::ofFloat(scalar.value));
            }
            return Sw6Reply::data(name, std::move(values));
        }

        for (const Sw6Value& requested : args) {
            const KinematicScalar* scalar = requested.isText() ? scalarOf(requested.label) : nullptr;
            if (scalar == nullptr) {
                return Sw6Reply::failure(name, err::kNoSuchEntry);
            }
            values.append(Sw6Value::ofText(requested.label));
            values.append(Sw6Value::ofFloat(scalar->value));
        }
        return Sw6Reply::data(name, std::move(values));
    }

    case NamedOp::QueryGeometryRow: {
        const QString row = textAt(args, 0);
        const auto hinge = row.isEmpty() ? std::nullopt : state_->geometryRow(row);
        if (!hinge.has_value()) {
            return Sw6Reply::failure(name, err::kNoSuchEntry);
        }
        return Sw6Reply::data(name, poseReply(row, *hinge));
    }
    }

    state_->publishTo(context.device());
    return Sw6Reply::ack(name);
}

// --- Alignment and identification ------------------------------------------

AlignmentCommandHandler::AlignmentCommandHandler(std::shared_ptr<Sw6DeviceState> state)
    : state_(std::move(state))
{
}

Result<MessagePtr> AlignmentCommandHandler::handle(const AlignmentCommand& command,
                                                   ExecutionContext& context)
{
    const AlignmentCommandSpec* spec = findAlignmentCommand(commandOpcodeOf(command.command));
    if (spec == nullptr) {
        return Sw6Reply::failure(command.command, err::kUnknownCommandName);
    }
    if (command.argumentError != err::kSuccess) {
        return Sw6Reply::failure(command.command, command.argumentError);
    }

    const QString& name = command.command;
    const Sw6Values& args = command.values;

    // Processes the request addresses: the one it names, or all of them.
    const auto requested = [this, &args]() -> QStringList {
        const QString named = textAt(args, 0);
        return named.isEmpty() ? state_->processNames() : QStringList{named};
    };

    switch (spec->op) {
    case AlignmentOp::DefineGradient:
    case AlignmentOp::DefineRaster:
        return defineSearch(*spec, command);

    case AlignmentOp::ScanAxis:
    case AlignmentOp::ScanPlane: {
        // A scan moves the platform, so the parameter tree follows it out.
        Result<MessagePtr> scanned = runScan(*spec, command);
        state_->publishTo(context.device());
        return scanned;
    }

    case AlignmentOp::SurfaceDetect: {
        Result<MessagePtr> detected = detectSurface(command);
        state_->publishTo(context.device());
        return detected;
    }

    case AlignmentOp::SetCentre: {
        const QString processName = textAt(args, 0);
        const auto scanCentre = numberAt(args, 1);
        const auto stepCentre = numberAt(args, 2);
        if (!scanCentre.has_value() || !stepCentre.has_value()) {
            return Sw6Reply::failure(name, err::kBadArgumentValue);
        }
        if (state_->process(processName) == nullptr) {
            return Sw6Reply::failure(name, err::kNoSuchEntry);
        }
        Sw6Process& search = state_->defineProcess(processName);
        search.scanCentre = *scanCentre;
        search.stepCentre = *stepCentre;
        break;
    }

    case AlignmentOp::QueryCentre: {
        Sw6Values values;
        for (const QString& processName : requested()) {
            const Sw6Process* search = state_->process(processName);
            if (search == nullptr) {
                return Sw6Reply::failure(name, err::kNoSuchEntry);
            }
            values.append(Sw6Value::ofText(processName));
            values.append(Sw6Value::ofFloat(search->scanCentre));
            values.append(Sw6Value::ofFloat(search->stepCentre));
        }
        fitToFrame(name, values, 3);
        return Sw6Reply::data(name, std::move(values));
    }

    case AlignmentOp::Start: {
        const QString processName = textAt(args, 0);
        if (state_->process(processName) == nullptr) {
            return Sw6Reply::failure(name, err::kNoSuchEntry);
        }
        Sw6Process& search = state_->defineProcess(processName);
        for (const int index : {search.scanAxis, search.stepAxis}) {
            if (const quint32 code = state_->checkMotionAllowed(index); code != err::kSuccess) {
                return Sw6Reply::failure(name, code);
            }
        }

        search.result[0] =
            state_->scanTowardsPeak(search.scanAxis, search.scanRange, search.scanCentre);
        search.result[1] =
            state_->scanTowardsPeak(search.stepAxis, search.stepRange, search.stepCentre);
        search.result[2] = state_->signalStrength();

        // An alignment keeps tracking the peak once it has found it, so the
        // process stays running until `$FRP,<proc>s,0d` stops it - which is
        // also what keeps status word bit 6 meaningful.
        search.state = 1;
        break;
    }

    case AlignmentOp::Control: {
        const QString processName = textAt(args, 0);
        if (state_->process(processName) == nullptr) {
            return Sw6Reply::failure(name, err::kNoSuchEntry);
        }
        Sw6Process& search = state_->defineProcess(processName);
        switch (args.at(1).asInt()) {
        case 0:
            search.state = 3;
            break;
        case 1:
            if (search.state != 1) {
                return Sw6Reply::failure(name, err::kBadArgumentValue);
            }
            search.state = 2;
            break;
        case 2:
            if (search.state != 2) {
                return Sw6Reply::failure(name, err::kBadArgumentValue);
            }
            search.state = 1;
            break;
        default:
            return Sw6Reply::failure(name, err::kBadArgumentValue);
        }
        break;
    }

    case AlignmentOp::QueryState: {
        Sw6Values values;
        for (const QString& processName : requested()) {
            const Sw6Process* search = state_->process(processName);
            if (search == nullptr) {
                return Sw6Reply::failure(name, err::kNoSuchEntry);
            }
            values.append(Sw6Value::ofText(processName));
            values.append(Sw6Value::ofInt(search->state));
        }
        fitToFrame(name, values, 2);
        return Sw6Reply::data(name, std::move(values));
    }

    case AlignmentOp::QueryResult: {
        const Sw6Process* search = state_->process(textAt(args, 0));
        if (search == nullptr) {
            return Sw6Reply::failure(name, err::kNoSuchEntry);
        }

        Sw6Values values{Sw6Value::ofText(textAt(args, 0))};
        if (args.size() > 1) {
            const qint64 result = args.at(1).asInt();
            if (args.at(1).isText() || result < 1 || result > kProcessResultCount) {
                return Sw6Reply::failure(name, err::kBadArgumentValue);
            }
            values.append(Sw6Value::ofFloat(search->result[static_cast<std::size_t>(result - 1)]));
        } else {
            for (const double value : search->result) {
                values.append(Sw6Value::ofFloat(value));
            }
        }
        return Sw6Reply::data(name, std::move(values));
    }

    case AlignmentOp::QueryResultHelp:
        // Names of the `$FRRq` result ids, in id order.
        return Sw6Reply::data(name, {Sw6Value::ofText(QStringLiteral("SCAN")),
                                     Sw6Value::ofText(QStringLiteral("STEP")),
                                     Sw6Value::ofText(QStringLiteral("SIGNAL"))});

    case AlignmentOp::SetCoupling: {
        const QString base = textAt(args, 0);
        const QString coupled = textAt(args, 1);
        if (base.isEmpty() || coupled.isEmpty() || base == coupled) {
            return Sw6Reply::failure(name, err::kBadArgumentValue);
        }
        if (state_->process(base) == nullptr || state_->process(coupled) == nullptr) {
            return Sw6Reply::failure(name, err::kNoSuchEntry);
        }
        state_->defineProcess(base).coupled = coupled;
        break;
    }

    case AlignmentOp::QueryCoupling: {
        Sw6Values values;
        for (const QString& processName : requested()) {
            const Sw6Process* search = state_->process(processName);
            if (search == nullptr) {
                return Sw6Reply::failure(name, err::kNoSuchEntry);
            }
            values.append(Sw6Value::ofText(processName));
            // A string argument cannot be empty on the wire, so an uncoupled
            // process reports the reserved name `NONE`.
            values.append(Sw6Value::ofText(
                search->coupled.isEmpty() ? QStringLiteral("NONE") : search->coupled));
        }
        fitToFrame(name, values, 2);
        return Sw6Reply::data(name, std::move(values));
    }

    case AlignmentOp::FrequencyResponse: {
        const int index = axisIndex(textAt(args, 0));
        const auto mode = numberAt(args, 1);
        const auto amplitude = numberAt(args, 2);
        const auto low = numberAt(args, 3);
        const auto high = numberAt(args, 4);
        if (index < 0 || !mode.has_value() || !amplitude.has_value() || !low.has_value()
            || !high.has_value() || *mode < 0.0 || *low <= 0.0 || *low >= *high) {
            return Sw6Reply::failure(name, err::kBadArgumentValue);
        }
        if (const quint32 code = state_->checkMotionAllowed(index); code != err::kSuccess) {
            return Sw6Reply::failure(name, code);
        }

        Sw6FrequencyResponse& sweep = state_->frequencyResponse(index);
        sweep.mode = static_cast<int>(*mode);
        sweep.amplitude = *amplitude;
        sweep.lowFrequency = *low;
        sweep.highFrequency = *high;
        break;
    }

    case AlignmentOp::QueryFrequencyResponse: {
        const int index = axisIndex(textAt(args, 0));
        const auto mode = numberAt(args, 1);
        if (index < 0 || !mode.has_value()) {
            return Sw6Reply::failure(name, err::kBadArgumentValue);
        }

        // The response data itself is firmware-defined, so the device answers
        // with the sweep that was run - and only for the mode it ran in.
        const Sw6FrequencyResponse& sweep = state_->frequencyResponse(index);
        if (sweep.mode != static_cast<int>(*mode)) {
            return Sw6Reply::failure(name, err::kNoSuchEntry);
        }
        return Sw6Reply::data(name, {Sw6Value::ofText(axisName(index)),
                                     Sw6Value::ofInt(sweep.mode),
                                     Sw6Value::ofFloat(sweep.amplitude),
                                     Sw6Value::ofFloat(sweep.lowFrequency),
                                     Sw6Value::ofFloat(sweep.highFrequency)});
    }

    case AlignmentOp::SetInputCalculation: {
        const auto input = numberAt(args, 0);
        const auto type = numberAt(args, 1);
        if (!input.has_value() || !type.has_value() || *type < 0.0) {
            return Sw6Reply::failure(name, err::kBadArgumentValue);
        }
        const int identifier = static_cast<int>(*input);
        if (identifier < 1 || identifier > kAnalogChannelCount) {
            return Sw6Reply::failure(name, err::kBadArgumentValue);
        }

        Sw6InputCalculation calculation;
        calculation.type = static_cast<int>(*type);
        calculation.parameters = args.mid(2);
        state_->setInputCalculation(identifier, std::move(calculation));
        break;
    }

    case AlignmentOp::QueryInputCalculation: {
        const auto input = numberAt(args, 0);
        QList<int> inputs = state_->configuredInputs();
        if (input.has_value()) {
            const int identifier = static_cast<int>(*input);
            if (identifier < 1 || identifier > kAnalogChannelCount) {
                return Sw6Reply::failure(name, err::kBadArgumentValue);
            }
            inputs = {identifier};
        }

        Sw6Values values;
        for (const int identifier : inputs) {
            const Sw6InputCalculation calculation =
                state_->inputCalculation(identifier).value_or(Sw6InputCalculation{});
            values.append(Sw6Value::ofInt(identifier));
            values.append(Sw6Value::ofInt(calculation.type));
            values.append(calculation.parameters);
        }
        return Sw6Reply::data(name, std::move(values));
    }

    case AlignmentOp::QueryInputValue: {
        const auto input = numberAt(args, 0);
        Sw6Values values;
        if (input.has_value()) {
            const int identifier = static_cast<int>(*input);
            if (identifier < 1 || identifier > kAnalogChannelCount) {
                return Sw6Reply::failure(name, err::kBadArgumentValue);
            }
            values.append(Sw6Value::ofInt(identifier));
            values.append(Sw6Value::ofFloat(state_->inputValue(identifier)));
        } else {
            for (int identifier = 1; identifier <= kAnalogChannelCount; ++identifier) {
                values.append(Sw6Value::ofInt(identifier));
                values.append(Sw6Value::ofFloat(state_->inputValue(identifier)));
            }
        }
        return Sw6Reply::data(name, std::move(values));
    }
    }

    state_->publishTo(context.device());
    return Sw6Reply::ack(name);
}

Result<MessagePtr> AlignmentCommandHandler::defineSearch(const AlignmentCommandSpec& spec,
                                                         const AlignmentCommand& command)
{
    const Sw6Values& args = command.values;
    const QString processName = textAt(args, 0);
    if (processName.isEmpty()) {
        return Sw6Reply::failure(command.command, err::kBadArgumentValue);
    }

    // A gradient search follows the signal wherever it leads, so it is defined
    // by its two axes alone; a raster or first-light search is given the
    // window it may cover on each of them.
    const bool windowed = spec.op == AlignmentOp::DefineRaster;
    const int scanAxis = axisIndex(textAt(args, 1));
    const int stepAxis = axisIndex(textAt(args, windowed ? 3 : 2));
    const std::optional<double> scanRange = windowed ? numberAt(args, 2) : std::optional{0.0};
    const std::optional<double> stepRange = windowed ? numberAt(args, 4) : std::optional{0.0};
    if (scanAxis < 0 || stepAxis < 0 || !scanRange.has_value() || !stepRange.has_value()) {
        return Sw6Reply::failure(command.command, err::kBadArgumentValue);
    }

    Sw6Process& search = state_->defineProcess(processName);
    search.definedBy = command.command;
    search.scanAxis = scanAxis;
    search.stepAxis = stepAxis;
    search.scanRange = *scanRange;
    search.stepRange = *stepRange;
    search.state = 0;
    search.result = {};
    return Sw6Reply::ack(command.command);
}

Result<MessagePtr> AlignmentCommandHandler::runScan(const AlignmentCommandSpec& spec,
                                                    const AlignmentCommand& command)
{
    const Sw6Values& args = command.values;

    const int firstAxis = axisIndex(textAt(args, 0));
    const auto firstSpan = numberAt(args, 1);
    if (firstAxis < 0 || !firstSpan.has_value()) {
        return Sw6Reply::failure(command.command, err::kBadArgumentValue);
    }

    int secondAxis = -1;
    double secondSpan = 0.0;
    if (spec.op == AlignmentOp::ScanPlane) {
        secondAxis = axisIndex(textAt(args, 2));
        const auto span = numberAt(args, 3);
        if (secondAxis < 0 || !span.has_value()) {
            return Sw6Reply::failure(command.command, err::kBadArgumentValue);
        }
        secondSpan = *span;
    }

    for (const int index : {firstAxis, secondAxis}) {
        if (index < 0) {
            continue;
        }
        if (const quint32 code = state_->checkMotionAllowed(index); code != err::kSuccess) {
            return Sw6Reply::failure(command.command, code);
        }
    }

    // The trailing arguments - thresholds, step sizes, the I/O line a scan
    // watches - are firmware-defined, so the device model only uses the axes
    // and the window they may move in.
    state_->scanTowardsPeak(firstAxis, *firstSpan, 0.0);
    if (secondAxis >= 0) {
        state_->scanTowardsPeak(secondAxis, secondSpan, 0.0);
    }
    return Sw6Reply::ack(command.command);
}

Result<MessagePtr> AlignmentCommandHandler::detectSurface(const AlignmentCommand& command)
{
    const Sw6Values& args = command.values;

    const int index = axisIndex(textAt(args, 0));
    const auto force = numberAt(args, 1);
    const auto bias = numberAt(args, 2);
    if (index < 0 || !force.has_value() || !bias.has_value()) {
        return Sw6Reply::failure(command.command, err::kBadArgumentValue);
    }
    if (const quint32 code = state_->checkMotionAllowed(index); code != err::kSuccess) {
        return Sw6Reply::failure(command.command, code);
    }
    if (const quint32 code = state_->checkSoftLimit(index, *bias); code != err::kSuccess) {
        return Sw6Reply::failure(command.command, code);
    }

    const auto slot = static_cast<std::size_t>(index);
    state_->axis(AxisField::SurfaceForce1)[slot] = *force;
    state_->axis(AxisField::SurfaceOffset)[slot] = *bias;
    state_->axis(AxisField::SurfaceForce2)[slot] = numberAt(args, 3).value_or(0.0);

    // The simulated surface sits at the bias the request names, so the axis
    // stops there and that position is what `$FSRq` reports.
    state_->applyPose(index, *bias);
    state_->axis(AxisField::SurfaceResult)[slot] = *bias;
    return Sw6Reply::ack(command.command);
}

// --- Trajectories ----------------------------------------------------------

TrajectoryCommandHandler::TrajectoryCommandHandler(std::shared_ptr<Sw6DeviceState> state)
    : state_(std::move(state))
{
}

Result<MessagePtr> TrajectoryCommandHandler::handle(const TrajectoryCommand& command,
                                                    ExecutionContext& context)
{
    const TrajectoryCommandSpec* spec = findTrajectoryCommand(commandOpcodeOf(command.command));
    if (spec == nullptr) {
        return Sw6Reply::failure(command.command, err::kUnknownCommandName);
    }
    if (command.argumentError != err::kSuccess) {
        return Sw6Reply::failure(command.command, command.argumentError);
    }

    const QString& name = command.command;
    const Sw6Values& args = command.values;

    // Every command but `$TGT` / `$TGTq` starts with the trajectory number,
    // which may be left out by the queries and by `$TGC` to mean "all of them".
    const bool numbered = spec->op != TrajectoryOp::SetPeriod
                          && spec->op != TrajectoryOp::QueryPeriod;
    QVector<int> selected;
    if (numbered) {
        if (args.isEmpty()) {
            selected.reserve(kTrajectoryCount);
            for (int number = 1; number <= kTrajectoryCount; ++number) {
                selected.append(number);
            }
        } else {
            const qint64 number = args.first().isText() ? -1 : args.first().asInt();
            if (number < 1 || number > kTrajectoryCount) {
                return Sw6Reply::failure(name, err::kNoSuchEntry);
            }
            selected.append(static_cast<int>(number));
        }
    }

    switch (spec->op) {
    case TrajectoryOp::Clear:
        for (const int number : selected) {
            const Sw6Trajectory& target = state_->trajectory(number);
            if (target.state == TrajectoryState::Running
                || target.state == TrajectoryState::Paused) {
                return Sw6Reply::failure(name, err::kRejectedWhileMoving);
            }
        }
        for (const int number : selected) {
            state_->trajectory(number) = Sw6Trajectory{};
        }
        break;

    case TrajectoryOp::AddPoint: {
        Sw6Trajectory& target = state_->trajectory(selected.first());
        if (target.state == TrajectoryState::Running
            || target.state == TrajectoryState::Paused) {
            return Sw6Reply::failure(name, err::kRejectedWhileMoving);
        }

        const qint64 pointNumber = args.at(1).asInt();
        if (args.at(1).isText() || pointNumber < 0 || pointNumber >= kTrajectoryCapacity) {
            return Sw6Reply::failure(name, err::kBadArgumentValue);
        }
        for (qsizetype cursor = 2; cursor < 5; ++cursor) {
            if (args.at(cursor).isText()) {
                return Sw6Reply::failure(name, err::kArgumentTypeMismatch);
            }
        }

        // Section 5.12 rule 2: an axis the point leaves out keeps the value it
        // had at the point before, so the controller always holds a full pose.
        Sw6TrajectoryPoint point;
        if (const auto previous = target.points.constFind(static_cast<int>(pointNumber) - 1);
            previous != target.points.constEnd()) {
            point.pose = previous->pose;
        }
        point.interpolation = static_cast<int>(args.at(2).asInt());
        point.speed = args.at(3).asDouble();
        point.dwellMs = static_cast<int>(args.at(4).asInt());
        if (const quint32 code = readAxisPairs(args, 5, point.pose); code != err::kSuccess) {
            return Sw6Reply::failure(name, code);
        }

        target.points.insert(static_cast<int>(pointNumber), point);
        target.state = TrajectoryState::Writing;
        break;
    }

    case TrajectoryOp::Finish: {
        Sw6Trajectory& target = state_->trajectory(selected.first());
        if (target.state == TrajectoryState::Running
            || target.state == TrajectoryState::Paused) {
            return Sw6Reply::failure(name, err::kRejectedWhileMoving);
        }

        const Sw6TrajectoryCheck check = state_->checkTrajectory(selected.first());
        target.state = check.passed() ? TrajectoryState::Ready : TrajectoryState::Failed;

        // Both outcomes are a successful exchange; the verdict is the data.
        return Sw6Reply::data(name, {Sw6Value::ofInt(check.passed() ? 1 : 0),
                                     Sw6Value::ofInt(check.errorPoint),
                                     Sw6Value::ofInt(check.reason)});
    }

    case TrajectoryOp::Start: {
        const Sw6Trajectory& target = state_->trajectory(selected.first());
        if (target.state == TrajectoryState::Running
            || target.state == TrajectoryState::Paused) {
            return Sw6Reply::failure(name, err::kRejectedWhileMoving);
        }
        // Ready is what `$TGF` leaves behind; a trajectory that already ran
        // passed the same check and may run again.
        if (target.state != TrajectoryState::Ready
            && target.state != TrajectoryState::Completed
            && target.state != TrajectoryState::Stopped) {
            return Sw6Reply::failure(name, err::kBadArgumentValue);
        }
        for (int index = 0; index < kAxisCount; ++index) {
            if (const quint32 code = state_->checkMotionAllowed(index); code != err::kSuccess) {
                return Sw6Reply::failure(name, code);
            }
        }
        state_->runTrajectory(selected.first());
        break;
    }

    case TrajectoryOp::Control:
        if (!state_->controlTrajectory(selected.first(), args.at(1).asInt())) {
            return Sw6Reply::failure(name, err::kBadArgumentValue);
        }
        break;

    case TrajectoryOp::QueryInfo: {
        Sw6Values values;
        values.reserve(selected.size() * 6);
        for (const int number : selected) {
            const Sw6Trajectory& target = state_->trajectory(number);
            values.append(Sw6Value::ofInt(number));
            values.append(Sw6Value::ofInt(target.points.size()));
            values.append(Sw6Value::ofInt(static_cast<int>(target.state)));
            values.append(Sw6Value::ofInt(target.currentPoint));
            values.append(Sw6Value::ofInt(target.currentLoop));
            values.append(Sw6Value::ofInt(kTrajectoryCapacity - target.points.size()));
        }
        return Sw6Reply::data(name, std::move(values));
    }

    case TrajectoryOp::SetLoops: {
        const qint64 loops = args.at(1).asInt();
        if (args.at(1).isText() || loops < 0) {
            return Sw6Reply::failure(name, err::kBadArgumentValue);
        }
        state_->trajectory(selected.first()).loops = static_cast<int>(loops);
        break;
    }

    case TrajectoryOp::QueryLoops: {
        Sw6Values values;
        values.reserve(selected.size() * 2);
        for (const int number : selected) {
            values.append(Sw6Value::ofInt(number));
            values.append(Sw6Value::ofInt(state_->trajectory(number).loops));
        }
        return Sw6Reply::data(name, std::move(values));
    }

    case TrajectoryOp::SetPeriod: {
        const qint64 cycles = args.first().asInt();
        if (args.first().isText() || cycles < 1) {
            return Sw6Reply::failure(name, err::kBadArgumentValue);
        }
        state_->setTrajectoryPeriod(static_cast<int>(cycles));
        break;
    }

    case TrajectoryOp::QueryPeriod:
        return Sw6Reply::data(name, {Sw6Value::ofInt(state_->trajectoryPeriod())});
    }

    state_->publishTo(context.device());
    return Sw6Reply::ack(name);
}

// --- System commands -------------------------------------------------------

SystemCommandHandler::SystemCommandHandler(std::shared_ptr<Sw6DeviceState> state)
    : state_(std::move(state))
{
}

Result<MessagePtr> SystemCommandHandler::handle(const SystemCommand& command,
                                                ExecutionContext& context)
{
    const SystemCommandSpec* spec = findSystemCommand(commandOpcodeOf(command.command));
    if (spec == nullptr) {
        return Sw6Reply::failure(command.command, err::kUnknownCommandName);
    }
    if (command.argumentError != err::kSuccess) {
        return Sw6Reply::failure(command.command, command.argumentError);
    }

    const QString& name = command.command;

    switch (spec->op) {
    case SystemOp::Identify:
        return Sw6Reply::data(name, {Sw6Value::ofText(state_->model()),
                                     Sw6Value::ofText(state_->dateCode()),
                                     Sw6Value::ofText(state_->serialNumber())});

    case SystemOp::FirmwareVersion:
        return Sw6Reply::data(name, {Sw6Value::ofText(state_->firmwareVersion())});

    case SystemOp::QueryError:
        return Sw6Reply::data(name, {Sw6Value::ofHex(state_->errorCode())});

    case SystemOp::QueryStatus:
        return Sw6Reply::data(name, {Sw6Value::ofHex(state_->statusWord())});

    case SystemOp::CommandList: {
        // The whole command set does not fit the 256 byte frame of section
        // 4.1, so the list stops where it stops fitting and the count reports
        // how many names actually went out.
        Sw6Values names;
        names.reserve(commandNames().size());
        for (const QString& entry : commandNames()) {
            names.append(Sw6Value::ofText(entry));
        }
        // Four digits and a separator, the most the count in front can cost.
        fitToFrame(name, names, 1, 5);

        Sw6Values values{Sw6Value::ofInt(names.size())};
        values.append(names);
        return Sw6Reply::data(name, std::move(values));
    }

    case SystemOp::QueryTimer:
        return Sw6Reply::data(name, {Sw6Value::ofFloat(state_->timer())});

    case SystemOp::QueryStreamMask:
        return Sw6Reply::data(name, {Sw6Value::ofHex(state_->streamMask(), 2)});

    case SystemOp::QueryMaxAngularRate:
        return Sw6Reply::data(name, {Sw6Value::ofFloat(state_->maxAngularRate())});

    case SystemOp::QueryAnalogChannels:
        return Sw6Reply::data(name, {Sw6Value::ofInt(kAnalogChannelCount)});

    case SystemOp::QueryAverageCount:
        return Sw6Reply::data(name, {Sw6Value::ofInt(state_->averageCount())});

    case SystemOp::QueryTriggerLines:
        return Sw6Reply::data(name, {Sw6Value::ofInt(kTriggerLineCount)});

    case SystemOp::QueryPlatformSpeed:
        return Sw6Reply::data(name, {Sw6Value::ofFloat(state_->platformSpeed())});

    case SystemOp::SetPlatformSpeed: {
        const double speed = command.values.first().asDouble();
        if (speed <= 0.0) {
            return Sw6Reply::failure(name, err::kBadArgumentValue);
        }
        state_->setPlatformSpeed(speed);
        break;
    }

    case SystemOp::SetAverageCount: {
        const qint64 samples = command.values.first().asInt();
        if (samples < 1) {
            return Sw6Reply::failure(name, err::kBadArgumentValue);
        }
        state_->setAverageCount(static_cast<int>(samples));
        break;
    }

    case SystemOp::ClearError:
        state_->clearFaults();
        break;

    case SystemOp::Save:
        state_->save();
        break;

    case SystemOp::Restore:
        state_->restore();
        break;

    case SystemOp::LoadDefaults:
        state_->resetToDefaults();
        break;

    case SystemOp::Halt:
        state_->halt();
        break;

    case SystemOp::Stop:
        state_->stop(false);
        break;

    case SystemOp::StopWithFault:
        state_->stop(true);
        break;

    case SystemOp::Reboot:
        state_->resetToDefaults();
        break;

    case SystemOp::PrepareShutdown:
        state_->halt();
        state_->setStreamEnabled(false);
        break;

    case SystemOp::SetTimer:
        state_->setTimer(command.values.first().asDouble());
        break;

    case SystemOp::Delay: {
        // Section 5.1 means this for macros and sequences. Sleeping would
        // block the session thread, so the device only advances the timer
        // `$TIMq` reports and answers straight away.
        const qint64 milliseconds = command.values.first().asInt();
        if (milliseconds < 0) {
            return Sw6Reply::failure(name, err::kBadArgumentValue);
        }
        state_->setTimer(state_->timer() + static_cast<double>(milliseconds) / 1000.0);
        break;
    }

    case SystemOp::SetStreamMask: {
        const qint64 mask = command.values.first().asInt();
        if (mask < 0 || mask > 0xFF) {
            return Sw6Reply::failure(name, err::kBadArgumentValue);
        }
        state_->setStreamMask(static_cast<quint8>(mask));
        break;
    }
    }

    state_->publishTo(context.device());
    return Sw6Reply::ack(name);
}

// --- Unknown commands ------------------------------------------------------

Result<MessagePtr> UnknownCommandHandler::handle(const UnknownCommand& command, ExecutionContext&)
{
    return Sw6Reply::failure(command.command, err::kUnknownCommandName);
}

// --- Realtime stream -------------------------------------------------------

RealtimeStreamHandler::RealtimeStreamHandler(std::shared_ptr<Sw6DeviceState> mirror)
    : mirror_(std::move(mirror))
{
}

Result<MessagePtr> RealtimeStreamHandler::handle(const RealtimeFrame& frame,
                                                 ExecutionContext& context)
{
    lastSample_ = frame.toSample();
    ++frameCount_;

    if (mirror_) {
        mirror_->axis(AxisField::ActualPose) = lastSample_.actualPose;
        if ((lastSample_.mask & stream::kMaskTheoreticalPose) != 0) {
            mirror_->axis(AxisField::TargetPose) = lastSample_.theoreticalPose;
        }
        if ((lastSample_.mask & stream::kMaskActualLength) != 0) {
            mirror_->leg(LegField::ActualLength) = lastSample_.actualLength;
        }
        if ((lastSample_.mask & stream::kMaskTheoreticalLength) != 0) {
            mirror_->leg(LegField::TheoreticalLength) = lastSample_.theoreticalLength;
        }
        if ((lastSample_.mask & stream::kMaskLegSpeed) != 0) {
            mirror_->leg(LegField::Speed) = lastSample_.legSpeed;
        }
        mirror_->publishTo(context.device());
    }

    // An unsolicited report is never answered.
    return MessagePtr{};
}

Sw6StreamSource::Sw6StreamSource(std::shared_ptr<Sw6DeviceState> state, int intervalMs)
    : state_(std::move(state)), intervalMs_(intervalMs)
{
}

std::vector<MessagePtr> Sw6StreamSource::poll(qint64)
{
    if (!state_ || !state_->streamEnabled()) {
        return {};
    }
    return {std::make_shared<RealtimeFrame>(RealtimeFrame::fromSample(state_->sample()))};
}

// --- Registration ----------------------------------------------------------

Result<void> registerSw6Commands(CommandRegistry& registry,
                                 const std::shared_ptr<Sw6DeviceState>& state, bool initiator,
                                 int streamIntervalMs)
{
    // A duplicate opcode can only mean two table rows share a name, or that two
    // names collided in the command hash. Both are build-time mistakes, and
    // failing here names the command instead of silently dropping it.
    const auto bindOne = [](bool bound, std::string_view name) -> Result<void> {
        if (!bound) {
            return makeError(ErrorCode::PluginError,
                             QStringLiteral("SW6 command '%1' has a duplicate opcode")
                                 .arg(QString::fromUtf8(name.data(),
                                                        static_cast<qsizetype>(name.size()))));
        }
        return core::success();
    };

    if (initiator) {
        registry.bindEncoder<AxisCommand>();
        registry.bindEncoder<LegCommand>();
        registry.bindEncoder<ChannelCommand>();
        registry.bindEncoder<SystemCommand>();
        registry.bindEncoder<CoordinateCommand>();
        registry.bindEncoder<NamedCommand>();
        registry.bindEncoder<TrajectoryCommand>();
        registry.bindEncoder<AlignmentCommand>();

        // Every reply has the same shape, so one decoder type serves the whole
        // command set; the opcode it is bound at is what names it afterwards.
        for (const QString& name : commandNames()) {
            registry.bindDecoder<Sw6Reply>(commandOpcodeOf(name), name);
        }

        registry.bindAt<RealtimeFrame>(kRealtimeStreamOpcode,
                                       std::make_shared<RealtimeStreamHandler>(state),
                                       QStringLiteral("实时数据流"));

        HWSIM_LOG_INFO(kLogCategory)
            << "registered " << registry.size() << " SW6 commands as initiator";
        return core::success();
    }

    for (const AxisCommandSpec& spec : kAxisCommands) {
        const auto bound =
            registry.bindDecoder<AxisCommand>(commandOpcode(spec.name), displayNameOf(spec.summary));
        if (const auto checked = bindOne(bound, spec.name); checked.hasError()) {
            return checked;
        }
    }
    registry.bindHandlerOnly<AxisCommand>(std::make_shared<AxisCommandHandler>(state));

    for (const LegCommandSpec& spec : kLegCommands) {
        const auto bound =
            registry.bindDecoder<LegCommand>(commandOpcode(spec.name), displayNameOf(spec.summary));
        if (const auto checked = bindOne(bound, spec.name); checked.hasError()) {
            return checked;
        }
    }
    registry.bindHandlerOnly<LegCommand>(std::make_shared<LegCommandHandler>(state));

    for (const ChannelCommandSpec& spec : kChannelCommands) {
        const auto bound = registry.bindDecoder<ChannelCommand>(commandOpcode(spec.name),
                                                                displayNameOf(spec.summary));
        if (const auto checked = bindOne(bound, spec.name); checked.hasError()) {
            return checked;
        }
    }
    registry.bindHandlerOnly<ChannelCommand>(std::make_shared<ChannelCommandHandler>(state));

    for (const SystemCommandSpec& spec : kSystemCommands) {
        const auto bound = registry.bindDecoder<SystemCommand>(commandOpcode(spec.name),
                                                               displayNameOf(spec.summary));
        if (const auto checked = bindOne(bound, spec.name); checked.hasError()) {
            return checked;
        }
    }
    registry.bindHandlerOnly<SystemCommand>(std::make_shared<SystemCommandHandler>(state));

    for (const CoordinateCommandSpec& spec : kCoordinateCommands) {
        const auto bound = registry.bindDecoder<CoordinateCommand>(commandOpcode(spec.name),
                                                                   displayNameOf(spec.summary));
        if (const auto checked = bindOne(bound, spec.name); checked.hasError()) {
            return checked;
        }
    }
    registry.bindHandlerOnly<CoordinateCommand>(std::make_shared<CoordinateCommandHandler>(state));

    for (const NamedCommandSpec& spec : kNamedCommands) {
        const auto bound = registry.bindDecoder<NamedCommand>(commandOpcode(spec.name),
                                                              displayNameOf(spec.summary));
        if (const auto checked = bindOne(bound, spec.name); checked.hasError()) {
            return checked;
        }
    }
    registry.bindHandlerOnly<NamedCommand>(std::make_shared<NamedCommandHandler>(state));

    for (const TrajectoryCommandSpec& spec : kTrajectoryCommands) {
        const auto bound = registry.bindDecoder<TrajectoryCommand>(commandOpcode(spec.name),
                                                                   displayNameOf(spec.summary));
        if (const auto checked = bindOne(bound, spec.name); checked.hasError()) {
            return checked;
        }
    }
    registry.bindHandlerOnly<TrajectoryCommand>(std::make_shared<TrajectoryCommandHandler>(state));

    for (const AlignmentCommandSpec& spec : kAlignmentCommands) {
        const auto bound = registry.bindDecoder<AlignmentCommand>(commandOpcode(spec.name),
                                                                  displayNameOf(spec.summary));
        if (const auto checked = bindOne(bound, spec.name); checked.hasError()) {
            return checked;
        }
    }
    registry.bindHandlerOnly<AlignmentCommand>(std::make_shared<AlignmentCommandHandler>(state));

    registry.bindAt<UnknownCommand>(kUnknownCommandOpcode,
                                    std::make_shared<UnknownCommandHandler>(),
                                    QStringLiteral("未知命令"));

    registry.bindEncoder<Sw6Reply>();
    registry.bindEncoder<RealtimeFrame>();

    if (streamIntervalMs > 0) {
        registry.setUnsolicitedSource(std::make_shared<Sw6StreamSource>(state, streamIntervalMs));
    }

    HWSIM_LOG_INFO(kLogCategory)
        << "registered " << registry.size() << " SW6 commands as responder, realtime stream every "
        << streamIntervalMs << " ms";
    return core::success();
}

} // namespace hwsim::plugins::sw6
