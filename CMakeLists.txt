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

# --- locktest: lock correctness tests ---
add_executable(locktest bench/lock_test.cpp)
target_include_directories(locktest PRIVATE include)
target_link_libraries(locktest PRIVATE Threads::Threads)

# --- skiplistbench: concurrent skip list with per-node locking ---
add_executable(skiplistbench bench/skiplist_bench.cpp)
target_include_directories(skiplistbench PRIVATE include)
target_link_libraries(skiplistbench PRIVATE Threads::Threads)

# --- bptreebench: concurrent B+ tree with crabbing ---
add_executable(bptreebench bench/bptree_bench.cpp)
target_include_directories(bptreebench PRIVATE include)
target_link_libraries(bptreebench PRIVATE Threads::Threads)

# --- indextest: correctness probe for skiplist + bptree ---
add_executable(indextest bench/index_test.cpp)
target_include_directories(indextest PRIVATE include)
target_link_libraries(indextest PRIVATE Threads::Threads)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(lockbench PRIVATE -march=native -fno-omit-frame-pointer)
  target_compile_options(indexbench PRIVATE -march=native -fno-omit-frame-pointer)
  target_compile_options(arraybench PRIVATE -march=native -fno-omit-frame-pointer)
  target_compile_options(locktest PRIVATE -march=native -fno-omit-frame-pointer)
  target_compile_options(skiplistbench PRIVATE -march=native -fno-omit-frame-pointer)
  target_compile_options(bptreebench   PRIVATE -march=native -fno-omit-frame-pointer)
  target_compile_options(indextest     PRIVATE -march=native -fno-omit-frame-pointer)
endif()