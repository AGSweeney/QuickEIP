# QuickEIP hardware test firmware for an ESP32-S31 board (internal EMAC, YT8531 RGMII PHY).
# Needs ESP-IDF 6.1 or later: earlier versions have no esp32s31 target. See ../README.md.
cmake_minimum_required(VERSION 3.16)

# The QuickEIP checkout is an IDF component (its root CMakeLists.txt).
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../..")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(quickeip_hwtest)
