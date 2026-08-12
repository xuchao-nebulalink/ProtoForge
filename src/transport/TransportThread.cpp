#include "TransportThread.h"

#include <core/Logger.h>

#include <QMetaObject>
#include <QThread>

namespace {
constexpr auto kLogCategory = "transport.thread";
}

namespace hwsim::transport {

/// Lives on the worker thread and exists only to give queued invocations a
/// receiver with the right thread affinity.
class TransportThread::Executor : public QObject {
public:
    Executor() = default;
};

TransportThread::TransportThread(QString name, QObject* parent)
    : QObject(parent), name_(std::move(name))
{
}

TransportThread::~TransportThread()
{
    stop();
}

void TransportThread::start()
{
    if (thread_ != nullptr) {
        return;
    }

    thread_ = new QThread;
    thread_->setObjectName(name_);

    executor_ = new Executor;
    executor_->moveToThread(thread_);
    QObject::connect(thread_, &QThread::finished, executor_, &QObject::deleteLater);

    thread_->start();
    HWSIM_LOG_DEBUG(kLogCategory) << "started " << name_;
}

void TransportThread::stop()
{
    if (thread_ == nullptr) {
        return;
    }

    bool terminated = false;
    thread_->quit();
    if (!thread_->wait(5000)) {
        HWSIM_LOG_WARNING(kLogCategory) << name_ << " did not stop in time, terminating";
        thread_->terminate();
        thread_->wait();
        terminated = true;
    }

    if (terminated && executor_ != nullptr) {
        // On a clean quit the executor is destroyed by the deleteLater queued
        // on finished(). terminate() kills the thread without draining its
        // event queue, so that never runs and the executor has to go by hand.
        delete executor_;
    }
    executor_ = nullptr;

    delete thread_;
    thread_ = nullptr;

    HWSIM_LOG_DEBUG(kLogCategory) << "stopped " << name_;
}

bool TransportThread::isRunning() const
{
    return thread_ != nullptr && thread_->isRunning();
}

QThread* TransportThread::workerThread() const
{
    return thread_;
}

void TransportThread::adopt(QObject* object)
{
    if (object == nullptr || thread_ == nullptr) {
        return;
    }
    object->moveToThread(thread_);
}

void TransportThread::post(std::function<void()> work)
{
    if (executor_ == nullptr) {
        // Running inline keeps the caller working, but on the wrong thread for
        // anything thread-confined, so make the mistake visible rather than
        // letting it turn into an intermittent data race later.
        HWSIM_LOG_WARNING(kLogCategory)
            << name_ << " is not running; work will execute on the calling thread";
        work();
        return;
    }
    QMetaObject::invokeMethod(executor_, std::move(work), Qt::QueuedConnection);
}

void TransportThread::invokeBlocking(const std::function<void()>& work)
{
    // Calling from the worker itself is legitimate and must not deadlock on a
    // blocking queued connection to the thread we are already on.
    if (QThread::currentThread() == thread_) {
        work();
        return;
    }
    if (executor_ == nullptr) {
        HWSIM_LOG_WARNING(kLogCategory)
            << name_ << " is not running; work will execute on the calling thread";
        work();
        return;
    }
    QMetaObject::invokeMethod(executor_, work, Qt::BlockingQueuedConnection);
}

} // namespace hwsim::transport
