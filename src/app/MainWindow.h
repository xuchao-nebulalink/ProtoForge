#pragma once

#include "ScriptHostAdapter.h"
#include "Workspace.h"

#include <scripting/ScriptEngine.h>
#include <ui/DeviceTreeWidget.h>
#include <ui/LogDockWidget.h>
#include <ui/PacketView.h>
#include <ui/ParameterPanel.h>
#include <ui/ScenarioPanel.h>

#include <QMainWindow>

#include <memory>

namespace hwsim::app {

/// Main window: device tree on the left, packet traffic in the centre,
/// parameters and scenario controls on the right, log along the bottom.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(Workspace& workspace, protocol::PluginManager& plugins, core::EventBus& bus,
               std::shared_ptr<core::RingBufferLogSink> logHistory, QWidget* parent = nullptr);
    ~MainWindow() override;

    [[nodiscard]] core::Result<void> openWorkspace(const QString& path);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void buildDocks();
    void buildMenus();
    void refreshDeviceTree();

    void onAddDevice();
    void onRemoveDevice();
    void onStartAll();
    void onStopAll();
    void onSaveWorkspace();
    void onSaveWorkspaceAs();
    void onOpenWorkspace();
    void onRunScript();
    void onShowPluginInfo();
    void onDeviceSelected(const QString& deviceId);
    void onManualSend(const QString& deviceId, const QByteArray& bytes);

    void restoreLayout();
    void saveLayout() const;

    Workspace* workspace_{nullptr};
    protocol::PluginManager* plugins_{nullptr};
    core::EventBus* bus_{nullptr};

    ui::DeviceTreeWidget* deviceTree_{nullptr};
    ui::PacketView* packetView_{nullptr};
    ui::ParameterPanel* parameterPanel_{nullptr};
    ui::ScenarioPanel* scenarioPanel_{nullptr};
    ui::LogDockWidget* logDock_{nullptr};

    ScriptHostAdapter scriptHost_;
    std::unique_ptr<scripting::ScriptEngine> scriptEngine_;

    QString workspacePath_;
    QString selectedDeviceId_;
};

} // namespace hwsim::app
