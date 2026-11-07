cmake_minimum_required(VERSION 3.22)
project(smogame C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# High-performance silicon flags for Snapdragon 8s Gen 4 (ARMv8.2-A / NEON)
add_compile_options(
    -O3
    -flto
    -g0
    -w
    -fno-strict-aliasing
    -D_GNU_SOURCE
    -DSMO_LAYOUT=android
    -D__ANDROID__
    -ffast-math
    -ffunction-sections
    -fdata-sections
)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64" OR ANDROID_ABI STREQUAL "arm64-v8a")
    add_compile_options(-march=armv8.2-a+simd)
endif()

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/../../runtime
    ${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/android
)

file(GLOB GENERATED_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/*.cpp
)

# In case generated/ is still empty prior to extraction, gamelib_entry.c provides the base export
list(APPEND GENERATED_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/gamelib_entry.c
)

add_library(smogame SHARED ${GENERATED_SRCS})

target_link_libraries(smogame PRIVATE
    main
    m
    dl
)

set_target_properties(smogame PROPERTIES
    OUTPUT_NAME "smogame"
    PREFIX "lib"
)
