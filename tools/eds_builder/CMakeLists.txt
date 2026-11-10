cmake_minimum_required(VERSION 3.21)
project(eds_builder VERSION 0.1 LANGUAGES CXX)

# Qt 6 with the Widgets module. Point CMake at the kit, for example:
#   cmake -S . -B build -DCMAKE_PREFIX_PATH=C:/Qt/6.10.0/msvc2022_64
find_package(Qt6 REQUIRED COMPONENTS Core Widgets)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

add_library(eds_core STATIC
    src/edsmodel.cpp
    src/edsmodel.h
    src/edsvalidator.cpp
    src/edsvalidator.h
    src/edswriter.cpp
    src/edswriter.h
    src/projectscanner.cpp
    src/projectscanner.h
)
target_include_directories(eds_core PUBLIC src)
target_link_libraries(eds_core PUBLIC Qt6::Core)

add_executable(eds_builder WIN32
    src/main.cpp
    src/mainwindow.cpp
    src/mainwindow.h
    src/theme.cpp
    src/theme.h
    res/eds_builder.qrc     # the icon for the window title bar
    res/eds_builder.rc      # the icon of the .exe itself
)
target_link_libraries(eds_builder PRIVATE eds_core Qt6::Widgets)

add_executable(eds_builder_cli src/cli_main.cpp res/eds_builder.rc)
target_link_libraries(eds_builder_cli PRIVATE eds_core)

if(MSVC)
    target_compile_options(eds_core PRIVATE /W4 /utf-8)
    target_compile_options(eds_builder PRIVATE /W4 /utf-8)
    target_compile_options(eds_builder_cli PRIVATE /W4 /utf-8)
endif()

# Copy the Qt DLLs and plugins next to the executables so they run from the
# build folder and the folder can be zipped and handed out.
if(WIN32)
    get_target_property(_qmake Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin "${_qmake}" DIRECTORY)
    find_program(WINDEPLOYQT windeployqt HINTS "${_qt_bin}")
    if(WINDEPLOYQT)
        add_custom_command(TARGET eds_builder POST_BUILD
            COMMAND "${WINDEPLOYQT}" --no-translations --no-compiler-runtime --no-opengl-sw
                    --no-system-d3d-compiler "$<TARGET_FILE:eds_builder>"
            COMMENT "windeployqt")
    endif()
endif()
