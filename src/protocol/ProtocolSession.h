#pragma once

#include "CommandRegistry.h"
#include "IByteFilter.h"
#include "IFrameCodec.h"
#include "IMiddleware.h"
#include "PendingRequestTable.h"
#include "ProtocolEvents.h"

#include <core/ByteBuffer.h>
#include <core/EventBus.h>
#include <transport/ILink.h>

#include <QObject>

#include <memory>

class QTimer;

namespace hwsim::protocol {

/// Binds one link to one protocol stack and runs the traffic through it.
///
/// Inbound:
///     bytes -> inbound filters -> buffer -> codec.scan -> registry.parse
///           -> middleware chain -> registry.dispatch -> handler
///
/// Outbound:
///     message -> registry.encodeBody -> codec.wrap -> outbound filters -> link
///
/// One session per link, so each peer gets its own reassembly buffer and its
/// own outstanding-request table. That is what keeps a TCP server with several
/// masters connected from interleaving their state.
///
/// The session is not thread safe by design: it lives on its transport's thread
/// together with the link and the device model, and nothing else touches it.
class HWSIM_PROTOCOL_API ProtocolSession : public QObject {
    Q_OBJECT

public:
    struct Options {
        QString name{QStringLiteral("session")};
        QString deviceName;
        transport::TransportRole role{transport::TransportRole::Responder};

        /// Used by sendRequest when no per-call timeout is given.
        int defaultTimeoutMs{1000};

        /// Reassembly buffer ceiling. Exceeding it means the peer is sending
        /// something this codec will never frame, so the buffer is dropped and
        /// the session resynchronises.
        std::size_t maxBufferBytes{64 * 1024};

        bool publishEvents{true};
    };

    struct Counters {
        quint64 framesDecoded{0};
        quint64 framesSent{0};
        quint64 decodeFailures{0};
        quint64 handlerFailures{0};
        quint64 resyncBytes{0};
        quint64 droppedByFilter{0};
        quint64 requestTimeouts{0};
    };

    ProtocolSession(transport::ILink* link, FrameCodecPtr codec,
                    std::shared_ptr<CommandRegistry> registry, Options options,
                    QObject* parent = nullptr);
    ~ProtocolSession() override;

    void setDevice(IDeviceAccess* device) { device_ = device; }
    [[nodiscard]] IDeviceAccess* device() const noexcept { return device_; }

    void setEventBus(core::EventBus* bus) { eventBus_ = bus; }
    [[nodiscard]] core::EventBus* eventBus() const noexcept { return eventBus_; }

    [[nodiscard]] MiddlewareChain& middleware() noexcept { return middleware_; }
    [[nodiscard]] ByteFilterChain& inboundFilters() noexcept { return inboundFilters_; }
    [[nodiscard]] ByteFilterChain& outboundFilters() noexcept { return outboundFilters_; }

    /// Initiator side. Encodes and sends `request`, then invokes `callback`
    /// with the matching response or a timeout error.
    core::Result<quint64> sendRequest(MessagePtr request, PendingRequestTable::Callback callback,
                                      int timeoutMs = -1);

    /// Sends a message without registering a correlation entry: notifications,
    /// unsolicited reports, or a reply produced outside the pipeline.
    core::Result<void> send(const MessagePtr& message, EncodeContext context = {});

    /// Writes bytes straight to the link, bypassing encoding but still passing
    /// through the outbound filters. Used by the manual send panel and by tests
    /// that need to inject a malformed frame.
    core::Result<void> sendRaw(QByteArray bytes);

    [[nodiscard]] const Options& options() const noexcept { return options_; }
    [[nodiscard]] transport::ILink* link() const noexcept { return link_; }
    [[nodiscard]] CommandRegistry* registry() const noexcept { return registry_.get(); }
    [[nodiscard]] IFrameCodec* codec() const noexcept { return codec_.get(); }

    [[nodiscard]] Counters counters() const noexcept { return counters_; }
    void resetCounters() { counters_ = Counters{}; }

    [[nodiscard]] std::size_t pendingRequestCount() const noexcept { return pending_.size(); }

signals:
    void frameProcessed(const hwsim::protocol::ProtocolFrameEvent& event);
    void errorOccurred(const hwsim::core::Error& error);

private:
    void onBytesReceived(transport::LinkId id, const QByteArray& data);
    void onLinkStateChanged(transport::LinkId id, transport::LinkState state);

    void ingest(const QByteArray& data);
    void drainBuffer();
    void processFrame(Frame frame);
    void runPipeline(const Frame& frame, const MessagePtr& request);

    core::Result<void> transmit(const MessagePtr& message, EncodeContext& context, int extraDelayMs);
    void deliverBytes(QByteArray bytes, Frame outbound, const MessagePtr& message, int extraDelayMs);
    void writeToLink(const QByteArray& bytes);

    void checkTimeouts();
    void updateTimeoutTimer();

    void publishFrame(const Frame& frame, bool decoded, QString description, QString annotation);
    void publishError(const core::Error& error, const QByteArray& raw);

    transport::ILink* link_{nullptr};
    FrameCodecPtr codec_;
    std::shared_ptr<CommandRegistry> registry_;
    Options options_;

    IDeviceAccess* device_{nullptr};
    core::EventBus* eventBus_{nullptr};

    MiddlewareChain middleware_;
    ByteFilterChain inboundFilters_;
    ByteFilterChain outboundFilters_;

    core::ByteBuffer buffer_;
    PendingRequestTable pending_;
    QTimer* timeoutTimer_{nullptr};

    Counters counters_;
};

} // namespace hwsim::protocol
