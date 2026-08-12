#pragma once

#include "ScriptApi.h"

#include <QObject>
#include <QString>

#include <memory>

class QJSEngine;

namespace hwsim::scripting {

/// Runs scenario scripts.
///
/// Built on QJSEngine rather than an embedded Lua or Python: it ships with Qt,
/// so the platform gains scripting without a third-party dependency or an extra
/// runtime to deploy alongside the executable, and QVariant conversion to and
/// from JavaScript is already handled.
///
/// The script sees one global object, `hwsim`, backed by ScriptApi.
class HWSIM_SCRIPTING_API ScriptEngine : public QObject {
    Q_OBJECT

public:
    struct Outcome {
        bool completed{false};
        bool passed{false};
        int checkCount{0};
        QStringList failures;
        QString errorText;
        qint64 durationMs{0};

        [[nodiscard]] QString summary() const;
    };

    explicit ScriptEngine(IScriptHost& host, QObject* parent = nullptr);
    ~ScriptEngine() override;

    [[nodiscard]] Outcome runSource(const QString& source, const QString& name = QStringLiteral("<inline>"));
    [[nodiscard]] Outcome runFile(const QString& path);

    [[nodiscard]] ScriptApi* api() const noexcept { return api_.get(); }

    /// Adds another object to the script's global scope, for tests that need a
    /// bespoke helper.
    void exposeObject(const QString& name, QObject* object);

signals:
    void scriptMessage(const QString& message);
    void scriptFailure(const QString& message);

private:
    std::unique_ptr<QJSEngine> engine_;
    std::unique_ptr<ScriptApi> api_;
};

} // namespace hwsim::scripting
