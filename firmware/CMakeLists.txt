cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CHATESP_PERMANENT_WRITE_POLICY OR
   NOT CHATESP_PERMANENT_WRITE_POLICY STREQUAL "FORBID")
    message(FATAL_ERROR
        "ChatESP requires CHATESP_PERMANENT_WRITE_POLICY=FORBID")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(chatesp)
