set(MICROPYTHON_TOP
    "${CMAKE_CURRENT_LIST_DIR}/../../third_party/micropython")
if(CMAKE_SCRIPT_MODE_FILE)
    set(MICROPYTHON_WORK_ROOT
        "${CMAKE_CURRENT_LIST_DIR}/../../.pio/chatesp_micropython_requirements")
else()
    set(MICROPYTHON_WORK_ROOT "${CMAKE_CURRENT_BINARY_DIR}")
endif()
set(MICROPYTHON_GENERATED "${MICROPYTHON_WORK_ROOT}/micropython_embed")
set(MICROPYTHON_BUILD "${MICROPYTHON_WORK_ROOT}/micropython_build")

if(NOT EXISTS "${MICROPYTHON_TOP}/ports/embed/embed.mk")
    message(FATAL_ERROR
        "MicroPython submodule is absent. Run git submodule update --init.")
endif()

find_program(CHATESP_MAKE_EXECUTABLE NAMES gmake make REQUIRED)
execute_process(
    COMMAND
        "${CHATESP_MAKE_EXECUTABLE}"
        -f "${CMAKE_CURRENT_LIST_DIR}/micropython_embed.mk"
        "MICROPYTHON_TOP=${MICROPYTHON_TOP}"
        "BUILD=${MICROPYTHON_BUILD}"
        "PACKAGE_DIR=${MICROPYTHON_GENERATED}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
    RESULT_VARIABLE MICROPYTHON_GENERATE_RESULT
    OUTPUT_VARIABLE MICROPYTHON_GENERATE_OUTPUT
    ERROR_VARIABLE MICROPYTHON_GENERATE_ERROR
)
if(NOT MICROPYTHON_GENERATE_RESULT EQUAL 0)
    message(FATAL_ERROR
        "MicroPython embed generation failed:\n"
        "${MICROPYTHON_GENERATE_OUTPUT}\n${MICROPYTHON_GENERATE_ERROR}")
endif()

# The VM hook runs after bytecode jumps. Add the same bounded hook to iterator
# steps so C implementations such as sum(range(...)) cannot bypass the wall
# clock and operation limits.
set(MICROPYTHON_RUNTIME_SOURCE
    "${MICROPYTHON_GENERATED}/py/runtime.c")
file(READ "${MICROPYTHON_RUNTIME_SOURCE}" MICROPYTHON_RUNTIME_TEXT)
string(FIND "${MICROPYTHON_RUNTIME_TEXT}"
    "mp_obj_t mp_iternext(mp_obj_t o_in) {\n    chatesp_micropython_vm_hook();"
    MICROPYTHON_ITERATION_HOOK_OFFSET)
if(MICROPYTHON_ITERATION_HOOK_OFFSET EQUAL -1)
    set(MICROPYTHON_ITERATION_FUNCTION
        "mp_obj_t mp_iternext(mp_obj_t o_in) {\n")
    string(FIND "${MICROPYTHON_RUNTIME_TEXT}"
        "${MICROPYTHON_ITERATION_FUNCTION}"
        MICROPYTHON_ITERATION_FUNCTION_OFFSET)
    if(MICROPYTHON_ITERATION_FUNCTION_OFFSET EQUAL -1)
        message(FATAL_ERROR
            "The pinned MicroPython iterator function has changed.")
    endif()
    string(REPLACE
        "${MICROPYTHON_ITERATION_FUNCTION}"
        "${MICROPYTHON_ITERATION_FUNCTION}    chatesp_micropython_vm_hook();\n"
        MICROPYTHON_RUNTIME_TEXT "${MICROPYTHON_RUNTIME_TEXT}")
    file(WRITE "${MICROPYTHON_RUNTIME_SOURCE}"
        "${MICROPYTHON_RUNTIME_TEXT}")
endif()

file(GLOB_RECURSE MICROPYTHON_SOURCES
    "${MICROPYTHON_GENERATED}/*.c")
list(REMOVE_ITEM MICROPYTHON_SOURCES
    "${MICROPYTHON_GENERATED}/port/embed_util.c"
    "${MICROPYTHON_GENERATED}/port/mphalport.c")

idf_component_register(
    SRCS
        ${MICROPYTHON_SOURCES}
        "chatesp_micropython.c"
        "usermod/chatesp_plot/chatesp_plot_module.c"
    INCLUDE_DIRS
        "."
        "${MICROPYTHON_GENERATED}"
        "${MICROPYTHON_GENERATED}/port"
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function
    -Wno-unused-parameter
)
