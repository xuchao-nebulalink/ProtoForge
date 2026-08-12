#pragma once

#include "ILink.h"
#include "TransportConfig.h"

#include <QList>
#include <QObject>
#include <QString>

#include <memory>
#include <vector>

namespace hwsim::transport {

/// A communication endpoint.
///
/// Subclasses implement openImpl()/closeImpl() and create links as peers appear.
/// Everything shared - link registry, statistics, state, error reporting - lives
/// here so a new transport is a small class.
///
/// Threading: an ITransport and all of its links live on a single thread, which
/// is normally a worker thread created by TransportThread. Call open(), close()
/// and send() from that thread; use TransportThread::post() from elsewhere.
class HWSIM_TRANSPORT_API ITransport : public QObject {
    Q_OBJECT

public:
    ~ITransport() override;

    [[nodiscard]] virtual TransportKind kind() const noexcept = 0;

    [[nodiscard]] QString name() const { return name_; }
    void setName(QString name) { name_ = std::move(name); }

    [[nodiscard]] const TransportConfig& config() const noexcept { return config_; }
    [[nodiscard]] QString describe() const;

    [[nodiscard]] core::Result<void> open(TransportConfig config);
    void close();
    [[nodiscard]] bool isOpen() const noexcept { return open_; }

    [[nodiscard]] QList<ILink*> links() const;
    [[nodiscard]] ILink* findLink(LinkId id) const;
    [[nodiscard]] qsizetype linkCount() const;

    /// Sends to every open link. Returns the number of links written to.
    qsizetype broadcast(const QByteArray& data);

    /// Convenience for single-link transports (client, serial, loopback).
    [[nodiscard]] ILink* primaryLink() const;

signals:
    void opened();
    void closed();

    /// Emitted on the transport thread with a pointer that is valid until the
    /// matching linkClosed. Connect with Qt::DirectConnection from the same
    /// thread; cross-thread observers use TransportEvents instead.
    void linkOpened(hwsim::transport::ILink* link);
    void linkClosed(hwsim::transport::LinkId id, const QString& reason);
    void errorOccurred(const hwsim::core::Error& error);

protected:
    explicit ITransport(QObject* parent = nullptr);

    [[nodiscard]] virtual core::Result<void> openImpl(const TransportConfig& config) = 0;
    virtual void closeImpl() = 0;

    /// Takes ownership and emits linkOpened.
    ILink* addLink(std::unique_ptr<ILink> link);

    /// Emits linkClosed and schedules destruction. Safe to call from inside one
    /// of the link's own signal handlers: the object is destroyed later, on the
    /// next turn of the event loop.
    void removeLink(LinkId id, const QString& reason);
    void removeAllLinks(const QString& reason);

    void reportError(core::Error error);

private:
    void collectGarbage();

    QString name_;
    TransportConfig config_;
    bool open_{false};
    std::vector<std::unique_ptr<ILink>> links_;
    std::vector<std::unique_ptr<ILink>> retired_;
    bool garbageCollectionScheduled_{false};
};

} // namespace hwsim::transport
