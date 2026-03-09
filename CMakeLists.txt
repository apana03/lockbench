cmake_minimum_required(VERSION 3.20)
project(lockbench LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Useful for benchmarks
add_compile_options(-O3)

find_package(Threads REQUIRED)

# --- lockbench: raw lock/unlock micro-benchmarks ---
add_executable(lockbench bench/main.cpp)
target_include_directories(lockbench PRIVATE include)
target_link_libraries(lockbench PRIVATE Threads::Threads)

# --- indexbench: concurrent index benchmarks ---
add_executable(indexbench bench/index_bench.cpp)
target_include_directories(indexbench PRIVATE include)
target_link_libraries(indexbench PRIVATE Threads::Threads)

# --- arraybench: shared array benchmarks ---
add_executable(arraybench bench/array_bench.cpp)
target_include_directories(arraybench PRIVATE include)
target_link_libraries(arraybench PRIVATE Threads::Threads)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(lockbench PRIVATE -march=native -fno-omit-frame-pointer)
  target_compile_options(indexbench PRIVATE -march=native -fno-omit-frame-pointer)
  target_compile_options(arraybench PRIVATE -march=native -fno-omit-frame-pointer)
endif()