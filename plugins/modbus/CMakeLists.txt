hwsim_add_protocol_plugin(
    NAME modbus
    CLASS_NAME ModbusPlugin
    SOURCES
        ModbusTypes.h
        ModbusTypes.cpp
        ModbusMessages.h
        ModbusMessages.cpp
        ModbusCodecs.h
        ModbusCodecs.cpp
        ModbusHandlers.h
        ModbusHandlers.cpp
        ModbusPlugin.h
        ModbusPlugin.cpp
        modbus.json
)
