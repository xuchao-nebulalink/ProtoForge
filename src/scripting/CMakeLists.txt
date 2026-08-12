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
        Qt6::Qml
)
