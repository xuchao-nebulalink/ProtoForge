#include "MainWindow.h"

#include "AddDeviceDialog.h"

#include <core/Logger.h>

#include <QCloseEvent>
#include <QDockWidget>
#include <QFileDialog>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>

namespace {
constexpr auto kLogCategory = "app.ui";
}

namespace hwsim::app {

MainWindow::MainWindow(Workspace& workspace, protocol::PluginManager& plugins, core::EventBus& bus,
                       std::shared_ptr<core::RingBufferLogSink> logHistory, QWidget* parent)
    : QMainWindow(parent), workspace_(&workspace), plugins_(&plugins), bus_(&bus),
      scriptHost_(workspace)
{
    setWindowTitle(QStringLiteral("设备协议模拟平台"));
    resize(1500, 900);

    scriptEngine_ = std::make_unique<scripting::ScriptEngine>(scriptHost_);

    buildDocks();
    buildMenus();

    logDock_->attachToLogger(std::move(logHistory));
    packetView_->attachToEventBus(bus_);

    parameterPanel_->setController(workspace_);
    scenarioPanel_->setController(workspace_);

    connect(workspace_, &Workspace::devicesChanged, this, &MainWindow::refreshDeviceTree);

    // Per-device updates arrive on every link connect and disconnect. Rebuilding
    // the whole tree for those would discard the user's selection and expansion
    // state, and cost a blocking round trip to every device thread.
    connect(workspace_, &Workspace::deviceUpdated, this, [this](const QString& deviceId) {
        deviceTree_->updateDevice(workspace_->deviceNode(deviceId));
    });

    restoreLayout();
    refreshDeviceTree();

    statusBar()->showMessage(
        QStringLiteral("已加载 %1 个协议插件").arg(plugins_->plugins().size()));
}

MainWindow::~MainWindow()
{
    packetView_->detachFromEventBus();
    logDock_->detachFromLogger();
}

void MainWindow::buildDocks()
{
    packetView_ = new ui::PacketView(this);
    setCentralWidget(packetView_);
    connect(packetView_, &ui::PacketView::manualSendRequested, this, &MainWindow::onManualSend);

    deviceTree_ = new ui::DeviceTreeWidget(this);
    auto* treeDock = new QDockWidget(QStringLiteral("设备树"), this);
    treeDock->setObjectName(QStringLiteral("deviceTreeDock"));
    treeDock->setWidget(deviceTree_);
    addDockWidget(Qt::LeftDockWidgetArea, treeDock);
    connect(deviceTree_, &ui::DeviceTreeWidget::deviceSelected, this,
            &MainWindow::onDeviceSelected);

    parameterPanel_ = new ui::ParameterPanel(this);
    scenarioPanel_ = new ui::ScenarioPanel(this);

    auto* rightTabs = new QTabWidget(this);
    rightTabs->addTab(parameterPanel_, QStringLiteral("参数"));
    rightTabs->addTab(scenarioPanel_, QStringLiteral("场景"));

    auto* rightDock = new QDockWidget(QStringLiteral("设备配置"), this);
    rightDock->setObjectName(QStringLiteral("configDock"));
    rightDock->setWidget(rightTabs);
    addDockWidget(Qt::RightDockWidgetArea, rightDock);

    logDock_ = new ui::LogDockWidget(this);
    auto* logDockWidget = new QDockWidget(QStringLiteral("日志"), this);
    logDockWidget->setObjectName(QStringLiteral("logDock"));
    logDockWidget->setWidget(logDock_);
    addDockWidget(Qt::BottomDockWidgetArea, logDockWidget);

    connect(parameterPanel_, &ui::ParameterPanel::statusMessage, statusBar(),
            [this](const QString& message) { statusBar()->showMessage(message, 4000); });
    connect(scenarioPanel_, &ui::ScenarioPanel::statusMessage, statusBar(),
            [this](const QString& message) { statusBar()->showMessage(message, 4000); });
}

void MainWindow::buildMenus()
{
    // Actions are created explicitly rather than through the addAction(text,
    // shortcut, context, slot) convenience overloads: those are deprecated on
    // QMenu in Qt 6.5, and the surviving QWidget variadic form is easy to get
    // wrong because the shortcut sits between the text and the receiver.
    const auto makeAction = [this](QMenu* menu, const QString& text,
                                   void (MainWindow::*slot)(),
                                   QKeySequence::StandardKey shortcut = QKeySequence::UnknownKey) {
        QAction* action = menu->addAction(text);
        if (shortcut != QKeySequence::UnknownKey) {
            action->setShortcut(shortcut);
        }
        connect(action, &QAction::triggered, this, slot);
        return action;
    };

    QMenu* fileMenu = menuBar()->addMenu(QStringLiteral("文件(&F)"));
    makeAction(fileMenu, QStringLiteral("打开工程..."), &MainWindow::onOpenWorkspace,
               QKeySequence::Open);
    makeAction(fileMenu, QStringLiteral("保存工程"), &MainWindow::onSaveWorkspace,
               QKeySequence::Save);
    makeAction(fileMenu, QStringLiteral("工程另存为..."), &MainWindow::onSaveWorkspaceAs);
    fileMenu->addSeparator();

    QAction* quitAction = fileMenu->addAction(QStringLiteral("退出"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    QMenu* deviceMenu = menuBar()->addMenu(QStringLiteral("设备(&D)"));
    QAction* addAction = makeAction(deviceMenu, QStringLiteral("添加设备..."),
                                    &MainWindow::onAddDevice);
    QAction* removeAction = makeAction(deviceMenu, QStringLiteral("移除选中设备"),
                                       &MainWindow::onRemoveDevice);
    deviceMenu->addSeparator();
    QAction* startAction = makeAction(deviceMenu, QStringLiteral("全部启动"),
                                      &MainWindow::onStartAll);
    QAction* stopAction = makeAction(deviceMenu, QStringLiteral("全部停止"),
                                     &MainWindow::onStopAll);

    QMenu* toolsMenu = menuBar()->addMenu(QStringLiteral("工具(&T)"));
    QAction* scriptAction = makeAction(toolsMenu, QStringLiteral("运行脚本..."),
                                       &MainWindow::onRunScript);
    makeAction(toolsMenu, QStringLiteral("插件信息..."), &MainWindow::onShowPluginInfo);

    auto* toolbar = addToolBar(QStringLiteral("主工具栏"));
    toolbar->setObjectName(QStringLiteral("mainToolBar"));
    toolbar->addAction(addAction);
    toolbar->addAction(removeAction);
    toolbar->addSeparator();
    toolbar->addAction(startAction);
    toolbar->addAction(stopAction);
    toolbar->addSeparator();
    toolbar->addAction(scriptAction);
}

void MainWindow::refreshDeviceTree()
{
    deviceTree_->setDevices(workspace_->deviceNodes());
}

void MainWindow::onDeviceSelected(const QString& deviceId)
{
    if (deviceId == selectedDeviceId_) {
        return;
    }
    selectedDeviceId_ = deviceId;
    parameterPanel_->setDevice(deviceId);
    scenarioPanel_->setDevice(deviceId);
}

void MainWindow::onAddDevice()
{
    if (plugins_->plugins().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("没有协议插件"),
                             QStringLiteral("未找到任何协议插件，请检查 bin/plugins 目录。"));
        return;
    }

    AddDeviceDialog dialog(*plugins_, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const auto added = workspace_->addDevice(dialog.configuration());
    if (added.hasError()) {
        QMessageBox::warning(this, QStringLiteral("添加失败"), added.error().toString());
        return;
    }

    refreshDeviceTree();
    onDeviceSelected(added.value());

    if (const auto started = workspace_->startDevice(added.value()); started.hasError()) {
        statusBar()->showMessage(
            QStringLiteral("设备已添加但启动失败: %1").arg(started.error().toString()), 8000);
    } else {
        statusBar()->showMessage(QStringLiteral("设备 %1 已启动").arg(added.value()), 4000);
    }
}

void MainWindow::onRemoveDevice()
{
    const QString deviceId = deviceTree_->selectedDeviceId();
    if (deviceId.isEmpty()) {
        return;
    }

    const auto answer = QMessageBox::question(
        this, QStringLiteral("移除设备"),
        QStringLiteral("确定要移除设备 '%1' 吗？").arg(deviceId));
    if (answer != QMessageBox::Yes) {
        return;
    }

    workspace_->removeDevice(deviceId);
    selectedDeviceId_.clear();
    parameterPanel_->setDevice({});
    scenarioPanel_->setDevice({});
    refreshDeviceTree();
}

void MainWindow::onStartAll()
{
    if (const auto started = workspace_->startAll(); started.hasError()) {
        statusBar()->showMessage(
            QStringLiteral("部分设备启动失败: %1").arg(started.error().toString()), 8000);
    } else {
        statusBar()->showMessage(QStringLiteral("全部设备已启动"), 4000);
    }
    refreshDeviceTree();
}

void MainWindow::onStopAll()
{
    workspace_->stopAll();
    statusBar()->showMessage(QStringLiteral("全部设备已停止"), 4000);
    refreshDeviceTree();
}

void MainWindow::onOpenWorkspace()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("打开工程"), {}, QStringLiteral("工程文件 (*.json);;所有文件 (*)"));
    if (path.isEmpty()) {
        return;
    }
    if (const auto opened = openWorkspace(path); opened.hasError()) {
        QMessageBox::warning(this, QStringLiteral("打开失败"), opened.error().toString());
    }
}

core::Result<void> MainWindow::openWorkspace(const QString& path)
{
    const auto loaded = workspace_->load(path);
    if (loaded.hasError()) {
        return loaded;
    }

    workspacePath_ = path;
    setWindowTitle(QStringLiteral("设备协议模拟平台 - %1").arg(path));
    refreshDeviceTree();
    return core::success();
}

void MainWindow::onSaveWorkspace()
{
    if (workspacePath_.isEmpty()) {
        onSaveWorkspaceAs();
        return;
    }
    if (const auto saved = workspace_->save(workspacePath_); saved.hasError()) {
        QMessageBox::warning(this, QStringLiteral("保存失败"), saved.error().toString());
        return;
    }
    statusBar()->showMessage(QStringLiteral("已保存到 %1").arg(workspacePath_), 4000);
}

void MainWindow::onSaveWorkspaceAs()
{
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("保存工程"), QStringLiteral("workspace.json"),
        QStringLiteral("工程文件 (*.json)"));
    if (path.isEmpty()) {
        return;
    }
    workspacePath_ = path;
    onSaveWorkspace();
}

