cmake_minimum_required(VERSION 3.21)

project(audio_duplicator VERSION 0.2.1 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Audio Duplicator currently supports Windows only.")
endif()

add_executable(audio_duplicator WIN32
    audio_duplicator_compat.cpp
    device_selector.cpp
    device_selector.h
)

target_compile_features(audio_duplicator PRIVATE cxx_std_17)
target_compile_definitions(audio_duplicator PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
)
target_link_libraries(audio_duplicator PRIVATE
    ole32
    mmdevapi
    avrt
    user32
    shell32
    gdi32
)

if(MSVC)
    target_compile_options(audio_duplicator PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(audio_duplicator PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS audio_duplicator RUNTIME DESTINATION .)
