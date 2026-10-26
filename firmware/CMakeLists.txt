cmake_minimum_required(VERSION 3.16)

# Resolve ESP-IDF built-ins from IDF_PATH, and only the intended local components
# from this project, preventing local folders from shadowing IDF built-ins.
set(COMPONENT_DIRS
    "$ENV{IDF_PATH}/components"
    "${CMAKE_CURRENT_SOURCE_DIR}/main"
    "${CMAKE_CURRENT_SOURCE_DIR}/components/tracker_core"
    "${CMAKE_CURRENT_SOURCE_DIR}/components/tracker_store"
    "${CMAKE_CURRENT_SOURCE_DIR}/components/tracker_net"
    "${CMAKE_CURRENT_SOURCE_DIR}/components/tracker_setup"
    "${CMAKE_CURRENT_SOURCE_DIR}/components/tracker_output"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(car-lte-gps-tracker)
