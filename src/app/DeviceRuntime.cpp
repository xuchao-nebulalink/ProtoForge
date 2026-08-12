#include "DeviceRuntime.h"

#include <core/Logger.h>
#include <transport/TransportFactory.h>

#include <QCoreApplication>

namespace {
constexpr auto kLogCategory = "app.device";
}

using hwsim::core::ErrorCode;
using hwsim::core::makeError;
using hwsim::core::Result;

namespace hwsim::app {

DeviceRuntime::DeviceRuntime(Config config, core::EventBus& bus)
    : config_(std::move(config)), bus_(&bus)
{
}

DeviceRuntime::~DeviceRuntime()
{
    stop();
}

Result<std::unique_ptr<DeviceRuntime>> DeviceRuntime::create(Config config,
                                                             protocol::PluginManager& plugins,
                                                             core::EventBus& bus)
{
    protocol::IProtocolPlugin* plugin = plugins.find(config.protocolId);
    if (plugin == nullptr) {
        return makeError(ErrorCode::PluginError,
                         QStringLiteral("protocol plugin '%1' is not loaded").arg(config.protocolId),
                         config.name);
    }

    // The role is part of the configuration handed to the plugin, because it
    // decides whether the registry gets request handlers or response decoders.
    config.protocolConfig.insert(QStringLiteral("role"),
                                 config.role == transport::TransportRole::Initiator
                                     ? QStringLiteral("initiator")
                                     : QStringLiteral("responder"));

    std::unique_ptr<DeviceRuntime> runtime(new DeviceRuntime(std::move(config), bus));
    runtime->plugin_ = plugin;

    if (const auto built = runtime->buildDevice(); built.hasError()) {
        return built.error();
    }

    runtime->registry_ = std::make_shared<protocol::CommandRegistry>();
    if (const auto registered =
            plugin->registerCommands(*runtime->registry_, runtime->config_.protocolConfig);
        registered.hasError()) {
        return registered.error();
    }

    return std::move(runtime);
}

Result<void> DeviceRuntime::buildDevice()
{
    device_ = std::make_unique<simulator::DeviceModel>(config_.name);
    device_->setEventBus(bus_);

    // A profile is optional: an initiator-only runtime has no device model of
    // its own, it just drives someone else's.
    if (!config_.profile.name.isEmpty()) {
        if (const auto applied = config_.profile.applyTo(*device_); applied.hasError()) {
            return applied;
        }
    } else if (!config_.profile.parameters.isEmpty()) {
        if (const auto loaded = device_->parameters().loadDefinitions(config_.profile.parameters);
            loaded.hasError()) {
            return loaded;
        }
    }

    device_->setName(config_.name);
    return core::success();
}

Result<void> DeviceRuntime::start()
{
    if (running_) {
        return core::success();
    }

    auto created = transport::TransportFactory::instance().create(config_.transport.kind());
    if (created.hasError()) {
        return created.error();
    }
    transport_ = std::move(created).value();
    transport_->setName(config_.name);

    thread_ = std::make_unique<transport::TransportThread>(
        QStringLiteral("io-%1").arg(config_.name));
    thread_->start();

    // Everything the pipeline touches has to belong to the worker thread before
    // any of it starts running.
    thread_->adopt(transport_.get());
    thread_->adopt(device_.get());

    connect(transport_.get(), &transport::ITransport::linkOpened, this,
            &DeviceRuntime::onLinkOpened, Qt::DirectConnection);
    connect(transport_.get(), &transport::ITransport::linkClosed, this,
            &DeviceRuntime::onLinkClosed, Qt::DirectConnection);

    // A DisconnectFault fires on a specific link, so the callback resolves the
    // link by id at the moment it triggers rather than capturing a pointer that
    // may already have been retired. Registered once per runtime, not per link.
    device_->faults().setDisconnectRequest(
        [this](transport::LinkId linkId, const QString& reason) {
            if (transport_ == nullptr) {
                return;
            }
            if (transport::ILink* target = transport_->findLink(linkId); target != nullptr) {
                HWSIM_LOG_DEBUG(kLogCategory) << "closing link " << linkId << ": " << reason;
                target->close();
            }
        });

    Result<void> openResult = core::success();
    thread_->invokeBlocking([this, &openResult] {
        openResult = transport_->open(config_.transport);
        if (openResult.hasValue()) {
            device_->start();
        }
    });

    if (openResult.hasError()) {
        teardownOnWorkerThread();
        thread_->stop();
        thread_.reset();
        return openResult;
    }

    running_ = true;
    HWSIM_LOG_INFO(kLogCategory) << "device '" << config_.name << "' started on "
                                 << config_.transport.describe();
    emit started();
    return core::success();
}

void DeviceRuntime::stop()
{
    if (!running_) {
        return;
    }
    running_ = false;

    if (thread_ != nullptr) {
        teardownOnWorkerThread();
        thread_->stop();
    }

    thread_.reset();

    HWSIM_LOG_INFO(kLogCategory) << "device '" << config_.name << "' stopped";
    emit stopped();
}

void DeviceRuntime::teardownOnWorkerThread()
{
    if (thread_ == nullptr) {
        return;
    }

    thread_->invokeBlocking([this] {
        sessions_.clear();

        // The transport and its sockets belong to this thread, so they must be
        // destroyed here. Destroying them on the main thread after the worker
        // thread is gone trips Qt's "socket notifiers cannot be disabled from
        // another thread" check.
        if (transport_ != nullptr) {
            transport_->close();
            transport_.reset();
        }

        if (device_ != nullptr) {
            device_->stop();
            // Hand the device back to the main thread. Without this its thread
            // affinity would still point at the QThread we are about to delete,
            // and a later start() could not move it again: moveToThread only
            // works when called from the object's current thread.
            device_->moveToThread(QCoreApplication::instance()->thread());
        }
    });
}

bool DeviceRuntime::isRunning() const
{
    return running_;
}

void DeviceRuntime::invoke(const std::function<void()>& work) const
{
    if (thread_ != nullptr && thread_->isRunning()) {
        thread_->invokeBlocking(work);
    } else {
        work();
    }
}

void DeviceRuntime::onLinkOpened(transport::ILink* link)
{
    if (link == nullptr) {
        return;
    }

    auto codec = plugin_->createCodec(config_.protocolConfig);
    if (codec.hasError()) {
        HWSIM_LOG_ERROR(kLogCategory)
            << config_.name << ": cannot create codec: " << codec.error().toString();
        return;
    }

    protocol::ProtocolSession::Options options;
    options.name = QStringLiteral("%1#%2").arg(config_.name).arg(link->id());
    options.deviceName = config_.name;
    options.role = config_.role;
    options.defaultTimeoutMs = config_.responseTimeoutMs;
    options.publishEvents = config_.traceFrames;

    auto session = std::make_unique<protocol::ProtocolSession>(link, std::move(codec).value(),
                                                               registry_, options);
    session->setDevice(device_.get());
    session->setEventBus(bus_);

    session->middleware().add(std::make_shared<protocol::DeviceStateGateMiddleware>());
    if (config_.traceFrames) {
        session->middleware().add(std::make_shared<protocol::TracingMiddleware>());
    }

    device_->faults().attachTo(*session);

    sessions_.emplace(link->id(), std::move(session));
    emit linkCountChanged(static_cast<int>(sessions_.size()));
}

void DeviceRuntime::onLinkClosed(transport::LinkId id, const QString& reason)
{
    Q_UNUSED(reason)

    // Fold the dying session's counters into the running total, otherwise
    // aggregateCounters() would go backwards every time a peer reconnects.
    if (const auto it = sessions_.find(id); it != sessions_.end()) {
        const auto counters = it->second->counters();
        retiredCounters_.framesDecoded += counters.framesDecoded;
        retiredCounters_.framesSent += counters.framesSent;
        retiredCounters_.decodeFailures += counters.decodeFailures;
        retiredCounters_.handlerFailures += counters.handlerFailures;
        retiredCounters_.resyncBytes += counters.resyncBytes;
        retiredCounters_.droppedByFilter += counters.droppedByFilter;
        retiredCounters_.requestTimeouts += counters.requestTimeouts;
        sessions_.erase(it);
    }

    emit linkCountChanged(static_cast<int>(sessions_.size()));
}

int DeviceRuntime::linkCount() const
{
    // sessions_ is mutated on the worker thread by onLinkOpened, so even a
    // size() read has to be marshalled there.
    int count = 0;
    invoke([this, &count] { count = static_cast<int>(sessions_.size()); });
    return count;
}

QStringList DeviceRuntime::linkDescriptions() const
{
    QStringList descriptions;
    if (transport_ == nullptr) {
        return descriptions;
    }

    invoke([this, &descriptions] {
        for (const transport::ILink* link : transport_->links()) {
            descriptions.append(QStringLiteral("#%1 %2").arg(link->id()).arg(link->peerDescription()));
        }
    });
    return descriptions;
}

Result<void> DeviceRuntime::sendRaw(const QByteArray& bytes)
{
    if (!running_) {
        return makeError(ErrorCode::NotReady, QStringLiteral("device is not running"), config_.name);
    }

    Result<void> result = makeError(ErrorCode::NotConnected,
                                    QStringLiteral("no open link"), config_.name);
    invoke([this, &bytes, &result] {
        if (sessions_.empty()) {
            return;
        }
        result = sessions_.begin()->second->sendRaw(bytes);
    });
    return result;
}

protocol::ProtocolSession::Counters DeviceRuntime::aggregateCounters() const
{
    protocol::ProtocolSession::Counters total = retiredCounters_;

    invoke([this, &total] {
        for (const auto& [id, session] : sessions_) {
            const auto counters = session->counters();
            total.framesDecoded += counters.framesDecoded;
            total.framesSent += counters.framesSent;
            total.decodeFailures += counters.decodeFailures;
            total.handlerFailures += counters.handlerFailures;
            total.resyncBytes += counters.resyncBytes;
            total.droppedByFilter += counters.droppedByFilter;
            total.requestTimeouts += counters.requestTimeouts;
        }
    });
    return total;
}

} // namespace hwsim::app
