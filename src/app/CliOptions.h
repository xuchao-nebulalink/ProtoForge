#pragma once

#include <core/Logger.h>
#include <core/Result.h>

#include <QString>
#include <QStringList>

namespace hwsim::app {

/// Parsed command line.
///
/// The headless path exists so the same binary that an engineer drives by hand
/// can be dropped into CI: load a workspace, run a scenario script, exit with
/// the script's verdict as the process exit code.
struct CliOptions {
    bool headless{false};

    QString workspacePath;
    QStringList scriptPaths;
    QStringList pluginDirectories;

    core::LogLevel logLevel{core::LogLevel::Info};
    QString logFilePath;

    /// Start every device in the workspace as soon as it loads.
    bool autoStart{true};

    /// Headless runs stop after this long even if a script hangs. 0 disables it.
    int timeoutSeconds{0};

    [[nodiscard]] static core::Result<CliOptions> parse(const QStringList& arguments);
    [[nodiscard]] static QString helpText();
};

} // namespace hwsim::app
