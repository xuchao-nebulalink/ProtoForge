#include "IByteFilter.h"

#include <algorithm>

namespace hwsim::protocol {

void ByteFilterChain::add(ByteFilterPtr filter)
{
    if (!filter) {
        return;
    }
    remove(filter->name());
    filters_.push_back(std::move(filter));
}

void ByteFilterChain::remove(const QString& name)
{
    std::erase_if(filters_, [&name](const ByteFilterPtr& filter) { return filter->name() == name; });
}

void ByteFilterChain::clear()
{
    filters_.clear();
}

QStringList ByteFilterChain::names() const
{
    QStringList result;
    result.reserve(static_cast<qsizetype>(filters_.size()));
    for (const ByteFilterPtr& filter : filters_) {
        result.append(filter->name());
    }
    return result;
}

ByteFilterDecision ByteFilterChain::apply(QByteArray& bytes, const ByteFilterContext& context) const
{
    ByteFilterDecision merged;
    QStringList notes;

    for (const ByteFilterPtr& filter : filters_) {
        if (!filter->isEnabled()) {
            continue;
        }

        const ByteFilterDecision decision = filter->apply(bytes, context);
        if (!decision.note.isEmpty()) {
            notes.append(QStringLiteral("%1: %2").arg(filter->name(), decision.note));
        }
        merged.delayMs += decision.delayMs;

        if (!decision.deliver) {
            merged.deliver = false;
            merged.note = notes.join(QStringLiteral("; "));
            // A dropped buffer cannot be corrupted further; stop here so the
            // note reflects the rule that actually discarded it.
            return merged;
        }
    }

    merged.note = notes.join(QStringLiteral("; "));
    return merged;
}

} // namespace hwsim::protocol
