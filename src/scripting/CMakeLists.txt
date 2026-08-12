hwsim_add_library(hwsim_scripting
    EXPORT_MACRO HWSIM_SCRIPTING_BUILD
    SOURCES
        ScriptingGlobal.h
        IScriptHost.h
        ScriptApi.h
        ScriptApi.cpp
        ScriptEngine.h
        ScriptEngine.cpp
    PUBLIC_DEPS
        hwsim::core
        Qt6::Core
    PRIVATE_DEPS
        # QJSEngine is an implementation detail: ScriptEngine.h only
        # forward-declares it behind a unique_ptr. Keeping this private also
        # stops Qt's QML import scanner from running over the application,
        # which it otherwise does for anything that links Qml, even though this
        # project never loads a .qml file.
        Qt6::Qml
)
