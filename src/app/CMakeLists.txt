# Emitted from the list of plugins the build selected for static linkage. When
# every plugin is dynamic this is an empty registration function, so the code in
# main.cpp is identical either way.
hwsim_generate_static_plugin_imports(HWSIM_STATIC_PLUGIN_IMPORTS)

qt_add_executable(hwsim
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
)

hwsim_set_target_warnings(hwsim)
