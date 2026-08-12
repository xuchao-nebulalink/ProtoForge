#pragma once

#include "TransportGlobal.h"

#include <QObject>
#include <QString>

#include <functional>
#include <memory>

class QThread;

namespace hwsim::transport {

/// A worker thread with a running event loop, plus the plumbing to get work
/// onto it.
///
/// The threading rule for the whole platform is: one device runs one transport,
/// one protocol session and one device model, all on the same thread. Nothing
/// in that pipeline needs a lock, because nothing is shared across threads. The
/// UI never touches those objects; it observes value-typed events instead.
class HWSIM_TRANSPORT_API TransportThread : public QObject {
    Q_OBJECT

public:
    explicit TransportThread(QString name, QObject* parent = nullptr);
    ~TransportThread() override;

    void start();

    /// Stops the event loop and joins. Objects adopted by this thread must be
    /// destroyed before or during stop().
    void stop();

    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] QThread* workerThread() const;
    [[nodiscard]] QString name() const { return name_; }

    /// Moves an object (and its children) onto the worker thread.
    void adopt(QObject* object);

    /// Runs `work` on the worker thread and returns immediately.
    void post(std::function<void()> work);

    /// Runs `work` on the worker thread and waits for it. Executes inline when
    /// called from the worker thread itself, so it cannot self-deadlock.
    void invokeBlocking(const std::function<void()>& work);

private:
    class Executor;

    QString name_;
    QThread* thread_{nullptr};
    Executor* executor_{nullptr};
};

} // namespace hwsim::transport
