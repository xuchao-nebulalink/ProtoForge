hwsim_add_test(tst_endtoend
    LABEL integration
    SOURCES tst_endtoend.cpp
    DEPS
        hwsim::core
        hwsim::transport
        hwsim::protocol
        hwsim::simulator)

hwsim_add_test(tst_scripting
    LABEL integration
    SOURCES tst_scripting.cpp
    DEPS
        hwsim::core
        hwsim::scripting)

hwsim_add_test(tst_sw6_endtoend
    LABEL integration
    SOURCES tst_sw6_endtoend.cpp ${HWSIM_SW6_SOURCES}
    DEPS
        hwsim::core
        hwsim::transport
        hwsim::protocol)
target_include_directories(tst_sw6_endtoend PRIVATE "${HWSIM_SW6_DIR}")

# Fixture location is compiled in rather than passed through the environment,
# so the tests can also be run straight from a debugger.
target_compile_definitions(tst_endtoend PRIVATE
    HWSIM_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../fixtures")
target_compile_definitions(tst_scripting PRIVATE
    HWSIM_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../fixtures")
