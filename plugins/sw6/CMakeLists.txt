# NAME must match the identifier passed to HWSIM_EXPORT_STATIC_PLUGIN in
# Sw6Plugin.cpp: CMake derives the extern declaration from it for static builds.
#
# The sources below, minus Sw6Plugin.*, are also compiled into tst_sw6 and
# tst_sw6_endtoend; see tests/CMakeLists.txt for the list they share.
hwsim_add_protocol_plugin(
    NAME sw6
    CLASS_NAME Sw6Plugin
    SOURCES
        Sw6Types.h
        Sw6Types.cpp
        Sw6Commands.h
        Sw6Commands.cpp
        Sw6Messages.h
        Sw6Messages.cpp
        Sw6DeviceState.h
        Sw6DeviceState.cpp
        Sw6Handlers.h
        Sw6Handlers.cpp
        Sw6Codec.h
        Sw6Codec.cpp
        Sw6Plugin.h
        Sw6Plugin.cpp
        sw6.json
)
