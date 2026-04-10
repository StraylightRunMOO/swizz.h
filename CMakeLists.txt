cmake_minimum_required(VERSION 3.10)
project(SwissTable C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Include directories (root for swiss.h)
include_directories(.)

# Compiler flags
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -O2)
endif()

# Test executable
add_executable(test_swiss tests/test.c)
set_target_properties(test_swiss PROPERTIES OUTPUT_NAME "test")

# Example executable
add_executable(example examples/example.c)

# Benchmark executable
add_executable(benchmark benchmarks/benchmark.c)

# Stress test executable
add_executable(stress benchmarks/stress.c)

# Enable testing
enable_testing()
add_test(NAME SwissTableTests COMMAND test_swiss)
