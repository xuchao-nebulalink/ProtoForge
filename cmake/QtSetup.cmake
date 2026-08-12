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

        # Only consider kits built for the compiler actually in use. A machine
        # can easily have msvc, mingw, arm64 and wasm kits side by side, and
        # picking one by name order would hand the Visual Studio generator a
        # MinGW kit and fail much later with a confusing error.
        if(MSVC)
            set(_kit_globs "${_root}/6.*/msvc*_64")
        elseif(MINGW)
            set(_kit_globs "${_root}/6.*/mingw*_64" "${_root}/6.*/llvm-mingw*_64")
        elseif(APPLE)
            set(_kit_globs "${_root}/6.*/macos" "${_root}/6.*/clang_64")
        else()
            set(_kit_globs "${_root}/6.*/gcc_64")
        endif()

        set(_root_kits "")
        file(GLOB _kits ${_kit_globs})
        foreach(_kit IN LISTS _kits)
            if(EXISTS "${_kit}/lib/cmake/Qt6/Qt6Config.cmake")
                list(APPEND _root_kits "${_kit}")
            endif()
        endforeach()

        # Sort within a root, never across roots. Sorting the full paths would
        # compare the drive letter before the version, so a 6.5 on D: would beat
        # a 6.9 on C:. Roots are already in priority order, with QT6_DIR first.
        # NATURAL so that 6.10 ranks above 6.9 rather than below it.
        if(_root_kits)
            list(SORT _root_kits COMPARE NATURAL)
            list(REVERSE _root_kits)
            list(APPEND _candidates ${_root_kits})
        endif()
    endforeach()

    if(NOT _candidates)
        set(${out_var} "" PARENT_SCOPE)
        return()
    endif()

    list(REMOVE_DUPLICATES _candidates)
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
        # Detection only knows the layout the official Qt installer produces.
        # A distribution package, Qt6_ROOT, or a prefix supplied through the
        # environment can all work without matching it, so give find_package a
        # chance before declaring defeat.
        find_package(Qt6 ${HWSIM_QT_MIN_VERSION} QUIET COMPONENTS Core)
    endif()

    if(NOT HWSIM_DETECTED_QT_PREFIX AND NOT Qt6_FOUND)
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

find_package(Qt6 ${HWSIM_QT_MIN_VERSION} REQUIRED COMPONENTS Core)

# Serial Port ships as an add-on and is NOT selected by default in the Qt
# installer, which makes it the most common thing missing from an otherwise
# complete installation. Check it separately so the failure names the fix
# instead of leaving find_package's generic "could not find Qt6SerialPort".
find_package(Qt6 ${HWSIM_QT_MIN_VERSION} QUIET COMPONENTS SerialPort)
if(NOT Qt6SerialPort_FOUND)
    get_target_property(_qt_core_location Qt6::Core LOCATION)
    message(FATAL_ERROR
        "Qt 6 was found (${Qt6_VERSION}) but the SerialPort module is missing.\n"
        "It is an optional add-on and is not ticked by default in the installer.\n"
        "Add it with the Qt Maintenance Tool:\n"
        "    Qt  ->  Qt ${Qt6_VERSION}  ->  Additional Libraries  ->  Qt Serial Port\n"
        "Qt was located at: ${_qt_core_location}")
endif()

find_package(Qt6 ${HWSIM_QT_MIN_VERSION} REQUIRED COMPONENTS
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
