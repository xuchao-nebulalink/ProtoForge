#include "IMiddleware.h"

#include <core/Clock.h>
#include <core/Logger.h>

#include <algorithm>

namespace hwsim::protocol {

// --- MiddlewareChain -------------------------------------------------------

void MiddlewareChain::add(MiddlewarePtr middleware)
{
    if (!middleware) {
        return;
    }
    remove(middleware->name());
    stages_.push_back(std::move(middleware));
    sortStages();
}

void MiddlewareChain::remove(const QString& name)
{
    std::erase_if(stages_, [&name](const MiddlewarePtr& stage) { return stage->name() == name; });
}

void MiddlewareChain::clear()
{
    stages_.clear();
}

QStringList MiddlewareChain::names() const
{
    QStringList result;
    result.reserve(static_cast<qsizetype>(stages_.size()));
    for (const MiddlewarePtr& stage : stages_) {
        result.append(stage->name());
    }
    return result;
}

void MiddlewareChain::sortStages()
{
    std::stable_sort(stages_.begin(), stages_.end(),
                     [](const MiddlewarePtr& lhs, const MiddlewarePtr& rhs) {
                         return lhs->priority() < rhs->priority();
                     });
}

core::Result<void> MiddlewareChain::run(PipelineContext& context,
                                        const IMiddleware::Next& terminal) const
{
    if (stages_.empty()) {
        return terminal(context);
    }

    // Fold the chain from the inside out so each stage receives a `next` that
    // already has the rest of the chain baked into it.
    IMiddleware::Next next = terminal;
    for (auto it = stages_.rbegin(); it != stages_.rend(); ++it) {
        next = [stage = *it, downstream = std::move(next)](PipelineContext& ctx) {
            return stage->process(ctx, downstream);
        };
    }
    return next(context);
}

// --- TracingMiddleware -----------------------------------------------------

TracingMiddleware::TracingMiddleware(QString category) : category_(std::move(category)) {}

core::Result<void> TracingMiddleware::process(PipelineContext& context, const Next& next)
{
    const QString requestText = context.request ? context.request->describe()
                                                : QStringLiteral("<undecoded>");

    const auto result = next(context);

    if (result.hasError()) {
        HWSIM_LOG_WARNING(category_)
            << context.execution.sessionName() << " | " << requestText
            << " -> error " << result.error().toString();
    } else if (context.suppressed) {
        HWSIM_LOG_DEBUG(category_)
            << context.execution.sessionName() << " | " << requestText
            << " -> suppressed (" << context.suppressReason << ')';
    } else {
        HWSIM_LOG_DEBUG(category_)
            << context.execution.sessionName() << " | " << requestText << " -> "
            << (context.response ? context.response->describe() : QStringLiteral("<no reply>"));
    }

    return result;
}

// --- StatisticsMiddleware --------------------------------------------------

core::Result<void> StatisticsMiddleware::process(PipelineContext& context, const Next& next)
{
    ++counters_.requests;
    const qint64 startedAt = core::monotonicUs();

    const auto result = next(context);

    const qint64 elapsed = core::monotonicUs() - startedAt;
    counters_.totalHandlerMicros += elapsed;
    counters_.slowestHandlerMicros = qMax(counters_.slowestHandlerMicros, elapsed);

    if (result.hasError()) {
        ++counters_.failures;
    } else if (context.suppressed) {
        ++counters_.suppressed;
    } else if (context.response) {
        ++counters_.replies;
    }

    return result;
}

// --- DeviceStateGateMiddleware ---------------------------------------------

core::Result<void> DeviceStateGateMiddleware::process(PipelineContext& context, const Next& next)
{
    IDeviceAccess* device = context.execution.device();
    if (device != nullptr && !device->isResponsive()) {
        context.suppressed = true;
        context.suppressReason =
            QStringLiteral("device is in state '%1'").arg(device->currentState());
        return core::success();
    }
    return next(context);
}

} // namespace hwsim::protocol
