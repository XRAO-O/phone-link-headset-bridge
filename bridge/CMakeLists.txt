cmake_minimum_required(VERSION 3.12)

project(headset_bridge C RC)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(BTSTACK_ROOT ${CMAKE_SOURCE_DIR}/../btstack)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

if(NOT EXISTS ${BTSTACK_ROOT}/src/btstack.h)
    message(FATAL_ERROR "BTstack not found in ${BTSTACK_ROOT}. Run: git submodule update --init")
endif()

option(BRIDGE_STATIC "Link the C runtime and PortAudio statically so the exes need no extra DLLs" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PORTAUDIO REQUIRED portaudio-2.0)

# bridge btstack_config.h must win over any other copy
include_directories(BEFORE ${CMAKE_SOURCE_DIR})
include_directories(
    ${BTSTACK_ROOT}/3rd-party/bluedroid/decoder/include
    ${BTSTACK_ROOT}/3rd-party/bluedroid/encoder/include
    ${BTSTACK_ROOT}/3rd-party/md5
    ${BTSTACK_ROOT}/3rd-party/micro-ecc
    ${BTSTACK_ROOT}/3rd-party/rijndael
    ${BTSTACK_ROOT}/3rd-party/yxml
    ${BTSTACK_ROOT}/src
    ${BTSTACK_ROOT}/platform/embedded
    ${BTSTACK_ROOT}/platform/posix
    ${BTSTACK_ROOT}/platform/windows
)

file(GLOB SOURCES_SRC       "${BTSTACK_ROOT}/src/*.c")
file(GLOB SOURCES_CLASSIC   "${BTSTACK_ROOT}/src/classic/*.c")
file(GLOB SOURCES_BLE       "${BTSTACK_ROOT}/src/ble/*.c")
file(GLOB SOURCES_BLUEDROID "${BTSTACK_ROOT}/3rd-party/bluedroid/encoder/srce/*.c" "${BTSTACK_ROOT}/3rd-party/bluedroid/decoder/srce/*.c")
file(GLOB SOURCES_WINDOWS   "${BTSTACK_ROOT}/platform/windows/*.c")
list(REMOVE_ITEM SOURCES_BLE "${BTSTACK_ROOT}/src/ble/le_device_db_memory.c")
list(FILTER SOURCES_SRC EXCLUDE REGEX "lc3")

add_library(btstack STATIC
    ${SOURCES_SRC}
    ${SOURCES_CLASSIC}
    ${SOURCES_BLE}
    ${SOURCES_BLUEDROID}
    ${SOURCES_WINDOWS}
    ${BTSTACK_ROOT}/3rd-party/micro-ecc/uECC.c
    ${BTSTACK_ROOT}/3rd-party/rijndael/rijndael.c
    ${BTSTACK_ROOT}/3rd-party/md5/md5.c
    ${BTSTACK_ROOT}/3rd-party/yxml/yxml.c
)

# Console bridge
add_executable(headset_bridge main.c scan.c audio_io.c config.c cli.rc)
target_compile_options(headset_bridge PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_include_directories(headset_bridge PRIVATE ${PORTAUDIO_INCLUDE_DIRS})
if(BRIDGE_STATIC)
    target_link_directories(headset_bridge PRIVATE ${PORTAUDIO_STATIC_LIBRARY_DIRS})
    # MSYS2's portaudio-2.0.pc does not list the system libraries the static archive needs
    target_link_libraries(headset_bridge btstack ${PORTAUDIO_STATIC_LIBRARIES} winmm ole32 uuid setupapi winusb m)
    target_link_options(headset_bridge PRIVATE -static)
else()
    target_link_directories(headset_bridge PRIVATE ${PORTAUDIO_LIBRARY_DIRS})
    target_link_libraries(headset_bridge btstack ${PORTAUDIO_LIBRARIES} setupapi winusb m)
    target_link_options(headset_bridge PRIVATE -static-libgcc)
endif()

# Windows GUI (runs headset_bridge.exe as a child process)
add_executable(headset_bridge_gui WIN32 gui.c config.c gui.rc)
target_compile_definitions(headset_bridge_gui PRIVATE UNICODE _UNICODE _WIN32_WINNT=0x0A00 WINVER=0x0A00)
target_compile_options(headset_bridge_gui PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(headset_bridge_gui comctl32 shell32 advapi32 user32 gdi32)
target_link_options(headset_bridge_gui PRIVATE -municode $<IF:$<BOOL:${BRIDGE_STATIC}>,-static,-static-libgcc>)

configure_file(${CMAKE_SOURCE_DIR}/bridge.example.ini ${CMAKE_SOURCE_DIR}/bin/bridge.example.ini COPYONLY)
