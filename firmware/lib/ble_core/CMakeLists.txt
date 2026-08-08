idf_component_register(
    SRCS
        "src/ble_settings.cpp"
        "src/indication_gate.cpp"
        "src/provisioning_session.cpp"
    INCLUDE_DIRS "include"
    REQUIRES provisioning_core
)
