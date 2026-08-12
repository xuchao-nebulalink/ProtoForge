#include "CliOptions.h"
#include "MainWindow.h"
#include "ScriptHostAdapter.h"
#include "Workspace.h"

#include <core/EventBus.h>
#include <core/LogSinks.h>
#include <core/Logger.h>
#include <protocol/PluginManager.h>
#include <scripting/ScriptEngine.h>
#include <transport/TransportFactory.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>

#include <iostream>

namespace {
constexpr auto kLogCategory = "app";

using namespace hwsim;

/// The window needs a shared history sink so a log dock opened after start-up
/// still shows everything that happened during initialisation.
std::shared_ptr<core::RingBufferLogSink> installLogging(const app::CliOptions& options)
{
    auto& logger = core::Logger::instance();
    logger.setLevel(options.logLevel);

    auto history = std::make_shared<core::RingBufferLogSink>(20000);
    logger.addSink(history);
    logger.addSink(std::make_shared<core::ConsoleLogSink>(false, !options.headless ? false : true));

    if (!options.logFilePath.isEmpty()) {
        logger.addSink(std::make_shared<core::FileLogSink>(options.logFilePath));
    }
    return history;
}

int loadPlugins(protocol::PluginManager& manager, const app::CliOptions& options)
{
    protocol::registerStaticPlugins(manager);

    QStringList directories = options.pluginDirectories;
    directories.prepend(protocol::PluginManager::defaultPluginDirectory());

    const int loaded = manager.loadAll(directories);

    for (const auto& failure : manager.failures()) {
        HWSIM_LOG_WARNING(kLogCategory)
            << "plugin rejected: " << failure.source << " - " << failure.reason;
    }
    return loaded;
}

/// Headless entry point: load, start, run scripts, report.
///
/// Runs to completion synchronously rather than entering an event loop, so the
/// overall timeout is enforced by checking a deadline between scripts. A single
/// script cannot hang indefinitely because every wait in ScriptApi carries its
/// own timeout; only a bare infinite loop in JavaScript would escape this.
int runHeadless(const app::CliOptions& options, app::Workspace& workspace)
{
    QElapsedTimer runTimer;
    runTimer.start();

    const auto deadlineExceeded = [&options, &runTimer] {
        return options.timeoutSeconds > 0
               && runTimer.elapsed() > static_cast<qint64>(options.timeoutSeconds) * 1000;
    };

    if (!options.workspacePath.isEmpty()) {
        if (const auto loaded = workspace.load(options.workspacePath); loaded.hasError()) {
            std::cerr << "failed to load workspace: "
                      << loaded.error().toString().toStdString() << std::endl;
            return 2;
        }
    }

    if (options.autoStart) {
        if (const auto started = workspace.startAll(); started.hasError()) {
            std::cerr << "failed to start devices: "
                      << started.error().toString().toStdString() << std::endl;
            return 3;
        }
    }

    app::ScriptHostAdapter host(workspace);
    scripting::ScriptEngine engine(host);

    int failures = 0;
    for (const QString& path : options.scriptPaths) {
        if (deadlineExceeded()) {
            std::cerr << "headless run exceeded --timeout of " << options.timeoutSeconds
                      << "s before " << path.toStdString() << std::endl;
            workspace.stopAll();
            return 4;
        }

        const auto outcome = engine.runFile(path);

        std::cout << path.toStdString() << ": " << outcome.summary().toStdString() << std::endl;
        for (const QString& failure : outcome.failures) {
            std::cout << "    " << failure.toStdString() << std::endl;
        }
        if (!outcome.errorText.isEmpty()) {
            std::cout << "    " << outcome.errorText.toStdString() << std::endl;
        }

        if (!outcome.completed || !outcome.passed) {
            ++failures;
        }
    }

    workspace.stopAll();
    core::Logger::instance().flush();

    if (deadlineExceeded()) {
        std::cerr << "headless run exceeded --timeout of " << options.timeoutSeconds << "s"
                  << std::endl;
        return 4;
    }
    return failures == 0 ? 0 : 1;
}

/// Peeked before the application object is built, because a headless run must
/// not create a QApplication and therefore must not need a display.
bool wantsHeadless(int argc, char** argv)
{
    for (int index = 1; index < argc; ++index) {
        if (std::string(argv[index]) == "--headless") {
            return true;
        }
    }
    return false;
}

} // namespace

int main(int argc, char** argv)
{
    const bool headless = wantsHeadless(argc, argv);

    std::unique_ptr<QCoreApplication> application;
    if (headless) {
        application = std::make_unique<QCoreApplication>(argc, argv);
    } else {
        application = std::make_unique<QApplication>(argc, argv);
    }

    QCoreApplication::setOrganizationName(QStringLiteral("HwSimPlatform"));
    QCoreApplication::setApplicationName(QStringLiteral("DeviceProtocolSimulator"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    const auto options = hwsim::app::CliOptions::parse(QCoreApplication::arguments());
    if (options.hasError()) {
        std::cerr << options.error().toString().toStdString() << std::endl;
        std::cerr << hwsim::app::CliOptions::helpText().toStdString() << std::endl;
        return 2;
    }

    auto logHistory = installLogging(options.value());
    HWSIM_LOG_INFO(kLogCategory) << "starting " << QCoreApplication::applicationName() << ' '
                                 << QCoreApplication::applicationVersion();

    hwsim::transport::registerTransportMetaTypes();

    hwsim::protocol::PluginManager plugins;
    const int pluginCount = loadPlugins(plugins, options.value());
    HWSIM_LOG_INFO(kLogCategory) << "loaded " << pluginCount << " protocol plugin(s)";

    hwsim::core::EventBus bus;
    hwsim::app::Workspace workspace(plugins, bus);

    int exitCode = 0;

    if (headless) {
        exitCode = runHeadless(options.value(), workspace);
    } else {
        hwsim::app::MainWindow window(workspace, plugins, bus, std::move(logHistory));

        if (!options.value().workspacePath.isEmpty()) {
            if (const auto opened = window.openWorkspace(options.value().workspacePath);
                opened.hasError()) {
                HWSIM_LOG_ERROR(kLogCategory) << opened.error().toString();
            } else if (options.value().autoStart) {
                const auto started = workspace.startAll();
                if (started.hasError()) {
                    HWSIM_LOG_WARNING(kLogCategory) << started.error().toString();
                }
            }
        }

        window.show();
        exitCode = QCoreApplication::exec();
    }

    workspace.clear();
    hwsim::core::Logger::instance().shutdown();
    return exitCode;
}
