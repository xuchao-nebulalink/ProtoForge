# Shared target helpers.
#
# Include convention across the whole tree: <CMAKE_SOURCE_DIR>/src is on every
# target's include path, so cross-module headers are addressed by module,
# e.g. #include <core/Result.h>, and same-directory headers use "Foo.h".

function(hwsim_add_library name)
    cmake_parse_arguments(ARG
        ""
        "EXPORT_MACRO"
        "SOURCES;PUBLIC_DEPS;PRIVATE_DEPS;DEFINES"
        ${ARGN})

    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "hwsim_add_library(${name}): SOURCES is required")
    endif()

    if(HWSIM_BUILD_SHARED)
        add_library(${name} SHARED ${ARG_SOURCES})
    else()
        add_library(${name} STATIC ${ARG_SOURCES})
    endif()

    # Targets are named hwsim_core, hwsim_transport, ... but are referred to as
    # hwsim::core, hwsim::transport, ... so the prefix has to be stripped here.
    string(REGEX REPLACE "^hwsim_" "" _alias "${name}")
    add_library(hwsim::${_alias} ALIAS ${name})

    if(ARG_EXPORT_MACRO)
        # Only set while compiling this target so consumers see dllimport.
        target_compile_definitions(${name} PRIVATE ${ARG_EXPORT_MACRO})
    endif()

    target_compile_definitions(${name} PUBLIC ${ARG_DEFINES})

    target_include_directories(${name}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
    )

    target_link_libraries(${name}
        PUBLIC  hwsim::cpp20 ${ARG_PUBLIC_DEPS}
        PRIVATE ${ARG_PRIVATE_DEPS}
    )

    set_target_properties(${name} PROPERTIES
        AUTOMOC ON
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        WINDOWS_EXPORT_ALL_SYMBOLS OFF
        FOLDER "framework"
    )

    hwsim_set_target_warnings(${name})
endfunction()


function(hwsim_add_test name)
    cmake_parse_arguments(ARG "" "LABEL" "SOURCES;DEPS" ${ARGN})

    add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE hwsim::cpp20 Qt6::Test ${ARG_DEPS})
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    set_target_properties(${name} PROPERTIES
        AUTOMOC ON
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "tests"
    )
    hwsim_set_target_warnings(${name})

    add_test(NAME ${name} COMMAND ${name})
    if(ARG_LABEL)
        set_tests_properties(${name} PROPERTIES LABELS "${ARG_LABEL}")
    endif()

    # Tests need both the framework DLLs and the Qt runtime on PATH.
    # ENVIRONMENT_MODIFICATION rather than ENVIRONMENT because the latter would
    # need the whole PATH inlined, and PATH's semicolons collide with CMake's
    # list separator on Windows.
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:${HWSIM_OUTPUT_DIR};PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endfunction()


function(hwsim_print_configuration_summary)
    get_property(_static_plugins GLOBAL PROPERTY HWSIM_STATIC_PLUGINS)
    get_property(_dynamic_plugins GLOBAL PROPERTY HWSIM_DYNAMIC_PLUGINS)

    if(HWSIM_BUILD_SHARED)
        set(_module_linkage "shared")
    else()
        set(_module_linkage "static")
    endif()

    message(STATUS "")
    message(STATUS "=== HwSimPlatform ${PROJECT_VERSION} ===")
    message(STATUS "  Qt version         : ${HWSIM_QT_VERSION}")
    message(STATUS "  Compiler           : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
    message(STATUS "  Module linkage     : ${_module_linkage}")
    message(STATUS "  Plugin default     : ${HWSIM_PLUGIN_LINKAGE}")
    message(STATUS "  Static plugins     : ${_static_plugins}")
    message(STATUS "  Dynamic plugins    : ${_dynamic_plugins}")
    message(STATUS "  std::format        : ${HWSIM_HAS_STD_FORMAT}")
    message(STATUS "  std::ranges        : ${HWSIM_HAS_RANGES}")
    message(STATUS "  Tests              : ${HWSIM_BUILD_TESTS}")
    message(STATUS "  Output directory   : ${HWSIM_OUTPUT_DIR}")
    message(STATUS "")
endfunction()
