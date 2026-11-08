cmake_minimum_required(VERSION 3.22)
project(smo_runtime C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Optimization and Silicon flags for AArch64 (ARMv8.2-A + SIMD NEON)
add_compile_options(
    -O3
    -flto
    -g0
    -w
    -fno-strict-aliasing
    -D_GNU_SOURCE
    -DSMO_LAYOUT=android
    -D__ANDROID__
    -Dfn_table=(*game_fn_table_p)
    -Dfn_table_count=(*game_fn_table_count_p)
    -Ddyn_syms=(*game_dyn_syms_p)
    -Ddyn_sym_count=(*game_dyn_sym_count_p)
    -ffunction-sections
    -fdata-sections
)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64" OR ANDROID_ABI STREQUAL "arm64-v8a")
    add_compile_options(-march=armv8.2-a+simd)
endif()

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/android
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/khronos
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
    ${ANDROID_NDK}/sources/android/native_app_glue
)

# Identify runtime C sources, excluding Windows-only stubs
set(EXCLUDE_SOURCES
    smo_guide.c
    nnmovie_mf.c
    nnpad_sdl.c
    vk_glslang_shim.c
    vk_shaderc_shim.c
    gamedll.c
    game_shim.c
    nvn_vk_draw.c      # Directly #included inside nvn_vk.c
    nvn_vk_store.c     # Directly #included inside nvn_vk.c
    nvn_vk_m4.c        # Directly #included inside nvn_vk.c
    nvn_vk_wd.c        # Directly #included inside nvn_vk.c
)

file(GLOB RUNTIME_C_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/*.c)
file(GLOB ANDROID_C_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/android/*.c)

set(FILTERED_SOURCES "")
foreach(SRC ${RUNTIME_C_SOURCES})
    get_filename_component(FNAME ${SRC} NAME)
    list(FIND EXCLUDE_SOURCES ${FNAME} IDX)
    if(IDX EQUAL -1)
        list(APPEND FILTERED_SOURCES ${SRC})
    endif()
endforeach()

list(APPEND FILTERED_SOURCES
    ${ANDROID_C_SOURCES}
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c
)

add_library(main SHARED ${FILTERED_SOURCES})

target_link_libraries(main PRIVATE
    android
    nativewindow
    log
    aaudio
    mediandk
    vulkan
    EGL
    GLESv3
    m
    z
    dl
)

set_target_properties(main PROPERTIES
    OUTPUT_NAME "main"
    PREFIX "lib"
)
