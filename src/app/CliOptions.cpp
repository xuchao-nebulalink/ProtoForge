#include "CliOptions.h"

#include <QCommandLineParser>

using hwsim::core::ErrorCode;
using hwsim::core::makeError;
using hwsim::core::Result;

namespace hwsim::app {

QString CliOptions::helpText()
{
    return QStringLiteral(
        "设备协议模拟平台\n"
        "\n"
        "用法:\n"
        "  hwsim [选项]\n"
        "\n"
        "选项:\n"
        "  --workspace <file>     加载工程文件 (.json)\n"
        "  --script <file>        运行场景脚本，可重复指定\n"
        "  --headless             不启动界面，跑完脚本后按结果退出\n"
        "  --plugin-dir <dir>     追加插件搜索目录，可重复指定\n"
        "  --log-level <level>    trace|debug|info|warn|error\n"
        "  --log-file <file>      同时写入日志文件\n"
        "  --no-auto-start        载入后不自动启动设备\n"
        "  --timeout <seconds>    无头模式下的整体超时\n");
}

Result<CliOptions> CliOptions::parse(const QStringList& arguments)
{
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("设备协议模拟平台"));
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption workspaceOption(QStringLiteral("workspace"),
                                             QStringLiteral("工程文件"), QStringLiteral("file"));
    const QCommandLineOption scriptOption(QStringLiteral("script"), QStringLiteral("场景脚本"),
                                          QStringLiteral("file"));
    const QCommandLineOption headlessOption(QStringLiteral("headless"),
                                            QStringLiteral("不启动界面"));
    const QCommandLineOption pluginDirOption(QStringLiteral("plugin-dir"),
                                             QStringLiteral("插件目录"), QStringLiteral("dir"));
    const QCommandLineOption logLevelOption(QStringLiteral("log-level"),
                                            QStringLiteral("日志级别"), QStringLiteral("level"),
                                            QStringLiteral("info"));
    const QCommandLineOption logFileOption(QStringLiteral("log-file"), QStringLiteral("日志文件"),
                                           QStringLiteral("file"));
    const QCommandLineOption noAutoStartOption(QStringLiteral("no-auto-start"),
                                               QStringLiteral("载入后不自动启动"));
    const QCommandLineOption timeoutOption(QStringLiteral("timeout"), QStringLiteral("整体超时"),
                                           QStringLiteral("seconds"), QStringLiteral("0"));

    parser.addOption(workspaceOption);
    parser.addOption(scriptOption);
    parser.addOption(headlessOption);
    parser.addOption(pluginDirOption);
    parser.addOption(logLevelOption);
    parser.addOption(logFileOption);
    parser.addOption(noAutoStartOption);
    parser.addOption(timeoutOption);

    parser.process(arguments);

    CliOptions options;
    options.workspacePath = parser.value(workspaceOption);
    options.scriptPaths = parser.values(scriptOption);
    options.headless = parser.isSet(headlessOption);
    options.pluginDirectories = parser.values(pluginDirOption);
    options.logLevel = core::logLevelFromName(parser.value(logLevelOption));
    options.logFilePath = parser.value(logFileOption);
    options.autoStart = !parser.isSet(noAutoStartOption);

    bool timeoutOk = false;
    options.timeoutSeconds = parser.value(timeoutOption).toInt(&timeoutOk);
    if (!timeoutOk || options.timeoutSeconds < 0) {
        return makeError(ErrorCode::InvalidArgument,
                         QStringLiteral("--timeout must be a non-negative number of seconds"));
    }

    if (options.headless && options.scriptPaths.isEmpty() && options.workspacePath.isEmpty()) {
        return makeError(ErrorCode::InvalidArgument,
                         QStringLiteral("--headless needs --workspace and/or --script"));
    }

    return options;
}

} // namespace hwsim::app
