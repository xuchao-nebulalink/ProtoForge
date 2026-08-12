#include "ScriptEngine.h"

#include <core/Clock.h>
#include <core/Logger.h>

#include <QElapsedTimer>
#include <QFile>
#include <QJSEngine>
#include <QJSValue>

namespace {
constexpr auto kLogCategory = "script";
}

namespace hwsim::scripting {

QString ScriptEngine::Outcome::summary() const
{
    if (!completed) {
        return QStringLiteral("脚本执行中断: %1").arg(errorText);
    }
    if (passed) {
        return QStringLiteral("通过 (%1 项断言, %2 ms)").arg(checkCount).arg(durationMs);
    }
    return QStringLiteral("失败 (%1/%2 项断言未通过, %3 ms)")
        .arg(failures.size())
        .arg(checkCount)
        .arg(durationMs);
}

ScriptEngine::ScriptEngine(IScriptHost& host, QObject* parent)
    : QObject(parent), engine_(std::make_unique<QJSEngine>()),
      api_(std::make_unique<ScriptApi>(host))
{
    engine_->installExtensions(QJSEngine::ConsoleExtension);

    // QJSEngine does not take ownership of an object exposed this way, which is
    // what we want: the api outlives any single script run so results can be
    // inspected afterwards.
    engine_->globalObject().setProperty(QStringLiteral("hwsim"),
                                        engine_->newQObject(api_.get()));
    QJSEngine::setObjectOwnership(api_.get(), QJSEngine::CppOwnership);

    connect(api_.get(), &ScriptApi::messageLogged, this, &ScriptEngine::scriptMessage);
    connect(api_.get(), &ScriptApi::checkFailed, this, &ScriptEngine::scriptFailure);
}

ScriptEngine::~ScriptEngine() = default;

void ScriptEngine::exposeObject(const QString& name, QObject* object)
{
    if (object == nullptr) {
        return;
    }
    engine_->globalObject().setProperty(name, engine_->newQObject(object));
    QJSEngine::setObjectOwnership(object, QJSEngine::CppOwnership);
}

ScriptEngine::Outcome ScriptEngine::runSource(const QString& source, const QString& name)
{
    api_->resetResults();

    QElapsedTimer timer;
    timer.start();

    HWSIM_LOG_INFO(kLogCategory) << "running script " << name;
    const QJSValue result = engine_->evaluate(source, name);

    Outcome outcome;
    outcome.durationMs = timer.elapsed();
    outcome.checkCount = api_->checkCount();
    outcome.failures = api_->failures();

    if (result.isError()) {
        outcome.completed = false;
        outcome.passed = false;
        outcome.errorText = QStringLiteral("%1:%2: %3")
                                .arg(result.property(QStringLiteral("fileName")).toString())
                                .arg(result.property(QStringLiteral("lineNumber")).toInt())
                                .arg(result.toString());
        HWSIM_LOG_ERROR(kLogCategory) << outcome.errorText;
        return outcome;
    }

    outcome.completed = true;
    outcome.passed = api_->passed();

    HWSIM_LOG_INFO(kLogCategory) << name << ": " << outcome.summary();
    return outcome;
}

ScriptEngine::Outcome ScriptEngine::runFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        Outcome outcome;
        outcome.completed = false;
        outcome.errorText = QStringLiteral("%1: %2").arg(path, file.errorString());
        HWSIM_LOG_ERROR(kLogCategory) << outcome.errorText;
        return outcome;
    }

    return runSource(QString::fromUtf8(file.readAll()), path);
}

} // namespace hwsim::scripting
