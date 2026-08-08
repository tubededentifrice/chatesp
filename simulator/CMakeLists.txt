cmake_minimum_required(VERSION 3.20)

project(chatesp_simulator VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/app_core/src/interaction_state.cpp")
    set(CHATESP_DEFAULT_APP_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/app_core")
else()
    set(CHATESP_DEFAULT_APP_CORE_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/../firmware/lib/app_core")
endif()

set(CHATESP_APP_CORE_DIR "${CHATESP_DEFAULT_APP_CORE_DIR}"
    CACHE PATH
    "Path to the portable ChatESP app_core library")
unset(CHATESP_DEFAULT_APP_CORE_DIR)

set(CHATESP_PROVISIONING_CORE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../firmware/lib/provisioning_core"
    CACHE PATH
    "Path to the portable ChatESP provisioning_core library")
set(CHATESP_BLE_CORE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../firmware/lib/ble_core"
    CACHE PATH
    "Path to the portable ChatESP ble_core library")

if(NOT EXISTS "${CHATESP_APP_CORE_DIR}/src/interaction_state.cpp")
    message(FATAL_ERROR
        "CHATESP_APP_CORE_DIR must point to the ChatESP app_core library")
endif()
if(NOT EXISTS
    "${CHATESP_PROVISIONING_CORE_DIR}/src/provisioning_packet.cpp")
    message(FATAL_ERROR
        "CHATESP_PROVISIONING_CORE_DIR must point to provisioning_core")
endif()
if(NOT EXISTS "${CHATESP_BLE_CORE_DIR}/src/provisioning_session.cpp")
    message(FATAL_ERROR
        "CHATESP_BLE_CORE_DIR must point to ble_core")
endif()

add_library(chatesp_sim_app_core STATIC
    "${CHATESP_APP_CORE_DIR}/src/interaction_state.cpp")
target_include_directories(chatesp_sim_app_core PUBLIC
    "${CHATESP_APP_CORE_DIR}/include")

add_library(chatesp_sim_provisioning_core STATIC
    "${CHATESP_PROVISIONING_CORE_DIR}/src/provisioning_packet.cpp"
    "${CHATESP_PROVISIONING_CORE_DIR}/src/provisioning_transfer.cpp")
target_include_directories(chatesp_sim_provisioning_core PUBLIC
    "${CHATESP_PROVISIONING_CORE_DIR}/include")

add_library(chatesp_sim_ble_core STATIC
    "${CHATESP_BLE_CORE_DIR}/src/ble_settings.cpp"
    "${CHATESP_BLE_CORE_DIR}/src/provisioning_session.cpp")
target_include_directories(chatesp_sim_ble_core PUBLIC
    "${CHATESP_BLE_CORE_DIR}/include")
target_link_libraries(chatesp_sim_ble_core PUBLIC
    chatesp_sim_provisioning_core)

add_library(chatesp_simulator_core STATIC
    src/ble_simulator.cpp
    src/simulator.cpp
    src/svg_renderer.cpp)
target_include_directories(chatesp_simulator_core PUBLIC include)
target_link_libraries(chatesp_simulator_core PUBLIC
    chatesp_sim_app_core
    chatesp_sim_ble_core)
target_compile_options(chatesp_simulator_core PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Werror>)

add_executable(chatesp-sim src/main.cpp)
target_link_libraries(chatesp-sim PRIVATE chatesp_simulator_core)
target_compile_options(chatesp-sim PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Werror>)

include(CTest)
if(BUILD_TESTING)
    add_executable(chatesp-sim-tests tests/test_simulator.cpp)
    target_link_libraries(chatesp-sim-tests PRIVATE chatesp_simulator_core)
    target_compile_options(chatesp-sim-tests PRIVATE
        $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Werror>)
    add_test(NAME chatesp-simulator COMMAND chatesp-sim-tests)
endif()
