# QuickEIP
#
# Two uses:
#   1. ESP-IDF component: put this directory in EXTRA_COMPONENT_DIRS (or
#      reference it from idf_component.yml) and REQUIRES quickeip.
#   2. Host build (POSIX port) with tests and tools:
#        cmake -S . -B build && cmake --build build && ctest --test-dir build
#
# Other platforms (NetBurner, ClearCore) use their own build systems; see
# docs/INTEGRATION.md for the file lists.

set(QUICKEIP_CORE_SOURCES
    src/eip_codec.c
    src/eip_adapter.c
    src/eip_session.c
    src/eip_io.c
    src/eip_scan.c
    src/eip_server.c
    src/eip_adapter_default.c)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${QUICKEIP_CORE_SOURCES}
             ports/bsd_sockets/eip_hal_bsd_sockets.c
             ports/esp-idf/eip_hal_esp_idf.c
        INCLUDE_DIRS include ports/esp-idf
        PRIV_INCLUDE_DIRS src
        REQUIRES esp_netif esp_timer lwip freertos
        PRIV_REQUIRES esp_system)
    return()
endif()

cmake_minimum_required(VERSION 3.13)
project(quickeip VERSION 2.0.0 LANGUAGES C)

option(QUICKEIP_BUILD_TESTS "Build the host tests" ON)
option(QUICKEIP_BUILD_TOOLS "Build the command-line tools" ON)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Portable core only (bring your own eip_hal_* implementation).
add_library(quickeip_core STATIC ${QUICKEIP_CORE_SOURCES})
target_include_directories(quickeip_core PUBLIC include PRIVATE src)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(quickeip_core PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
endif()

# Core + POSIX port.
if(UNIX)
    find_package(Threads REQUIRED)
    # One archive: the core calls the port and the port calls the core.
    add_library(quickeip STATIC
        ${QUICKEIP_CORE_SOURCES}
        ports/bsd_sockets/eip_hal_bsd_sockets.c
        ports/posix/eip_hal_posix.c)
    target_link_libraries(quickeip PUBLIC Threads::Threads)
    target_include_directories(quickeip PUBLIC include PRIVATE src)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(quickeip PRIVATE -Wall -Wextra)
    endif()

    if(QUICKEIP_BUILD_TESTS)
        enable_testing()
        add_executable(test_core tests/test_core.c)
        target_include_directories(test_core PRIVATE src)
        target_link_libraries(test_core PRIVATE quickeip_core)
        add_test(NAME core COMMAND test_core)

        # Links the core only: the test supplies a scripted fake HAL.
        add_executable(test_session tests/test_session.c)
        target_include_directories(test_session PRIVATE src)
        target_link_libraries(test_session PRIVATE quickeip_core)
        add_test(NAME session COMMAND test_session)

        add_executable(test_loopback tests/test_loopback.c)
        target_link_libraries(test_loopback PRIVATE quickeip)
        add_test(NAME loopback COMMAND test_loopback)
        set_tests_properties(loopback PROPERTIES TIMEOUT 60)
    endif()

    if(QUICKEIP_BUILD_TOOLS)
        add_executable(eiptool examples/posix_eiptool/eiptool.c)
        target_link_libraries(eiptool PRIVATE quickeip)
    endif()
endif()
