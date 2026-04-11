cmake_minimum_required(VERSION 3.14)
project(SwizzTable C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Include directories (root for swizz.h)
include_directories(.)

# -------------------------- SIMD feature detection --------------------------
include(CheckCCompilerFlag)
option(SWIZZ_ENABLE_SIMD "Enable SIMD optimizations (AVX2/SSE2/NEON)" ON)

# Use a list to collect SIMD flags
set(SWIZZ_SIMD_FLAGS)

if(SWIZZ_ENABLE_SIMD)
    # x86 AVX2
    check_c_compiler_flag("-mavx2" COMPILER_HAS_AVX2)
    if(COMPILER_HAS_AVX2)
        message(STATUS "SwizzTable: AVX2 support detected")
        list(APPEND SWIZZ_SIMD_FLAGS -mavx2)
    else()
        # Fallback to SSE2 on x86
        check_c_compiler_flag("-msse2" COMPILER_HAS_SSE2)
        if(COMPILER_HAS_SSE2)
            message(STATUS "SwizzTable: SSE2 support detected")
            list(APPEND SWISS_SIMD_FLAGS -msse2)
        endif()
    endif()

    # ARM NEON
    check_c_compiler_flag("-march=armv8-a+simd" COMPILER_HAS_NEON)
    if(COMPILER_HAS_NEON)
        message(STATUS "SwizzTable: NEON support detected")
        list(APPEND SWISS_SIMD_FLAGS -march=armv8-a+simd)
    endif()
endif()

# Base compiler flags
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -O3 -std=c11)
endif()

# -------------------------- Targets --------------------------
add_executable(test_swizz tests/test.c)
set_target_properties(test_swizz PROPERTIES OUTPUT_NAME "test")

add_executable(example examples/example.c)

add_executable(benchmark benchmarks/benchmark.c)

add_executable(stress benchmarks/stress.c)

# Apply SIMD flags to performance-critical targets
if(SWIZZ_ENABLE_SIMD AND SWIZZ_SIMD_FLAGS)
    target_compile_options(benchmark PRIVATE ${SWIZZ_SIMD_FLAGS})
    target_compile_options(stress PRIVATE ${SWISS_SIMD_FLAGS})
    target_compile_options(test_swizz PRIVATE ${SWISS_SIMD_FLAGS})
endif()

# Enable testing
enable_testing()
add_test(NAME SwizzTableTests COMMAND test_swizz)
