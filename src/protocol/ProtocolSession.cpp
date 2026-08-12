#include "ProtocolSession.h"

#include <core/Clock.h>
#include <core/HexUtils.h>
#include <core/Logger.h>

#include <QPointer>
#include <QTimer>

namespace {
constexpr auto kLogCategory = "protocol.session";
constexpr int kTimeoutTickMs = 25;
} // namespace

namespace hwsim::protocol {

ProtocolSession::ProtocolSession(transport::ILink* link, FrameCodecPtr codec,
                                 std::shared_ptr<CommandRegistry> registry, Options options,
                                 QObject* parent)
    : QObject(parent), link_(link), codec_(std::move(codec)), registry_(std::move(registry)),
      options_(std::move(options))
{
    Q_ASSERT(link_ != nullptr);
    Q_ASSERT(codec_ != nullptr);
    Q_ASSERT(registry_ != nullptr);

    buffer_.setCapacityLimit(options_.maxBufferBytes);

    connect(link_, &transport::ILink::bytesReceived, this, &ProtocolSession::onBytesReceived);
    connect(link_, &transport::ILink::stateChanged, this, &ProtocolSession::onLinkStateChanged);

    timeoutTimer_ = new QTimer(this);
    timeoutTimer_->setInterval(kTimeoutTickMs);
    connect(timeoutTimer_, &QTimer::timeout, this, &ProtocolSession::checkTimeouts);
}

ProtocolSession::~ProtocolSession()
{
    pending_.failAll(core::makeError(core::ErrorCode::Cancelled,
                                     QStringLiteral("session destroyed"), options_.name));
}

// --- Inbound ---------------------------------------------------------------

void ProtocolSession::onBytesReceived(transport::LinkId id, const QByteArray& data)
{
    Q_UNUSED(id)

    QByteArray payload = data;

    ByteFilterContext filterContext;
    filterContext.linkId = link_->id();
    filterContext.sessionName = options_.name;
    filterContext.direction = transport::Direction::Inbound;

    const ByteFilterDecision decision = inboundFilters_.apply(payload, filterContext);
    if (!decision.deliver) {
        ++counters_.droppedByFilter;
        HWSIM_LOG_DEBUG(kLogCategory) << options_.name << " dropped " << data.size()
                                      << " inbound bytes (" << decision.note << ')';
        return;
    }

    if (decision.delayMs > 0) {
        QPointer<ProtocolSession> self(this);
        QTimer::singleShot(decision.delayMs, this, [self, payload] {
            if (!self.isNull()) {
                self->ingest(payload);
            }
        });
        return;
    }

    ingest(payload);
}

void ProtocolSession::ingest(const QByteArray& data)
{
    buffer_.append(data);

    if (buffer_.isOverflowing()) {
        const auto discarded = buffer_.size();
        buffer_.clear();
        counters_.resyncBytes += discarded;
        publishError(core::makeError(core::ErrorCode::FrameTooLarge,
                                     QStringLiteral("reassembly buffer exceeded %1 bytes, resynchronising")
                                         .arg(options_.maxBufferBytes),
                                     options_.name),
                     {});
        return;
    }

    drainBuffer();
}

void ProtocolSession::drainBuffer()
{
    while (!buffer_.isEmpty()) {
        const FrameScanResult scan = codec_->scan(buffer_.readable(), transport::Direction::Inbound);

        if (scan.status == FrameScanStatus::NeedMoreData) {
            break;
        }

        if (scan.status == FrameScanStatus::Discard) {
            // Always consume at least one byte, otherwise a codec that reports
            // Discard with consumed == 0 would spin here forever.
            const std::size_t drop = scan.consumed > 0 ? scan.consumed : 1;
            const QByteArray junk = buffer_.take(drop);
            counters_.resyncBytes += static_cast<quint64>(junk.size());

            HWSIM_LOG_DEBUG(kLogCategory)
                << options_.name << " discarded " << junk.size() << " bytes: " << scan.diagnostic;

            if (options_.publishEvents && eventBus_ != nullptr) {
                ProtocolResyncEvent event;
                event.sessionName = options_.name;
                event.linkId = link_->id();
                event.discarded = junk;
                event.reason = scan.diagnostic;
                eventBus_->publish(event);
            }
            continue;
        }

        Frame frame = scan.frame;
        const QByteArray consumed = buffer_.take(scan.consumed > 0 ? scan.consumed
                                                                   : static_cast<std::size_t>(frame.raw.size()));
        if (frame.raw.isEmpty()) {
            frame.raw = consumed;
        }
        frame.linkId = link_->id();
        frame.direction = transport::Direction::Inbound;
        frame.timestampMs = core::wallClockMs();

        link_->noteFrameReceived();
        processFrame(std::move(frame));
    }
}

void ProtocolSession::processFrame(Frame frame)
{
    ++counters_.framesDecoded;

    const auto parsed = registry_->parse(frame);
    if (parsed.hasError()) {
        ++counters_.decodeFailures;
        publishFrame(frame, false, {}, parsed.error().toString());
        publishError(parsed.error(), frame.raw);
        return;
    }

    const MessagePtr message = parsed.value();
    publishFrame(frame, true, message->describe(), {});

    const bool handled = registry_->hasHandler(message->messageType());

    // A frame that nothing handles, or any frame at all while acting as an
    // initiator, is a candidate answer to something this session asked for.
    if (!handled || options_.role == transport::TransportRole::Initiator) {
        if (pending_.resolve(codec_->correlationKey(frame), message)) {
            updateTimeoutTimer();
            return;
        }
    }

    if (!handled) {
        const auto error = core::makeError(
            core::ErrorCode::UnknownCommand,
            QStringLiteral("no handler for %1").arg(message->describe()), options_.name);
        publishError(error, frame.raw);
        return;
    }

    runPipeline(frame, message);
}

void ProtocolSession::runPipeline(const Frame& frame, const MessagePtr& request)
{
    ExecutionContext execution(device_, frame);
    execution.setSessionName(options_.name);
    execution.setEventBus(eventBus_);

    PipelineContext pipeline{execution, frame, request, nullptr, false, {}, 0};

    const IMiddleware::Next terminal = [this](PipelineContext& context) -> core::Result<void> {
        auto dispatched = registry_->dispatch(*context.request, context.execution);
        if (dispatched.hasError()) {
            return dispatched.error();
        }
        context.response = std::move(dispatched).value();
        return core::success();
    };

    const auto outcome = middleware_.run(pipeline, terminal);
    if (outcome.hasError()) {
        ++counters_.handlerFailures;
        publishError(outcome.error(), frame.raw);
        return;
    }

    if (pipeline.suppressed) {
        HWSIM_LOG_DEBUG(kLogCategory)
            << options_.name << " suppressed reply: " << pipeline.suppressReason;
        return;
    }
    if (!pipeline.response) {
        return;
    }

    EncodeContext encodeContext = EncodeContext::forReply(frame);
    if (const auto sent = transmit(pipeline.response, encodeContext, pipeline.extraDelayMs);
        sent.hasError()) {
        publishError(sent.error(), {});
    }
}

// --- Outbound --------------------------------------------------------------

core::Result<quint64> ProtocolSession::sendRequest(MessagePtr request,
                                                   PendingRequestTable::Callback callback,
                                                   int timeoutMs)
{
    if (!request) {
        return core::makeError(core::ErrorCode::InvalidArgument,
                               QStringLiteral("request message is null"), options_.name);
    }

    EncodeContext context;
    context.linkId = link_->id();
    const QString correlationKey = codec_->prepareRequest(context);

    if (const auto sent = transmit(request, context, 0); sent.hasError()) {
        return sent.error();
    }

    const int effectiveTimeout = timeoutMs > 0 ? timeoutMs : options_.defaultTimeoutMs;
    const quint64 id = pending_.add(correlationKey, std::move(request), std::move(callback),
                                    effectiveTimeout, core::monotonicMs());
    updateTimeoutTimer();
    return id;
}

core::Result<void> ProtocolSession::send(const MessagePtr& message, EncodeContext context)
{
    if (!message) {
        return core::makeError(core::ErrorCode::InvalidArgument,
                               QStringLiteral("message is null"), options_.name);
    }
    if (context.linkId == transport::kInvalidLinkId) {
        context.linkId = link_->id();
    }
    return transmit(message, context, 0);
}

core::Result<void> ProtocolSession::sendRaw(QByteArray bytes)
{
    Frame outbound;
    outbound.raw = bytes;
    outbound.linkId = link_->id();
    outbound.direction = transport::Direction::Outbound;
    outbound.timestampMs = core::wallClockMs();

    deliverBytes(std::move(bytes), std::move(outbound), nullptr, 0);
    return core::success();
}

core::Result<void> ProtocolSession::transmit(const MessagePtr& message, EncodeContext& context,
                                             int extraDelayMs)
{
    // A message may carry its own opcode when the wire value depends on its
    // content; otherwise the registry supplies the one it was bound with.
    OpCode opcode = 0;
    if (const auto dynamic = message->dynamicOpcode(); dynamic.has_value()) {
        opcode = *dynamic;
    } else {
        const auto registered = registry_->opcodeFor(message->messageType());
        if (registered.hasError()) {
            return registered.error();
        }
        opcode = registered.value();
    }

    const auto body = registry_->encodeBody(*message, context);
    if (body.hasError()) {
        return body.error();
    }

    const auto wrapped = codec_->wrap(opcode, body.value(), context);
    if (wrapped.hasError()) {
        return wrapped.error();
    }

    Frame outbound;
    outbound.opcode = opcode;
    outbound.payload = body.value();
    outbound.attributes = context.attributes;
    outbound.linkId = link_->id();
    outbound.direction = transport::Direction::Outbound;
    outbound.timestampMs = core::wallClockMs();

    deliverBytes(wrapped.value(), std::move(outbound), message, extraDelayMs);
    return core::success();
}

void ProtocolSession::deliverBytes(QByteArray bytes, Frame outbound, const MessagePtr& message,
                                   int extraDelayMs)
{
    ByteFilterContext filterContext;
    filterContext.linkId = link_->id();
    filterContext.sessionName = options_.name;
    filterContext.direction = transport::Direction::Outbound;
    filterContext.frame = &outbound;

    const ByteFilterDecision decision = outboundFilters_.apply(bytes, filterContext);

    // Report what actually goes on the wire, corruption included, so the packet
    // view shows the injected fault rather than the pristine encoding.
    outbound.raw = bytes;
    publishFrame(outbound, message != nullptr, message ? message->describe() : QString{},
                 decision.note);

    if (!decision.deliver) {
        ++counters_.droppedByFilter;
        HWSIM_LOG_DEBUG(kLogCategory)
            << options_.name << " dropped outbound frame (" << decision.note << ')';
        return;
    }

    const int delay = decision.delayMs + extraDelayMs;
    if (delay > 0) {
        QPointer<ProtocolSession> self(this);
        QTimer::singleShot(delay, this, [self, bytes] {
            if (!self.isNull()) {
                self->writeToLink(bytes);
            }
        });
        return;
    }

    writeToLink(bytes);
}

void ProtocolSession::writeToLink(const QByteArray& bytes)
{
    if (link_ == nullptr || !link_->isOpen()) {
        publishError(core::makeError(core::ErrorCode::NotConnected,
                                     QStringLiteral("link is closed"), options_.name),
                     bytes);
        return;
    }

    if (link_->send(bytes) >= 0) {
        link_->noteFrameSent();
        ++counters_.framesSent;
    }
}

// --- Lifecycle and timeouts ------------------------------------------------

void ProtocolSession::onLinkStateChanged(transport::LinkId id, transport::LinkState state)
{
    Q_UNUSED(id)
    if (state == transport::LinkState::Connected) {
        return;
    }

    buffer_.clear();
    pending_.failAll(core::makeError(core::ErrorCode::NotConnected,
                                     QStringLiteral("link entered state %1").arg(
                                         transport::linkStateName(state)),
                                     options_.name));
    updateTimeoutTimer();
}

void ProtocolSession::checkTimeouts()
{
    const auto expired = pending_.takeExpired(core::monotonicMs());
    for (const auto& entry : expired) {
        ++counters_.requestTimeouts;

        HWSIM_LOG_DEBUG(kLogCategory)
            << options_.name << " request timed out after " << entry.timeoutMs << " ms";

        if (options_.publishEvents && eventBus_ != nullptr) {
            RequestTimedOutEvent event;
            event.sessionName = options_.name;
            event.linkId = link_->id();
            event.requestDescription = entry.request ? entry.request->describe() : QString{};
            event.timeoutMs = entry.timeoutMs;
            eventBus_->publish(event);
        }
    }
    updateTimeoutTimer();
}

void ProtocolSession::updateTimeoutTimer()
{
    if (timeoutTimer_ == nullptr) {
        return;
    }
    if (pending_.isEmpty()) {
        timeoutTimer_->stop();
    } else if (!timeoutTimer_->isActive()) {
        timeoutTimer_->start();
    }
}

// --- Reporting -------------------------------------------------------------

void ProtocolSession::publishFrame(const Frame& frame, bool decoded, QString description,
                                   QString annotation)
{
    ProtocolFrameEvent event;
    event.sessionName = options_.name;
    event.deviceName = options_.deviceName;
    event.linkId = frame.linkId;
    event.direction = frame.direction;
    event.opcode = frame.opcode;
    event.raw = frame.raw;
    event.messageDescription = std::move(description);
    event.decoded = decoded;
    event.annotation = std::move(annotation);
    event.timestampMs = frame.timestampMs != 0 ? frame.timestampMs : core::wallClockMs();

    emit frameProcessed(event);

    if (options_.publishEvents && eventBus_ != nullptr) {
        eventBus_->publish(event);
    }
}

void ProtocolSession::publishError(const core::Error& error, const QByteArray& raw)
{
    HWSIM_LOG_WARNING(kLogCategory) << options_.name << ": " << error.toString();

    emit errorOccurred(error);

    if (options_.publishEvents && eventBus_ != nullptr) {
        ProtocolErrorEvent event;
        event.sessionName = options_.name;
        event.linkId = link_ != nullptr ? link_->id() : transport::kInvalidLinkId;
        event.error = error;
        event.raw = raw;
        eventBus_->publish(event);
    }
}

} // namespace hwsim::protocol
