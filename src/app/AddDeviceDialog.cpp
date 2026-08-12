#include "AddDeviceDialog.h"

#include <transport/TransportFactory.h>
#include <ui/SchemaFormWidget.h>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QScrollArea>
#include <QTabWidget>
#include <QVBoxLayout>

namespace hwsim::app {
namespace {

QScrollArea* wrapInScrollArea(QWidget* content, QWidget* parent)
{
    auto* scroll = new QScrollArea(parent);
    scroll->setWidgetResizable(true);
    scroll->setWidget(content);
    scroll->setFrameShape(QFrame::NoFrame);
    return scroll;
}

} // namespace

AddDeviceDialog::AddDeviceDialog(protocol::PluginManager& plugins, QWidget* parent)
    : QDialog(parent), plugins_(&plugins)
{
    setWindowTitle(QStringLiteral("添加设备"));
    resize(560, 640);

    nameEdit_ = new QLineEdit(QStringLiteral("device-1"), this);

    protocolCombo_ = new QComboBox(this);
    for (const auto& loaded : plugins.plugins()) {
        protocolCombo_->addItem(
            QStringLiteral("%1 (%2)").arg(loaded.metadata.displayName, loaded.metadata.id),
            loaded.metadata.id);
    }

    roleCombo_ = new QComboBox(this);
    roleCombo_->addItem(QStringLiteral("模拟设备 (从站，等待上位机连接)"),
                        static_cast<int>(transport::TransportRole::Responder));
    roleCombo_->addItem(QStringLiteral("测试主站 (主动连接真实设备)"),
                        static_cast<int>(transport::TransportRole::Initiator));

    transportCombo_ = new QComboBox(this);
    for (const transport::TransportKind kind :
         transport::TransportFactory::instance().availableKinds()) {
        transportCombo_->addItem(transport::transportKindDisplayName(kind),
                                 static_cast<int>(kind));
    }

    transportForm_ = new ui::SchemaFormWidget(this);
    protocolForm_ = new ui::SchemaFormWidget(this);

    auto* tabs = new QTabWidget(this);
    tabs->addTab(wrapInScrollArea(transportForm_, this), QStringLiteral("通讯"));
    tabs->addTab(wrapInScrollArea(protocolForm_, this), QStringLiteral("协议"));

    auto* form = new QFormLayout;
    form->addRow(QStringLiteral("设备名称"), nameEdit_);
    form->addRow(QStringLiteral("协议"), protocolCombo_);
    form->addRow(QStringLiteral("角色"), roleCombo_);
    form->addRow(QStringLiteral("通讯方式"), transportCombo_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(tabs, 1);
    layout->addWidget(buttons);

    connect(protocolCombo_, &QComboBox::currentIndexChanged, this,
            &AddDeviceDialog::onProtocolChanged);
    connect(transportCombo_, &QComboBox::currentIndexChanged, this,
            &AddDeviceDialog::onTransportKindChanged);
    connect(buttons, &QDialogButtonBox::accepted, this, &AddDeviceDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    onTransportKindChanged();
    onProtocolChanged();
}

void AddDeviceDialog::onTransportKindChanged()
{
    const auto kind = static_cast<transport::TransportKind>(transportCombo_->currentData().toInt());
    transportForm_->setSchema(transport::TransportConfig::schemaFor(kind));
}

void AddDeviceDialog::onProtocolChanged()
{
    const QString id = protocolCombo_->currentData().toString();
    if (protocol::IProtocolPlugin* plugin = plugins_->find(id); plugin != nullptr) {
        protocolForm_->setSchema(plugin->configSchema());
    } else {
        protocolForm_->clearSchema();
    }
}

void AddDeviceDialog::accept()
{
    if (nameEdit_->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("缺少名称"),
                             QStringLiteral("请填写设备名称。"));
        return;
    }
    if (protocolCombo_->currentData().toString().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("没有可用协议"),
                             QStringLiteral("未加载任何协议插件，请检查 plugins 目录。"));
        return;
    }
    if (const auto valid = transportForm_->validate(); valid.hasError()) {
        QMessageBox::warning(this, QStringLiteral("通讯配置无效"), valid.error().toString());
        return;
    }
    if (const auto valid = protocolForm_->validate(); valid.hasError()) {
        QMessageBox::warning(this, QStringLiteral("协议配置无效"), valid.error().toString());
        return;
    }

    const auto kind = static_cast<transport::TransportKind>(transportCombo_->currentData().toInt());
    const auto role = static_cast<transport::TransportRole>(roleCombo_->currentData().toInt());

    result_ = DeviceRuntime::Config{};
    result_.id = nameEdit_->text().trimmed();
    result_.name = result_.id;
    result_.protocolId = protocolCombo_->currentData().toString();
    result_.protocolConfig = protocolForm_->values();
    result_.transport = transport::TransportConfig(kind, transportForm_->values());
    result_.transport.setRole(role);
    result_.role = role;

    // Seed the register map from the plugin's own template so a freshly created
    // device is immediately usable instead of empty.
    if (protocol::IProtocolPlugin* plugin = plugins_->find(result_.protocolId); plugin != nullptr) {
        result_.profile.name = result_.name;
        result_.profile.protocolId = result_.protocolId;
        result_.profile.parameters = plugin->defaultParameterTemplate();
    }

    QDialog::accept();
}

DeviceRuntime::Config AddDeviceDialog::configuration() const
{
    return result_;
}

} // namespace hwsim::app
