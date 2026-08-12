# The SW6 plugin is built as a Qt module, which cannot be linked into a test
# executable, so its non-plugin translation units are compiled into the test
# targets instead. Kept here because both the unit and the integration test
# need the same list.
set(HWSIM_SW6_DIR "${CMAKE_SOURCE_DIR}/plugins/sw6")
set(HWSIM_SW6_SOURCES
    "${HWSIM_SW6_DIR}/Sw6Types.cpp"
    "${HWSIM_SW6_DIR}/Sw6Commands.cpp"
    "${HWSIM_SW6_DIR}/Sw6Messages.cpp"
    "${HWSIM_SW6_DIR}/Sw6DeviceState.cpp"
    "${HWSIM_SW6_DIR}/Sw6Handlers.cpp"
    "${HWSIM_SW6_DIR}/Sw6Codec.cpp")

add_subdirectory(unit)
add_subdirectory(integration)
