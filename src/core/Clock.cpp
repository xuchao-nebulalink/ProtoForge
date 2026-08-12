#include "Clock.h"

#include <QDateTime>

namespace hwsim::core {
namespace {

class SystemClock final : public IClock {
public:
    [[nodiscard]] qint64 nowMs() const noexcept override { return monotonicMs(); }
};

} // namespace

qint64 monotonicMs() noexcept
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

qint64 monotonicUs() noexcept
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(now).count();
}

qint64 wallClockMs() noexcept
{
    return QDateTime::currentMSecsSinceEpoch();
}

QString formatWallClock(qint64 epochMs, bool withDate)
{
    const QDateTime moment = QDateTime::fromMSecsSinceEpoch(epochMs);
    return moment.toString(withDate ? QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")
                                    : QStringLiteral("HH:mm:ss.zzz"));
}

QString formatDuration(qint64 milliseconds)
{
    if (milliseconds < 1000) {
        return QStringLiteral("%1 ms").arg(milliseconds);
    }
    if (milliseconds < 60 * 1000) {
        return QStringLiteral("%1 s").arg(milliseconds / 1000.0, 0, 'f', 2);
    }
    const qint64 totalSeconds = milliseconds / 1000;
    return QStringLiteral("%1:%2:%3")
        .arg(totalSeconds / 3600, 2, 10, QLatin1Char('0'))
        .arg((totalSeconds % 3600) / 60, 2, 10, QLatin1Char('0'))
        .arg(totalSeconds % 60, 2, 10, QLatin1Char('0'));
}

IClock& systemClock()
{
    static SystemClock clock;
    return clock;
}

} // namespace hwsim::core
