#pragma once

#include "ILink.h"

#include <QIODevice>
#include <QPointer>

namespace hwsim::transport {

/// Link backed by any byte-stream QIODevice.
///
/// QTcpSocket and QSerialPort differ in how they are opened and how they report
/// disconnection, but once running they are the same read/write device, so both
/// share this implementation.
///
/// Ownership: the constructor reparents the device onto the link, so the link
/// destroys it. A transport must not delete a device it has handed over, and
/// must keep any of its own connections to that device valid only for as long
/// as the link lives.
class HWSIM_TRANSPORT_API StreamLink : public ILink {
    Q_OBJECT

public:
    /// Takes ownership of `device`.
    StreamLink(LinkId id, QIODevice* device, QString peerDescription, QObject* parent = nullptr);
    ~StreamLink() override;

    [[nodiscard]] QString peerDescription() const override;
    void setPeerDescription(QString description);

    [[nodiscard]] QIODevice* device() const { return device_; }

    /// Marks the link connected and starts forwarding reads.
    void markConnected();

protected:
    qint64 writeBytes(std::span<const std::byte> data) override;
    void closeImpl() override;

private:
    void onReadyRead();

    QPointer<QIODevice> device_;
    QString peer_;
};

} // namespace hwsim::transport
