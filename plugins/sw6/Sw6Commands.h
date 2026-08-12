#pragma once

// -----------------------------------------------------------------------------
// The command set as data.
//
// SW6 has on the order of two hundred commands, but only a handful of shapes:
// "axis name plus value" pairs, "leg number plus value" groups, and commands
// with no element at all. Each shape gets one message type and one handler; the
// tables below say which quantity a given command name touches and how its
// arguments are laid out, so adding a command is one row here plus one
// registration line - no new class, no switch.
// -----------------------------------------------------------------------------

#include "Sw6Types.h"

#include <QString>
#include <QStringList>

#include <array>
#include <optional>

namespace hwsim::plugins::sw6 {

/// Platform degrees of freedom, addressed by axis name X/Y/Z/U/V/W.
enum class AxisField : quint8 {
    TargetPose,
    ActualPose,
    Velocity,
    Acceleration,
    Deceleration,
    JogSpeed,
    SoftLimitMin,
    SoftLimitMax,
    SoftLimitEnabled,
    MaxVelocity,
    MaxAcceleration,
    RangeMin,
    RangeMax,
    OnTarget,
    ServoOn,
    PivotPoint,
    /// Section 5.15. The three surface-detection fields are read back together
    /// by `$FSFq`, so they have to stay adjacent and in `$FSF` argument order.
    ScanResult,
    SurfaceForce1,
    SurfaceOffset,
    SurfaceForce2,
    SurfaceResult,
    ImpulseAmplitude,
    StepAmplitude,
    Count,
};

/// Physical actuator legs, addressed by leg number 1..6. Fields that a single
/// command reads or writes as a pair must stay adjacent: `LLM` writes
/// TravelMin and TravelMax, `LIMq` reads both limit switches.
enum class LegField : quint8 {
    Referenced,
    Enabled,
    BrakeReleased,
    Compensation,
    TravelMin,
    TravelMax,
    LimitSwitchNegative,
    LimitSwitchPositive,
    ActualLength,
    TheoreticalLength,
    Speed,
    Count,
};

enum class AxisOp : quint8 {
    Set,
    Query,
    MoveAbsolute,
    MoveRelative,
    /// `$MRT` / `$MRW`: the offset is expressed in the enabled TOOL / WORK
    /// frame, so it has to be rotated into BASE before it becomes a target.
    MoveRelativeTool,
    MoveRelativeWork,
    /// `$FNL` / `$FPL` / `$MNL` / `$MPL`: the target is not in the request but
    /// in the field the spec points at.
    MoveToLimit,
    GoHome,
    Jog,
    /// Section 5.15 dynamics identification: `$IMP` / `$STE` store an
    /// excitation amplitude, which only makes sense on a live platform.
    Excite,
    /// `$DPO`, whose tuning happens inside the firmware.
    Optimise,
};

enum class LegOp : quint8 { Set, Query, Reference };

/// Section 5.7. The functional frames are named, so these commands carry a
/// frame type and a name rather than an element index.
enum class CoordinateOp : quint8 {
    DefineTool,
    QueryTool,
    DefineWork,
    QueryWork,
    DefineFromPose,
    Enable,
    QueryEnabled,
    Remove,
    Copy,
    List,
    Link,
    QueryLink,
};

/// Sections 5.9 and 5.10: quantities addressed by an integer element, either
/// an analog input channel or an I/O line. Which of the two - and therefore
/// the numbering the command accepts - follows from the field, see
/// channelRangeOf().
enum class ChannelField : quint8 {
    AnalogVoltage,
    AnalogRaw,
    AnalogNormalised,
    SensorPosition,
    DigitalLevel,
    TriggerOutputEnabled,
    TriggerInputEnabled,
    /// `$CTO` / `$CTI` hold several firmware-defined parameters per line, so
    /// these two do not live in the flat per-channel arrays.
    TriggerOutputConfig,
    TriggerInputConfig,
    Count,
};

enum class ChannelOp : quint8 { Set, Query, SetConfig, QueryConfig };

/// Section 5.12. Trajectories are addressed by number, but the rest of the
/// arguments are positional, so the handler reads them by position.
enum class TrajectoryOp : quint8 {
    Clear,
    AddPoint,
    Finish,
    Start,
    Control,
    QueryInfo,
    SetLoops,
    QueryLoops,
    SetPeriod,
    QueryPeriod,
};

/// Section 5.15. Alignment scans and dynamics identification: the searches are
/// named processes, the scans name their axes, and the trailing arguments are
/// firmware-defined, so all of it is read by position.
enum class AlignmentOp : quint8 {
    DefineGradient,
    DefineRaster,
    SetCentre,
    QueryCentre,
    Start,
    Control,
    QueryState,
    QueryResult,
    QueryResultHelp,
    SetCoupling,
    QueryCoupling,
    ScanAxis,
    ScanPlane,
    SurfaceDetect,
    FrequencyResponse,
    QueryFrequencyResponse,
    SetInputCalculation,
    QueryInputCalculation,
    QueryInputValue,
};

/// Sections 5.8 and 5.8.1: registries addressed by parameter name.
enum class NamedOp : quint8 { SetParameter, QueryParameter, QueryKinematic, QueryGeometryRow };

enum class SystemOp : quint8 {
    Identify,
    FirmwareVersion,
    QueryError,
    ClearError,
    CommandList,
    QueryStatus,
    Save,
    Restore,
    LoadDefaults,
    Halt,
    Stop,
    StopWithFault,
    Reboot,
    PrepareShutdown,
    SetTimer,
    QueryTimer,
    Delay,
    SetStreamMask,
    QueryStreamMask,
    QueryMaxAngularRate,
    QueryAnalogChannels,
    SetAverageCount,
    QueryAverageCount,
    QueryTriggerLines,
    SetPlatformSpeed,
    QueryPlatformSpeed,
};

enum class ValueKind : quint8 { Float, Integer, Text };

inline constexpr auto kAxisFieldCount = static_cast<std::size_t>(AxisField::Count);
inline constexpr auto kLegFieldCount = static_cast<std::size_t>(LegField::Count);
inline constexpr auto kChannelFieldCount = static_cast<std::size_t>(ChannelField::Count);

/// Element numbers a channel command accepts: analog channels start at 0 to
/// match the AD block of section 6.2, I/O lines at 1.
struct ChannelRange {
    int first{0};
    int count{0};