void MainWindow::onRunScript()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("运行场景脚本"), {}, QStringLiteral("JavaScript (*.js);;所有文件 (*)"));
    if (path.isEmpty()) {
        return;
    }

    const auto outcome = scriptEngine_->runFile(path);
    statusBar()->showMessage(outcome.summary(), 10000);

    if (!outcome.completed || !outcome.passed) {
        QMessageBox::warning(this, QStringLiteral("脚本结果"),
                             outcome.summary() + QStringLiteral("\n\n")
                                 + (outcome.errorText.isEmpty()
                                        ? outcome.failures.join(QLatin1Char('\n'))
                                        : outcome.errorText));
    } else {
        QMessageBox::information(this, QStringLiteral("脚本结果"), outcome.summary());
    }
}

void MainWindow::onShowPluginInfo()
{
    QStringList lines;
    for (const auto& loaded : plugins_->plugins()) {
        lines.append(QStringLiteral("%1 %2  [%3]  %4")
                         .arg(loaded.metadata.id, loaded.metadata.version,
                              loaded.isStatic ? QStringLiteral("静态") : QStringLiteral("动态"),
                              loaded.metadata.description));
    }

    for (const auto& failure : plugins_->failures()) {
        lines.append(QStringLiteral("加载失败: %1 - %2").arg(failure.source, failure.reason));
    }

    QMessageBox::information(this, QStringLiteral("协议插件"),
                             lines.isEmpty() ? QStringLiteral("没有加载任何插件。")
                                             : lines.join(QLatin1Char('\n')));
}

void MainWindow::onManualSend(const QString& deviceId, const QByteArray& bytes)
{
    const QString target = deviceId.isEmpty() ? selectedDeviceId_ : deviceId;
    if (target.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("请先选择目标设备"), 4000);
        return;
    }

    if (const auto sent = workspace_->sendRaw(target, bytes); sent.hasError()) {
        statusBar()->showMessage(QStringLiteral("发送失败: %1").arg(sent.error().toString()), 6000);
        return;
    }
    statusBar()->showMessage(QStringLiteral("已发送 %1 字节").arg(bytes.size()), 3000);
}

void MainWindow::restoreLayout()
{
    QSettings settings;
    restoreGeometry(settings.value(QStringLiteral("mainWindow/geometry")).toByteArray());
    restoreState(settings.value(QStringLiteral("mainWindow/state")).toByteArray());
}

void MainWindow::saveLayout() const
{
    QSettings settings;
    settings.setValue(QStringLiteral("mainWindow/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("mainWindow/state"), saveState());
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    saveLayout();
    workspace_->stopAll();

    HWSIM_LOG_INFO(kLogCategory) << "shutting down";
    event->accept();
}

} // namespace hwsim::app
