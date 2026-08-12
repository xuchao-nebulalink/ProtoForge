#pragma once

#include "IDeviceController.h"
#include "UiGlobal.h"

#include <QWidget>

class QCheckBox;
class QTableWidget;
class QTimer;

namespace hwsim::ui {

/// Live-data and fault-injection control for one device.
///
/// Both halves are driven entirely by the schemas that the signal sources and
/// fault rules publish, so a new waveform or a new fault kind shows up here
/// with a working editor and no changes to this file.
class HWSIM_UI_API ScenarioPanel : public QWidget {
    Q_OBJECT

public:
    explicit ScenarioPanel(QWidget* parent = nullptr);

    void setController(IDeviceController* controller);
    void setDevice(const QString& deviceId);
    [[nodiscard]] QString deviceId() const { return deviceId_; }

    void refresh();

signals:
    void statusMessage(const QString& message);

private:
    [[nodiscard]] QWidget* buildSignalsTab();
    [[nodiscard]] QWidget* buildFaultsTab();

    void refreshSignals();
    void refreshFaults();

    void addSignalBinding();
    void removeSelectedSignalBinding();
    void addFaultRule();
    void removeSelectedFaultRule();
    void armSelectedFaultRule();

    [[nodiscard]] QString selectedId(QTableWidget* table) const;

    IDeviceController* controller_{nullptr};
    QString deviceId_;

    QTableWidget* signalTable_{nullptr};
    QTableWidget* faultTable_{nullptr};
    QCheckBox* faultMasterSwitch_{nullptr};
    QTimer* refreshTimer_{nullptr};
};

} // namespace hwsim::ui
