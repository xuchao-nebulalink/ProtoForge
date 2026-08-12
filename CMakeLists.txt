cmake_minimum_required(VERSION 3.22)

project(HwSimPlatform
    VERSION 0.1.0
    DESCRIPTION "Device protocol simulation platform"
    LANGUAGES CXX
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(HWSIM_BUILD_TESTS      "Build unit and integration tests"          ON)
option(HWSIM_BUILD_SHARED     "Build framework modules as shared objects" ON)
option(HWSIM_WARNINGS_AS_ERRORS "Treat compiler warnings as errors"       OFF)

set(HWSIM_PLUGIN_LINKAGE "DYNAMIC" CACHE STRING
    "Default linkage for protocol plugins: DYNAMIC (QPluginLoader) or STATIC (linked into the app)")
set_property(CACHE HWSIM_PLUGIN_LINKAGE PROPERTY STRINGS DYNAMIC STATIC)

# Everything lands in <build>/bin so that the app finds bin/plugins at runtime
# without per-configuration path juggling on multi-config generators.
set(HWSIM_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin")
set(HWSIM_PLUGIN_OUTPUT_DIR "${HWSIM_OUTPUT_DIR}/plugins")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${HWSIM_OUTPUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${HWSIM_OUTPUT_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
foreach(_cfg IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_cfg} "${HWSIM_OUTPUT_DIR}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${_cfg} "${HWSIM_OUTPUT_DIR}")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/lib")
endforeach()

include(CompilerWarnings)
include(Cpp20Features)
include(QtSetup)
include(HwSimHelpers)
include(AddProtocolPlugin)

# After QtSetup, because qt_standard_project_setup() turns AUTOUIC back on.
# The UI is written in C++ rather than Designer forms: the main panels are
# generated at run time from ConfigSchema, which a .ui file cannot express.
set(CMAKE_AUTOUIC OFF)

if(NOT HWSIM_BUILD_SHARED)
    add_compile_definitions(HWSIM_STATIC_BUILD)
endif()

add_subdirectory(src/core)
add_subdirectory(src/transport)
add_subdirectory(src/protocol)
add_subdirectory(src/simulator)
add_subdirectory(src/scripting)
add_subdirectory(src/ui)
add_subdirectory(plugins)
add_subdirectory(src/app)

# Opening the generated solution should land on something that can actually be
# started; Visual Studio otherwise selects ALL_BUILD, which cannot be run.
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT hwsim)

if(HWSIM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

hwsim_print_configuration_summary()
