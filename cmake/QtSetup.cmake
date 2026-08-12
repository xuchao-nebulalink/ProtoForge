# Locates Qt 6 and exposes the component set shared by every module.
# UI-only components are pulled in directly by src/ui and src/app.

set(HWSIM_QT_MIN_VERSION 6.5)

# Searches the usual install roots for a Qt 6 kit.
#
# Saves the caller from having to name the exact kit directory, which is easy to
# get wrong: the compiler suffix tracks the Qt version rather than the compiler
# actually in use (msvc2019_64 up to Qt 6.7, msvc2022_64 from 6.8), and the Qt
# maintenance tool installs next to any existing Qt, which is often not on C:.
function(hwsim_find_qt6_prefix out_var)
    set(_roots "")
    if(DEFINED ENV{QT6_DIR})
        list(APPEND _roots "$ENV{QT6_DIR}")
    endif()
    if(DEFINED ENV{QTDIR})
        list(APPEND _roots "$ENV{QTDIR}")
    endif()
    foreach(_drive IN ITEMS C D E)
        list(APPEND _roots "${_drive}:/Qt")
    endforeach()
    list(APPEND _roots "/opt/Qt" "$ENV{HOME}/Qt")

    set(_candidates "")
    foreach(_root IN LISTS _roots)
        if(NOT IS_DIRECTORY "${_root}")
            continue()
        endif()

        # The root may already be a kit directory.
        if(EXISTS "${_root}/lib/cmake/Qt6/Qt6Config.cmake")
            list(APPEND _candidates "${_root}")
        endif()

        file(GLOB _kits
             "${_root}/6.*/msvc*_64"
             "${_root}/6.*/mingw*_64"
             "${_root}/6.*/gcc_64"
             "${_root}/6.*/macos")
        foreach(_kit IN LISTS _kits)
            if(EXISTS "${_kit}/lib/cmake/Qt6/Qt6Config.cmake")
                list(APPEND _candidates "${_kit}")
            endif()
        endforeach()
    endforeach()

    if(NOT _candidates)
        set(${out_var} "" PARENT_SCOPE)
        return()
    endif()

    # NATURAL so that 6.10 sorts above 6.9 rather than below it.
    list(REMOVE_DUPLICATES _candidates)
    list(SORT _candidates COMPARE NATURAL)
    list(REVERSE _candidates)
    list(GET _candidates 0 _best)
    set(${out_var} "${_best}" PARENT_SCOPE)
endfunction()

# Test what CMAKE_PREFIX_PATH actually contains rather than whether it merely
# mentions Qt, so that a stale QT6_DIR pointing at a directory that does not
# exist falls through to detection instead of failing the configure.
set(HWSIM_PREFIX_HAS_QT6 FALSE)
foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
    if(EXISTS "${_prefix}/lib/cmake/Qt6/Qt6Config.cmake")
        set(HWSIM_PREFIX_HAS_QT6 TRUE)
    endif()
endforeach()

if(NOT Qt6_DIR AND NOT HWSIM_PREFIX_HAS_QT6)
    hwsim_find_qt6_prefix(HWSIM_DETECTED_QT_PREFIX)
    if(HWSIM_DETECTED_QT_PREFIX)
        message(STATUS "Qt 6 auto-detected at ${HWSIM_DETECTED_QT_PREFIX}")
        list(APPEND CMAKE_PREFIX_PATH "${HWSIM_DETECTED_QT_PREFIX}")
    else()
        message(FATAL_ERROR
            "No Qt 6 installation found.\n"
            "Searched QT6_DIR, QTDIR and C:/Qt, D:/Qt, E:/Qt for a 6.x kit.\n"
            "Point the build at your installation with either:\n"
            "    set QT6_DIR=D:/Qt/6.8.1/msvc2022_64\n"
            "or:\n"
            "    cmake --preset msvc2022 -DCMAKE_PREFIX_PATH=D:/Qt/6.8.1/msvc2022_64\n"
            "The directory you name must contain lib/cmake/Qt6/Qt6Config.cmake.")
    endif()
endif()

find_package(Qt6 ${HWSIM_QT_MIN_VERSION} REQUIRED COMPONENTS
    Core
    Network
    SerialPort
    Gui
    Widgets
    Qml            # QJSEngine lives here; used by the scripting module
)

if(HWSIM_BUILD_TESTS)
    find_package(Qt6 ${HWSIM_QT_MIN_VERSION} REQUIRED COMPONENTS Test)
endif()

qt_standard_project_setup()

# Qt's helper resets CMAKE_CXX_STANDARD to its own default, restore ours.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(HWSIM_QT_VERSION "${Qt6_VERSION}" CACHE INTERNAL "Detected Qt version")
