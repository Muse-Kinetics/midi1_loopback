cmake_minimum_required(VERSION 3.14)

project(midi1_loopback
    VERSION 1.0.0
    DESCRIPTION "MIDI 1.0 virtual loopback buses (a non-translating IAC replacement)"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# --- RtMidi dependency --------------------------------------------------------
# By default we fetch a pinned RtMidi release so the repo is self-contained.
# To build against a local checkout instead, configure with:
#   -DRTMIDI_FETCH=OFF -DRtMidi_DIR=/path/to/installed/rtmidi/cmake
option(RTMIDI_FETCH "Download RtMidi via FetchContent" ON)
set(RTMIDI_GIT_TAG "6.0.0" CACHE STRING "RtMidi git tag to fetch")

if(RTMIDI_FETCH)
    include(FetchContent)
    # Don't build RtMidi's test programs.
    set(RTMIDI_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(RTMIDI_BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        rtmidi
        GIT_REPOSITORY https://github.com/thestk/rtmidi.git
        GIT_TAG ${RTMIDI_GIT_TAG}
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(rtmidi)
else()
    find_package(RtMidi REQUIRED)
endif()

# --- Our executable -----------------------------------------------------------
add_executable(midi1_loopback src/midi1_loopback.cpp)
target_link_libraries(midi1_loopback PRIVATE rtmidi)

# RtMidi's own CMake target carries the CoreMIDI/ALSA/etc. backend and its
# framework/link dependencies, so no per-platform framework lines are needed
# here. This tool is written for the macOS CoreMIDI backend.

install(TARGETS midi1_loopback RUNTIME DESTINATION bin)
install(FILES port_config.json DESTINATION share/midi1_loopback)
