#include "StreamLink.h"

#include <core/HexUtils.h>

namespace hwsim::transport {

StreamLink::StreamLink(LinkId id, QIODevice* device, QString peerDescription, QObject* parent)
    : ILink(id, parent), device_(device), peer_(std::move(peerDescription))
{
    if (device_ != nullptr) {
        device_->setParent(this);
        connect(device_, &QIODevice::readyRead, this, &StreamLink::onReadyRead);
    }
}

StreamLink::~StreamLink()
{
    // The device is a child, so ~QObject destroys it once this body returns,
    // and a socket emits disconnected on its way out. Whoever is still
    // listening -- typically the owning transport, which wires that signal to
    // removeLink -- would be told about a link that no longer exists. The link
    // owns the device outright, so cutting every connection from it here is
    // exactly the contract the header describes.
    if (!device_.isNull()) {
        device_->disconnect();
    }
}

QString StreamLink::peerDescription() const
{
    return peer_;
}

void StreamLink::setPeerDescription(QString description)
{
    peer_ = std::move(description);
}

void StreamLink::markConnected()
{
    setState(LinkState::Connected);
    // A device can have buffered data that arrived before the link existed.
    if (device_ != nullptr && device_->bytesAvailable() > 0) {
        onReadyRead();
    }
}

qint64 StreamLink::writeBytes(std::span<const std::byte> data)
{
    if (device_.isNull() || !device_->isOpen() || !device_->isWritable()) {
        reportError(core::makeError(core::ErrorCode::NotConnected,
                                    QStringLiteral("device is not writable"), peer_));
        return -1;
    }

    const qint64 written = device_->write(reinterpret_cast<const char*>(data.data()),
                                          static_cast<qint64>(data.size()));
    if (written < 0) {
        reportError(core::makeError(core::ErrorCode::IoError, device_->errorString(), peer_));
    }
    return written;
}

void StreamLink::closeImpl()
{
    if (!device_.isNull() && device_->isOpen()) {
        device_->close();
    }
}

void StreamLink::onReadyRead()
{
    if (device_.isNull()) {
        return;
    }
    const QByteArray chunk = device_->readAll();
    handleIncoming(chunk);
}

} // namespace hwsim::transport
