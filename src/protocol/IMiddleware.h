#pragma once

#include "ExecutionContext.h"
#include "IMessage.h"

#include <functional>
#include <memory>
#include <vector>

namespace hwsim::protocol {

/// State carried along one request/response cycle through the middleware chain.
struct HWSIM_PROTOCOL_API PipelineContext {
    ExecutionContext& execution;
    const Frame& frame;

    /// The decoded request. Middleware may replace it before it reaches the handler.
    MessagePtr request;

    /// Filled in by the dispatch stage. Middleware may inspect, replace or clear
    /// it on the way back out.
    MessagePtr response;

    /// Set by middleware that decides this request should not be answered.
    bool suppressed{false};
    QString suppressReason;

    /// Extra delay to apply before the reply goes out, accumulated by middleware.
    int extraDelayMs{0};
};

/// A stage in the request pipeline.
///
/// Cross-cutting behaviour lives here rather than in handlers: tracing,
/// statistics, device-state gating, rate limiting. Each stage decides whether
/// to call `next`, so a stage can short-circuit the whole chain, which is how a
/// device in a fault state stops replying without any handler knowing about it.
class HWSIM_PROTOCOL_API IMiddleware {
public:
    using Next = std::function<core::Result<void>(PipelineContext&)>;

    virtual ~IMiddleware() = default;

    [[nodiscard]] virtual QString name() const = 0;

    /// Lower runs closer to the transport, higher closer to the handler.
    [[nodiscard]] virtual int priority() const { return 100; }

    [[nodiscard]] virtual core::Result<void> process(PipelineContext& context, const Next& next) = 0;
};

using MiddlewarePtr = std::shared_ptr<IMiddleware>;

/// Ordered middleware chain, folded into a single callable.
class HWSIM_PROTOCOL_API MiddlewareChain {
public:
    void add(MiddlewarePtr middleware);
    void remove(const QString& name);
    void clear();

    [[nodiscard]] bool isEmpty() const noexcept { return stages_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return stages_.size(); }
    [[nodiscard]] QStringList names() const;

    /// Runs the chain, ending with `terminal` (normally the registry dispatch).
    [[nodiscard]] core::Result<void> run(PipelineContext& context,
                                         const IMiddleware::Next& terminal) const;

private:
    void sortStages();

    std::vector<MiddlewarePtr> stages_;
};

// --- Built-in stages -------------------------------------------------------

/// Writes one log line per request at Debug level, including the handler's
/// verdict. Priority puts it outermost so it also sees suppressed requests.
class HWSIM_PROTOCOL_API TracingMiddleware final : public IMiddleware {
public:
    explicit TracingMiddleware(QString category = QStringLiteral("protocol.trace"));

    [[nodiscard]] QString name() const override { return QStringLiteral("tracing"); }
    [[nodiscard]] int priority() const override { return 10; }
    [[nodiscard]] core::Result<void> process(PipelineContext& context, const Next& next) override;

private:
    QString category_;
};

/// Counts requests, replies and failures, and tracks handler latency.
class HWSIM_PROTOCOL_API StatisticsMiddleware final : public IMiddleware {
public:
    struct Counters {
        quint64 requests{0};
        quint64 replies{0};
        quint64 suppressed{0};
        quint64 failures{0};
        qint64 totalHandlerMicros{0};
        qint64 slowestHandlerMicros{0};

        [[nodiscard]] double averageHandlerMicros() const
        {
            return requests == 0 ? 0.0 : static_cast<double>(totalHandlerMicros)
                                             / static_cast<double>(requests);
        }
    };

    [[nodiscard]] QString name() const override { return QStringLiteral("statistics"); }
    [[nodiscard]] int priority() const override { return 20; }
    [[nodiscard]] core::Result<void> process(PipelineContext& context, const Next& next) override;

    [[nodiscard]] Counters counters() const { return counters_; }
    void reset() { counters_ = Counters{}; }

private:
    Counters counters_;
};

/// Drops requests while the device model reports itself unresponsive, which is
/// how a state machine in a fault state produces a silent device.
class HWSIM_PROTOCOL_API DeviceStateGateMiddleware final : public IMiddleware {
public:
    [[nodiscard]] QString name() const override { return QStringLiteral("device-state-gate"); }
    [[nodiscard]] int priority() const override { return 50; }
    [[nodiscard]] core::Result<void> process(PipelineContext& context, const Next& next) override;
};

} // namespace hwsim::protocol
