#pragma once

#include <core/EventBus.h>
#include <protocol/PluginManager.h>
#include <protocol/ProtocolSession.h>
#include <simulator/DeviceModel.h>
#include <simulator/DeviceProfile.h>
#include <transport/ITransport.h>
#include <transport/TransportThread.h>

#include <QObject>

#include <map>
#include <memory>

namespace hwsim::app {

/// One running device: its endpoint, its protocol stack, its device model, and
/// the thread they all live on.
///
/// This is where the layers are finally wired together, and it is the only
/// class that knows about all of them. Everything it owns is moved onto a
/// private worker thread, so the whole pipeline - socket reads, framing,
/// dispatch, register access, fault injection - runs single-threaded and
/// lock-free. The UI never touches these objects; it goes through Workspace,
/// which marshals calls onto this thread.
class DeviceRuntime : public QObject {
    Q_OBJECT

public:
    struct Config {
        QString id;
        QString name;

        QString protocolId;
        QVariantMap protocolConfig;

        transport::TransportConfig transport;
        transport::TransportRole role{transport::TransportRole::Responder};

        simulator::DeviceProfile profile;

        int responseTimeoutMs{1000};
        bool traceFrames{true};
    };

    ~DeviceRuntime() override;

    /// Builds the runtime but does not open the endpoint yet.
    [[nodiscard]] static core::Result<std::unique_ptr<DeviceRuntime>> create(
        Config config, protocol::PluginManager& plugins, core::EventBus& bus);

    [[nodiscard]] core::Result<void> start();
    void stop();
    [[nodiscard]] bool isRunning() const;

    [[nodiscard]] const Config& config() const noexcept { return config_; }
    [[nodiscard]] QString id() const { return config_.id; }
    [[nodiscard]] QString name() const { return config_.name; }

    /// Runs `work` on the device thread and waits. Every accessor below uses
    /// this, which is what makes it safe for the UI to call them directly.
    /// Const because it does not change the runtime itself, only marshals.
    void invoke(const std::function<void()>& work) const;

    [[nodiscard]] simulator::DeviceModel* deviceUnsafe() const noexcept { return device_.get(); }

    [[nodiscard]] int linkCount() const;
    [[nodiscard]] QStringList linkDescriptions() const;

    /// Writes raw bytes to the first open link, bypassing encoding. Used by the
    /// manual send box and by tests that inject malformed frames.
    [[nodiscard]] core::Result<void> sendRaw(const QByteArray& bytes);

    [[nodiscard]] protocol::ProtocolSession::Counters aggregateCounters() const;

signals:
    void linkCountChanged(int count);
    void started();
    void stopped();

private:
    explicit DeviceRuntime(Config config, core::EventBus& bus);

    [[nodiscard]] core::Result<void> buildDevice();

    /// Closes and destroys everything that belongs to the worker thread, on
    /// that thread, and hands the device model back to the main thread so the
    /// runtime can be started again.
    void teardownOnWorkerThread();

    void onLinkOpened(transport::ILink* link);
    void onLinkClosed(transport::LinkId id, const QString& reason);

    Config config_;
    core::EventBus* bus_{nullptr};
    protocol::IProtocolPlugin* plugin_{nullptr};

    std::unique_ptr<transport::TransportThread> thread_;
    std::unique_ptr<transport::ITransport> transport_;
    std::unique_ptr<simulator::DeviceModel> device_;

    /// Shared by every session on this device: the binding set is identical,
    /// and handlers are stateless with respect to the link.
    std::shared_ptr<protocol::CommandRegistry> registry_;

    std::map<transport::LinkId, std::unique_ptr<protocol::ProtocolSession>> sessions_;
    protocol::ProtocolSession::Counters retiredCounters_;
    bool running_{false};
};

} // namespace hwsim::app
