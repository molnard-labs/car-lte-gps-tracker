cmake_minimum_required(VERSION 3.16)

# Keep the diagnostic small enough for the original ESP32's internal IRAM/DRAM.
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(psram_ram_test)
