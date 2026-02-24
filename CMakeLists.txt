cmake_minimum_required(VERSION 3.20)
project(lockbench LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Useful for benchmarks
add_compile_options(-O3)

add_executable(lockbench bench/main.cpp)
target_include_directories(lockbench PRIVATE include)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(lockbench PRIVATE -march=native -fno-omit-frame-pointer)
endif()