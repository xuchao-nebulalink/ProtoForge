# Emitted from the list of plugins the build selected for static linkage. When
# every plugin is dynamic this is an empty registration function, so the code in
# main.cpp is identical either way.
hwsim_generate_static_plugin_imports(HWSIM_STATIC_PLUGIN_IMPORTS)

# Plain add_executable, not qt_add_executable: the latter installs deferred
# finalizers meant for Android, iOS and static Qt builds. On desktop with a
# shared Qt they add nothing here except a QML import scan this project has no
# use for, and that scanner cannot handle a non-ASCII build path.
add_executable(hwsim
    main.cpp
    CliOptions.h
    CliOptions.cpp
    DeviceRuntime.h
    DeviceRuntime.cpp
    Workspace.h
    Workspace.cpp
    ScriptHostAdapter.h
    ScriptHostAdapter.cpp
    AddDeviceDialog.h
    AddDeviceDialog.cpp
    MainWindow.h
    MainWindow.cpp
    ${HWSIM_STATIC_PLUGIN_IMPORTS}
)

target_include_directories(hwsim PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(hwsim PRIVATE
    hwsim::cpp20
    hwsim::core
    hwsim::transport
    hwsim::protocol
    hwsim::simulator
    hwsim::scripting
    hwsim::ui
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

hwsim_link_static_plugins(hwsim)

set_target_properties(hwsim PROPERTIES
    AUTOMOC ON
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    WIN32_EXECUTABLE OFF   # keep the console so --headless can report to stdout
    FOLDER "app"

    # Qt's bin directory is rarely on the system PATH, and when it is it often
    # points at a different Qt. Without this, F5 in Visual Studio fails on a
    # missing Qt6Core.dll before main() is reached.
    VS_DEBUGGER_ENVIRONMENT "PATH=$<TARGET_FILE_DIR:Qt6::Core>;%PATH%"

    # Start in the source tree so the relative paths in the README -- and in the
    # sample scenarios -- resolve. Plugins are found next to the executable, so
    # this does not disturb plugin discovery.
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

hwsim_set_target_warnings(hwsim)