    [[nodiscard]] constexpr bool contains(qint64 element) const noexcept
    {
        return element >= first && element < first + count;
    }
};

[[nodiscard]] constexpr ChannelRange channelRangeOf(ChannelField field) noexcept
{
    switch (field) {
    case ChannelField::AnalogVoltage:
    case ChannelField::AnalogRaw:
    case ChannelField::AnalogNormalised:
    case ChannelField::SensorPosition:
        return {0, kAnalogChannelCount};
    case ChannelField::DigitalLevel:
        return {1, kDigitalLineCount};
    default:
        return {1, kTriggerLineCount};
    }
}

/// True for the two fields whose values are a map of firmware-defined
/// parameters rather than a single number per element.
[[nodiscard]] constexpr bool isTriggerConfig(ChannelField field) noexcept
{
    return field == ChannelField::TriggerOutputConfig
           || field == ChannelField::TriggerInputConfig;
}

struct AxisCommandSpec {
    std::string_view name;
    AxisOp op;
    AxisField field;
    /// Values that follow each axis name in a set request, or that a query
    /// returns per axis. Queries always take bare axis names.
    quint8 valuesPerElement;
    ValueKind valueKind;
    std::string_view summary;
};

struct LegCommandSpec {
    std::string_view name;
    LegOp op;
    LegField field;
    quint8 valuesPerElement;
    ValueKind valueKind;
    std::string_view summary;
};

struct ChannelCommandSpec {
    std::string_view name;
    ChannelOp op;
    ChannelField field;
    quint8 valuesPerElement;
    ValueKind valueKind;
    std::string_view summary;
};

struct SystemCommandSpec {
    std::string_view name;
    SystemOp op;
    quint8 minValues;
    quint8 maxValues;
    ValueKind valueKind;
    std::string_view summary;
};

/// Arguments of these are positional rather than element-wise, so the spec only
/// says how many are allowed and the handler reads them by position.
struct CoordinateCommandSpec {
    std::string_view name;
    CoordinateOp op;
    quint8 minValues;
    quint8 maxValues;
    std::string_view summary;
};

struct NamedCommandSpec {
    std::string_view name;
    NamedOp op;
    quint8 minValues;
    quint8 maxValues;
    std::string_view summary;
};

struct TrajectoryCommandSpec {
    std::string_view name;
    TrajectoryOp op;
    quint8 minValues;
    quint8 maxValues;
    std::string_view summary;
};

struct AlignmentCommandSpec {
    std::string_view name;
    AlignmentOp op;
    quint8 minValues;
    quint8 maxValues;
    std::string_view summary;
};

// Sections 5.2, 5.3, 5.4 and 5.6: everything addressed by platform axis.
inline constexpr auto kAxisCommands = std::to_array<AxisCommandSpec>({
    // name     operation              field                      n  value kind         summary
    {"MOV",  AxisOp::MoveAbsolute, AxisField::TargetPose,       1, ValueKind::Float,   "绝对位姿移动"},
    {"MVR",  AxisOp::MoveRelative, AxisField::TargetPose,       1, ValueKind::Float,   "相对位姿移动"},
    {"MRT",  AxisOp::MoveRelativeTool, AxisField::TargetPose,   1, ValueKind::Float,   "TOOL 系下相对移动"},
    {"MRW",  AxisOp::MoveRelativeWork, AxisField::TargetPose,   1, ValueKind::Float,   "WORK 系下相对移动"},
    {"MOVq", AxisOp::Query,        AxisField::TargetPose,       1, ValueKind::Float,   "查询目标位姿"},
    {"POSq", AxisOp::Query,        AxisField::ActualPose,       1, ValueKind::Float,   "查询实际位姿"},
    {"GOH",  AxisOp::GoHome,       AxisField::TargetPose,       0, ValueKind::Float,   "回用户零位姿"},
    {"FNL",  AxisOp::MoveToLimit,  AxisField::SoftLimitMin,     0, ValueKind::Float,   "快速移至软限位下限"},
    {"FPL",  AxisOp::MoveToLimit,  AxisField::SoftLimitMax,     0, ValueKind::Float,   "快速移至软限位上限"},
    {"MNL",  AxisOp::MoveToLimit,  AxisField::RangeMin,         0, ValueKind::Float,   "移至负限位开关"},
    {"MPL",  AxisOp::MoveToLimit,  AxisField::RangeMax,         0, ValueKind::Float,   "移至正限位开关"},
    {"JOG",  AxisOp::Jog,          AxisField::JogSpeed,         1, ValueKind::Float,   "恒速点动"},
    {"JOGq", AxisOp::Query,        AxisField::JogSpeed,         1, ValueKind::Float,   "查询点动速度"},
    {"VEL",  AxisOp::Set,          AxisField::Velocity,         1, ValueKind::Float,   "设置速度"},
    {"VELq", AxisOp::Query,        AxisField::Velocity,         1, ValueKind::Float,   "查询速度"},
    {"ACC",  AxisOp::Set,          AxisField::Acceleration,     1, ValueKind::Float,   "设置加速度"},
    {"ACCq", AxisOp::Query,        AxisField::Acceleration,     1, ValueKind::Float,   "查询加速度"},
    {"DEC",  AxisOp::Set,          AxisField::Deceleration,     1, ValueKind::Float,   "设置减速度"},
    {"DECq", AxisOp::Query,        AxisField::Deceleration,     1, ValueKind::Float,   "查询减速度"},
    {"VMXq", AxisOp::Query,        AxisField::MaxVelocity,      1, ValueKind::Float,   "查询速度上限"},
    {"AMXq", AxisOp::Query,        AxisField::MaxAcceleration,  1, ValueKind::Float,   "查询加速度上限"},
    {"NLM",  AxisOp::Set,          AxisField::SoftLimitMin,     1, ValueKind::Float,   "设置软限位下限"},
    {"NLMq", AxisOp::Query,        AxisField::SoftLimitMin,     1, ValueKind::Float,   "查询软限位下限"},
    {"PLM",  AxisOp::Set,          AxisField::SoftLimitMax,     1, ValueKind::Float,   "设置软限位上限"},
    {"PLMq", AxisOp::Query,        AxisField::SoftLimitMax,     1, ValueKind::Float,   "查询软限位上限"},
    {"SSL",  AxisOp::Set,          AxisField::SoftLimitEnabled, 1, ValueKind::Integer, "启用软限位"},
    {"SSLq", AxisOp::Query,        AxisField::SoftLimitEnabled, 1, ValueKind::Integer, "查询软限位状态"},
    {"TMNq", AxisOp::Query,        AxisField::RangeMin,         1, ValueKind::Float,   "查询理论最小行程"},
    {"TMXq", AxisOp::Query,        AxisField::RangeMax,         1, ValueKind::Float,   "查询理论最大行程"},
    {"ONTq", AxisOp::Query,        AxisField::OnTarget,         1, ValueKind::Integer, "查询到位状态"},
    {"SVO",  AxisOp::Set,          AxisField::ServoOn,          1, ValueKind::Integer, "伺服使能"},
    {"SVOq", AxisOp::Query,        AxisField::ServoOn,          1, ValueKind::Integer, "查询伺服状态"},
    {"SPI",  AxisOp::Set,          AxisField::PivotPoint,       1, ValueKind::Float,   "设置旋转中心"},
    {"SPIq", AxisOp::Query,        AxisField::PivotPoint,       1, ValueKind::Float,   "查询旋转中心"},
    // Section 5.15, the per-axis half of the alignment and identification set.
    {"FSNq", AxisOp::Query,        AxisField::ScanResult,       1, ValueKind::Float,   "查询扫描结果"},
    {"FSFq", AxisOp::Query,        AxisField::SurfaceForce1,    3, ValueKind::Float,   "查询表面检测参数"},
    {"FSRq", AxisOp::Query,        AxisField::SurfaceResult,    1, ValueKind::Float,   "查询表面检测结果"},
    {"IMP",  AxisOp::Excite,       AxisField::ImpulseAmplitude, 1, ValueKind::Float,   "脉冲响应测量"},
    {"IMPq", AxisOp::Query,        AxisField::ImpulseAmplitude, 1, ValueKind::Float,   "查询脉冲响应"},
    {"STE",  AxisOp::Excite,       AxisField::StepAmplitude,    1, ValueKind::Float,   "阶跃响应测量"},
    {"STEq", AxisOp::Query,        AxisField::StepAmplitude,    1, ValueKind::Float,   "查询阶跃响应"},
    {"DPO",  AxisOp::Optimise,     AxisField::TargetPose,       0, ValueKind::Float,   "动态参数优化"},
});

// Sections 5.1, 5.3, 5.5 and 5.6: everything addressed by physical leg 1..6.
inline constexpr auto kLegCommands = std::to_array<LegCommandSpec>({
    // name     operation        field                          n  value kind         summary
    {"FRF",  LegOp::Reference, LegField::Referenced,          0, ValueKind::Integer, "执行回零参考"},
    {"FRFq", LegOp::Query,     LegField::Referenced,          1, ValueKind::Integer, "查询参考状态"},
    {"EAX",  LegOp::Set,       LegField::Enabled,             1, ValueKind::Integer, "启用物理腿"},
    {"EAXq", LegOp::Query,     LegField::Enabled,             1, ValueKind::Integer, "查询物理腿启用状态"},
    {"BRA",  LegOp::Set,       LegField::BrakeReleased,       1, ValueKind::Integer, "刹车开关"},
    {"BRAq", LegOp::Query,     LegField::BrakeReleased,       1, ValueKind::Integer, "查询刹车状态"},
    {"LLC",  LegOp::Set,       LegField::Compensation,        1, ValueKind::Float,   "烧录杆长补偿"},
    {"LLCq", LegOp::Query,     LegField::Compensation,        1, ValueKind::Float,   "读取杆长补偿"},
    {"LLM",  LegOp::Set,       LegField::TravelMin,           2, ValueKind::Float,   "设置物理腿行程"},
    {"LLMq", LegOp::Query,     LegField::TravelMin,           2, ValueKind::Float,   "查询物理腿行程"},
    {"LIMq", LegOp::Query,     LegField::LimitSwitchNegative, 2, ValueKind::Integer, "查询限位开关状态"},
});

// Sections 5.9 and 5.10: analog inputs and the digital I/O and trigger lines.
inline constexpr auto kChannelCommands = std::to_array<ChannelCommandSpec>({
    // name     operation               field                              n  value kind         summary
    {"TAVq", ChannelOp::Query,       ChannelField::AnalogVoltage,        1, ValueKind::Float,   "查询模拟输入电压"},
    {"TADq", ChannelOp::Query,       ChannelField::AnalogRaw,            1, ValueKind::Integer, "查询 ADC 原始值"},
    {"TNSq", ChannelOp::Query,       ChannelField::AnalogNormalised,     1, ValueKind::Float,   "查询归一化输入信号"},
    {"TSPq", ChannelOp::Query,       ChannelField::SensorPosition,       1, ValueKind::Float,   "查询传感器换算位置"},
    {"DIO",  ChannelOp::Set,         ChannelField::DigitalLevel,         1, ValueKind::Integer, "设置数字输出线"},
    {"DIOq", ChannelOp::Query,       ChannelField::DigitalLevel,         1, ValueKind::Integer, "查询数字 I/O 状态"},
    {"TRO",  ChannelOp::Set,         ChannelField::TriggerOutputEnabled, 1, ValueKind::Integer, "启用/禁用触发输出"},
    {"TROq", ChannelOp::Query,       ChannelField::TriggerOutputEnabled, 1, ValueKind::Integer, "查询触发输出状态"},
    {"TRI",  ChannelOp::Set,         ChannelField::TriggerInputEnabled,  1, ValueKind::Integer, "启用/禁用触发输入"},
    {"TRIq", ChannelOp::Query,       ChannelField::TriggerInputEnabled,  1, ValueKind::Integer, "查询触发输入状态"},
    {"CTO",  ChannelOp::SetConfig,   ChannelField::TriggerOutputConfig,  2, ValueKind::Float,   "配置触发输出"},
    {"CTOq", ChannelOp::QueryConfig, ChannelField::TriggerOutputConfig,  2, ValueKind::Float,   "查询触发输出配置"},
    {"CTI",  ChannelOp::SetConfig,   ChannelField::TriggerInputConfig,   2, ValueKind::Float,   "配置触发输入条件"},
    {"CTIq", ChannelOp::QueryConfig, ChannelField::TriggerInputConfig,   2, ValueKind::Float,   "查询触发输入配置"},
});

// Sections 5.1 and 5.16: commands with no element, plus the realtime stream mask.
inline constexpr auto kSystemCommands = std::to_array<SystemCommandSpec>({
    // name     operation                     min max  value kind         summary
    {"IDN",  SystemOp::Identify,            0, 0, ValueKind::Text,    "设备标识"},
    {"VER",  SystemOp::FirmwareVersion,     0, 0, ValueKind::Text,    "固件版本"},
    {"ERR",  SystemOp::QueryError,          0, 0, ValueKind::Text,    "查询当前错误码"},
    {"CLR",  SystemOp::ClearError,          0, 0, ValueKind::Text,    "清除错误与故障锁存"},
    {"HLPq", SystemOp::CommandList,         0, 0, ValueKind::Text,    "查询支持的命令列表"},
    {"STAq", SystemOp::QueryStatus,         0, 0, ValueKind::Text,    "查询状态字"},
    {"SAV",  SystemOp::Save,                0, 1, ValueKind::Text,    "保存配置"},
    {"RST",  SystemOp::Restore,             0, 1, ValueKind::Text,    "恢复已保存配置"},
    {"ITD",  SystemOp::LoadDefaults,        0, 1, ValueKind::Text,    "恢复默认配置"},
    {"HLT",  SystemOp::Halt,                0, 0, ValueKind::Text,    "平滑停止"},
    {"STP",  SystemOp::Stop,                0, 0, ValueKind::Text,    "急停"},
    {"STF",  SystemOp::StopWithFault,       0, 0, ValueKind::Text,    "强制停止并进入故障"},
    {"RBT",  SystemOp::Reboot,              0, 0, ValueKind::Text,    "重启控制器"},
    {"RTO",  SystemOp::PrepareShutdown,     0, 0, ValueKind::Text,    "准备断电"},
    {"TIM",  SystemOp::SetTimer,            1, 1, ValueKind::Float,   "设置计时器"},
    {"TIMq", SystemOp::QueryTimer,          0, 0, ValueKind::Text,    "查询计时器"},
    {"DEL",  SystemOp::Delay,               1, 1, ValueKind::Integer, "延时"},
    {"RSE",  SystemOp::SetStreamMask,       1, 1, ValueKind::Integer, "设置实时流掩码"},
    {"RSEq", SystemOp::QueryStreamMask,     0, 0, ValueKind::Text,    "查询实时流掩码"},
    {"RMXq", SystemOp::QueryMaxAngularRate, 0, 0, ValueKind::Text,    "查询角速度上限"},
    {"TACq", SystemOp::QueryAnalogChannels, 0, 0, ValueKind::Text,    "查询模拟输入通道数"},
    {"NAV",  SystemOp::SetAverageCount,     1, 1, ValueKind::Integer, "设置位置平均采样数"},
    {"NAVq", SystemOp::QueryAverageCount,   0, 0, ValueKind::Text,    "查询平均采样数"},
    {"TIOq", SystemOp::QueryTriggerLines,   0, 0, ValueKind::Text,    "查询触发 I/O 通道数"},
    // Like `$SVO`, the platform's vector speed is missing from the section 5
    // tables but is described in 4.6 and has a checksum vector in 9.2.
    {"VLS",  SystemOp::SetPlatformSpeed,    1, 1, ValueKind::Float,   "设置平台合成速度"},
    {"VLSq", SystemOp::QueryPlatformSpeed,  0, 0, ValueKind::Text,    "查询平台合成速度"},
});

// Section 5.7: the tool and work frames, addressed by name.
inline constexpr auto kCoordinateCommands = std::to_array<CoordinateCommandSpec>({
    // name      operation                   min max  summary
    {"KST",  CoordinateOp::DefineTool,     3, 13, "定义/修改工具坐标系"},
    {"KSTq", CoordinateOp::QueryTool,      1, 1,  "查询工具坐标系"},
    {"KSW",  CoordinateOp::DefineWork,     3, 13, "定义/修改工件坐标系"},
    {"KSWq", CoordinateOp::QueryWork,      1, 1,  "查询工件坐标系"},
    {"KSF",  CoordinateOp::DefineFromPose, 2, 2,  "由当前位姿定义坐标系"},
    {"KEN",  CoordinateOp::Enable,         3, 3,  "启用/停用坐标系"},
    {"KENq", CoordinateOp::QueryEnabled,   2, 2,  "查询坐标系启用状态"},
    {"KRM",  CoordinateOp::Remove,         2, 2,  "删除坐标系"},
    {"KCP",  CoordinateOp::Copy,           3, 3,  "复制坐标系"},
    {"KLSq", CoordinateOp::List,           1, 1,  "查询坐标系列表"},
    {"KLN",  CoordinateOp::Link,           4, 4,  "链接坐标系父子关系"},
    {"KLNq", CoordinateOp::QueryLink,      2, 2,  "查询坐标系父链接"},
});

// Sections 5.8 and 5.8.1: named parameters and the read-only geometry.
inline constexpr auto kNamedCommands = std::to_array<NamedCommandSpec>({
    // name      operation                 min max  summary
    {"PAR",  NamedOp::SetParameter,     2, 16, "设置命名参数"},
    {"PARq", NamedOp::QueryParameter,   0, 8,  "查询命名参数"},
    {"KINq", NamedOp::QueryKinematic,   0, 8,  "查询运动学标量"},
    {"GEOq", NamedOp::QueryGeometryRow, 1, 1,  "查询几何铰点阵一行"},
});

/// Axes a command may address. `$SPI` is the exception: a rotation centre is a
/// point, so it takes X/Y/Z only (section 5.7).
[[nodiscard]] constexpr int axisLimitFor(AxisField field) noexcept
{
    return field == AxisField::PivotPoint ? 3 : kAxisCount;
}

/// `$MNL` / `$MPL` drive onto the physical limit switches, which sit outside
/// the soft limits by definition; `$FNL` / `$FPL` move to the soft limits
/// themselves and are checked against them like any other move.
[[nodiscard]] constexpr bool limitMoveIsSoft(AxisField field) noexcept
{
    return field != AxisField::RangeMin && field != AxisField::RangeMax;
}

/// Upper bound a set command is validated against, when the protocol defines
/// one: `$VEL` above `VMXq` is rejected with 0x0300000C (section 5.4).
[[nodiscard]] constexpr std::optional<AxisField> capFieldFor(AxisField field) noexcept
{
    switch (field) {
    case AxisField::Velocity:
        return AxisField::MaxVelocity;
    case AxisField::Acceleration:
        return AxisField::MaxAcceleration;
    default:
        return std::nullopt;
    }
}

// Section 5.12: the buffered trajectories.
inline constexpr auto kTrajectoryCommands = std::to_array<TrajectoryCommandSpec>({
    // name      operation                 min max  summary
    {"TGC",  TrajectoryOp::Clear,       0, 1,  "清除轨迹"},
    // `<轨迹>d,<点号>d,<插补>d,<速度>f,<停留ms>d` and then up to six axis pairs.
    {"TGA",  TrajectoryOp::AddPoint,    5, 17, "写入轨迹点"},
    {"TGF",  TrajectoryOp::Finish,      1, 1,  "结束写入并检查轨迹"},
    {"TGS",  TrajectoryOp::Start,       1, 1,  "启动轨迹"},
    {"TST",  TrajectoryOp::Control,     2, 2,  "控制轨迹运行"},
    {"TGIq", TrajectoryOp::QueryInfo,   0, 1,  "查询轨迹信息"},
    {"TLC",  TrajectoryOp::SetLoops,    2, 2,  "设置轨迹循环次数"},
    {"TLCq", TrajectoryOp::QueryLoops,  0, 1,  "查询轨迹循环次数"},
    {"TGT",  TrajectoryOp::SetPeriod,   1, 1,  "设置轨迹插补周期"},
    {"TGTq", TrajectoryOp::QueryPeriod, 0, 0,  "查询轨迹插补周期"},
});

// Section 5.15: named searches, the scans and the identification sweeps.
inline constexpr auto kAlignmentCommands = std::to_array<AlignmentCommandSpec>({
    // name      operation                             min max  summary
    {"FDG",  AlignmentOp::DefineGradient,           3, 12, "梯度搜索"},
    {"FDR",  AlignmentOp::DefineRaster,             5, 12, "光栅扫描"},
    {"FDL",  AlignmentOp::DefineRaster,             5, 12, "第一束光搜索"},
    {"FGC",  AlignmentOp::SetCentre,                3, 3,  "设置梯度扫描偏置"},
    {"FGCq", AlignmentOp::QueryCentre,              0, 1,  "查询偏置中心"},
    {"FRS",  AlignmentOp::Start,                    1, 1,  "启动对准过程"},
    {"FRP",  AlignmentOp::Control,                  2, 2,  "停止/暂停/恢复过程"},
    {"FRPq", AlignmentOp::QueryState,               0, 1,  "查询过程状态"},
    {"FRRq", AlignmentOp::QueryResult,              1, 2,  "获取对准结果"},
    {"FRHq", AlignmentOp::QueryResultHelp,          0, 0,  "获取对准结果帮助"},
    {"FRC",  AlignmentOp::SetCoupling,              2, 2,  "设置过程耦合"},
    {"FRCq", AlignmentOp::QueryCoupling,            0, 1,  "查询过程耦合"},
    {"FAA",  AlignmentOp::ScanAxis,                 4, 4,  "快速角扫描至最大"},
    {"FAM",  AlignmentOp::ScanAxis,                 6, 6,  "角扫描至最大"},
    {"FAS",  AlignmentOp::ScanAxis,                 6, 6,  "快速角扫描"},
    {"FLM",  AlignmentOp::ScanAxis,                 2, 8,  "线扫描至最大"},
    {"FLS",  AlignmentOp::ScanAxis,                 2, 8,  "快速线扫描"},
    {"FSA",  AlignmentOp::ScanPlane,                4, 10, "扫描粗+细"},
    {"FSC",  AlignmentOp::ScanPlane,                4, 10, "扫描至阈值"},
    {"FSM",  AlignmentOp::ScanPlane,                4, 10, "扫描全局最大"},
    {"FIO",  AlignmentOp::ScanPlane,                4, 10, "I/O 对准扫描"},
    {"AAP",  AlignmentOp::ScanPlane,                4, 10, "扫描最大强度"},
    {"FSF",  AlignmentOp::SurfaceDetect,            3, 4,  "表面检测"},
    {"WFR",  AlignmentOp::FrequencyResponse,        5, 5,  "频率响应分析"},
    {"WFRq", AlignmentOp::QueryFrequencyResponse,   2, 2,  "查询频率响应"},
    {"SIC",  AlignmentOp::SetInputCalculation,      2, 10, "设置输入计算方式"},
    {"SICq", AlignmentOp::QueryInputCalculation,    0, 1,  "查询输入计算方式"},
    {"TCIq", AlignmentOp::QueryInputValue,          0, 1,  "查询计算出的输入值"},
});

// --- Lookup ----------------------------------------------------------------

[[nodiscard]] const AxisCommandSpec* findAxisCommand(OpCode opcode);
[[nodiscard]] const LegCommandSpec* findLegCommand(OpCode opcode);
[[nodiscard]] const ChannelCommandSpec* findChannelCommand(OpCode opcode);
[[nodiscard]] const SystemCommandSpec* findSystemCommand(OpCode opcode);
[[nodiscard]] const CoordinateCommandSpec* findCoordinateCommand(OpCode opcode);
[[nodiscard]] const NamedCommandSpec* findNamedCommand(OpCode opcode);
[[nodiscard]] const TrajectoryCommandSpec* findTrajectoryCommand(OpCode opcode);
[[nodiscard]] const AlignmentCommandSpec* findAlignmentCommand(OpCode opcode);

/// Reverse table the codec needs: an outgoing frame knows its opcode and has to
/// write `$<CmdName>`. Empty when the opcode is not a known command.
[[nodiscard]] QString commandNameFor(OpCode opcode);
[[nodiscard]] bool isKnownCommand(OpCode opcode);

/// Every registered command name, in table order. Answers `$HLPq`.
[[nodiscard]] QStringList commandNames();

} // namespace hwsim::plugins::sw6
