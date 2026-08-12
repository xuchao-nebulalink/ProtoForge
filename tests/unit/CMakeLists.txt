hwsim_add_test(tst_core
    LABEL unit
    SOURCES tst_core.cpp
    DEPS hwsim::core)

hwsim_add_test(tst_transport
    LABEL unit
    SOURCES tst_transport.cpp
    DEPS hwsim::core hwsim::transport)

hwsim_add_test(tst_protocol
    LABEL unit
    SOURCES tst_protocol.cpp
    DEPS hwsim::core hwsim::protocol)

hwsim_add_test(tst_simulator
    LABEL unit
    SOURCES tst_simulator.cpp
    DEPS hwsim::core hwsim::simulator)

hwsim_add_test(tst_sw6
    LABEL unit
    SOURCES tst_sw6.cpp ${HWSIM_SW6_SOURCES}
    DEPS hwsim::core hwsim::protocol)
target_include_directories(tst_sw6 PRIVATE "${HWSIM_SW6_DIR}")
