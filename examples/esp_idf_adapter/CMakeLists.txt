# QuickEIP ESP-IDF example. Build: idf.py set-target esp32 && idf.py build
cmake_minimum_required(VERSION 3.16)

# The QuickEIP checkout is an IDF component (its root CMakeLists.txt).
# protocol_examples_common brings up Wi-Fi or Ethernet from menuconfig.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../.."
    "$ENV{IDF_PATH}/examples/common_components/protocol_examples_common")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(quickeip_adapter)
