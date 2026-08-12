#pragma once

#include "ITransport.h"

#include <QStringList>

class QSerialPort;

namespace hwsim::transport {

/// Serial endpoint. Point to point, so it carries exactly one link for as long
/// as the port is open.
class HWSIM_TRANSPORT_API SerialTransport final : public ITransport {
    Q_OBJECT

public:
    explicit SerialTransport(QObject* parent = nullptr);
    ~SerialTransport() override;

    [[nodiscard]] TransportKind kind() const noexcept override { return TransportKind::Serial; }

    /// Port names available on this machine, for the configuration panel.
    [[nodiscard]] static QStringList availablePorts();

protected:
    [[nodiscard]] core::Result<void> openImpl(const TransportConfig& config) override;
    void closeImpl() override;

private:
    [[nodiscard]] core::Result<void> applySettings(QSerialPort& port, const TransportConfig& config);

    QSerialPort* port_{nullptr};
    LinkId currentLinkId_{kInvalidLinkId};
};

} // namespace hwsim::transport
