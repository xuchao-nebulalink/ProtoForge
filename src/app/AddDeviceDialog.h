#pragma once

#include "DeviceRuntime.h"

#include <QDialog>

class QComboBox;
class QLineEdit;
class QTabWidget;

namespace hwsim::ui {
class SchemaFormWidget;
}

namespace hwsim::app {

/// New-device wizard.
///
/// Every page is generated: the transport tab from TransportConfig::schemaFor,
/// the protocol tab from the plugin's own configSchema. Adding a transport type
/// or a protocol plugin therefore extends this dialog without touching it.
class AddDeviceDialog : public QDialog {
    Q_OBJECT

public:
    AddDeviceDialog(protocol::PluginManager& plugins, QWidget* parent = nullptr);

    /// Not named result(): QDialog::result() is a non-virtual member that
    /// returns the accept/reject code, and shadowing it invites a silent mix-up.
    [[nodiscard]] DeviceRuntime::Config configuration() const;

private:
    void onProtocolChanged();
    void onTransportKindChanged();
    void accept() override;

    protocol::PluginManager* plugins_{nullptr};

    QLineEdit* nameEdit_{nullptr};
    QComboBox* protocolCombo_{nullptr};
    QComboBox* roleCombo_{nullptr};
    QComboBox* transportCombo_{nullptr};

    ui::SchemaFormWidget* transportForm_{nullptr};
    ui::SchemaFormWidget* protocolForm_{nullptr};

    DeviceRuntime::Config result_;
};

} // namespace hwsim::app
