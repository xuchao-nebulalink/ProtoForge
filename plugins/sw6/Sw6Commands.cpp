#include "Sw6Commands.h"

#include <QHash>

namespace hwsim::plugins::sw6 {
namespace {

/// Opcode index over one of the command tables. Function-local statics keep
/// the build order well defined and the tables built exactly once.
template <typename Spec, std::size_t N>
QHash<OpCode, const Spec*> indexOf(const std::array<Spec, N>& table)
{
    QHash<OpCode, const Spec*> index;
    index.reserve(static_cast<qsizetype>(N));
    for (const Spec& spec : table) {
        index.insert(commandOpcode(spec.name), &spec);
    }
    return index;
}

const QHash<OpCode, QString>& nameIndex()
{
    static const QHash<OpCode, QString> index = [] {
        QHash<OpCode, QString> names;
        const auto insert = [&names](std::string_view name) {
            names.insert(commandOpcode(name),
                         QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())));
        };
        for (const AxisCommandSpec& spec : kAxisCommands) {
            insert(spec.name);
        }
        for (const LegCommandSpec& spec : kLegCommands) {
            insert(spec.name);
        }
        for (const ChannelCommandSpec& spec : kChannelCommands) {
            insert(spec.name);
        }
        for (const SystemCommandSpec& spec : kSystemCommands) {
            insert(spec.name);
        }
        for (const CoordinateCommandSpec& spec : kCoordinateCommands) {
            insert(spec.name);
        }
        for (const NamedCommandSpec& spec : kNamedCommands) {
            insert(spec.name);
        }
        for (const TrajectoryCommandSpec& spec : kTrajectoryCommands) {
            insert(spec.name);
        }
        for (const AlignmentCommandSpec& spec : kAlignmentCommands) {
            insert(spec.name);
        }
        return names;
    }();
    return index;
}

} // namespace

const AxisCommandSpec* findAxisCommand(OpCode opcode)
{
    static const auto index = indexOf(kAxisCommands);
    return index.value(opcode, nullptr);
}

const LegCommandSpec* findLegCommand(OpCode opcode)
{
    static const auto index = indexOf(kLegCommands);
    return index.value(opcode, nullptr);
}

const ChannelCommandSpec* findChannelCommand(OpCode opcode)
{
    static const auto index = indexOf(kChannelCommands);
    return index.value(opcode, nullptr);
}

const SystemCommandSpec* findSystemCommand(OpCode opcode)
{
    static const auto index = indexOf(kSystemCommands);
    return index.value(opcode, nullptr);
}

const CoordinateCommandSpec* findCoordinateCommand(OpCode opcode)
{
    static const auto index = indexOf(kCoordinateCommands);
    return index.value(opcode, nullptr);
}

const NamedCommandSpec* findNamedCommand(OpCode opcode)
{
    static const auto index = indexOf(kNamedCommands);
    return index.value(opcode, nullptr);
}

const TrajectoryCommandSpec* findTrajectoryCommand(OpCode opcode)
{
    static const auto index = indexOf(kTrajectoryCommands);
    return index.value(opcode, nullptr);
}

const AlignmentCommandSpec* findAlignmentCommand(OpCode opcode)
{
    static const auto index = indexOf(kAlignmentCommands);
    return index.value(opcode, nullptr);
}

QString commandNameFor(OpCode opcode)
{
    return nameIndex().value(opcode);
}

bool isKnownCommand(OpCode opcode)
{
    return nameIndex().contains(opcode);
}

QStringList commandNames()
{
    QStringList names;
    names.reserve(static_cast<qsizetype>(kAxisCommands.size() + kLegCommands.size()
                                         + kChannelCommands.size() + kSystemCommands.size()
                                         + kCoordinateCommands.size() + kNamedCommands.size()
                                         + kTrajectoryCommands.size()
                                         + kAlignmentCommands.size()));
    const auto append = [&names](std::string_view name) {
        names.append(QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())));
    };
    for (const AxisCommandSpec& spec : kAxisCommands) {
        append(spec.name);
    }
    for (const LegCommandSpec& spec : kLegCommands) {
        append(spec.name);
    }
    for (const ChannelCommandSpec& spec : kChannelCommands) {
        append(spec.name);
    }
    for (const SystemCommandSpec& spec : kSystemCommands) {
        append(spec.name);
    }
    for (const CoordinateCommandSpec& spec : kCoordinateCommands) {
        append(spec.name);
    }
    for (const NamedCommandSpec& spec : kNamedCommands) {
        append(spec.name);
    }
    for (const TrajectoryCommandSpec& spec : kTrajectoryCommands) {
        append(spec.name);
    }
    for (const AlignmentCommandSpec& spec : kAlignmentCommands) {
        append(spec.name);
    }
    return names;
}

} // namespace hwsim::plugins::sw6
