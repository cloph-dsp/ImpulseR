cmake_minimum_required(VERSION 3.22.1)
project(impulser_capture)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

# Enable optimization for release builds
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -ffast-math -funroll-loops")
set(CMAKE_C_FLAGS_RELEASE "-O3 -ffast-math")

# Oboe library (cloned from https://github.com/google/oboe at tag 1.8.0)
set(OBOE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/oboe-1.8.0)
add_subdirectory(${OBOE_DIR} oboe)

# Main native library
add_library(impulser_capture SHARED
    engine/OboeEngine.cpp
    engine/RingBuffer.h
    engine/ESSGenerator.cpp
    engine/Deconvolver.cpp
    engine/CalibrationFilter.cpp
    engine/IRProcessor.cpp
    engine/WavExporter.cpp
    renderer/SpectrogramRenderer.cpp
    renderer/WaveformRenderer.cpp
    jni/JniBridge.cpp
)

target_include_directories(impulser_capture PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer
    ${CMAKE_CURRENT_SOURCE_DIR}/jni
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

target_link_libraries(impulser_capture
    android
    log
    GLESv3
    EGL
    oboe
)

# Enable NEON on ARM for main library
if(ANDROID_ABI STREQUAL "arm64-v8a")
    target_compile_options(impulser_capture PRIVATE -mfpu=neon -mfloat-abi=softfp)
elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
    target_compile_options(impulser_capture PRIVATE -mfpu=neon -mfloat-abi=softfp)
endif()
