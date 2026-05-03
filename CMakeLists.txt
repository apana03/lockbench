cmake_minimum_required(VERSION 3.20)
project(lockbench LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

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

# --- cdsbench: libcds StripedMap with pluggable per-stripe lock ---
add_executable(cdsbench bench/cds_bench.cpp)
target_include_directories(cdsbench PRIVATE include third_party/libcds)
target_link_libraries(cdsbench PRIVATE Threads::Threads)

# --- cdstest: correctness probe for libcds StripedMap ---
add_executable(cdstest bench/cds_test.cpp)
target_include_directories(cdstest PRIVATE include third_party/libcds)
target_link_libraries(cdstest PRIVATE Threads::Threads)

# --- cds-rt: tiny static lib carrying the libcds runtime symbols
#     (URCU singletons, threading manager TLS, HP/DHP storage).
#     Required by anything using BronsonAVLTreeMap, not by StripedMap.
set(CDS_RT_SRCS
  third_party/libcds/src/urcu_gp.cpp
  third_party/libcds/src/urcu_sh.cpp
  third_party/libcds/src/init.cpp
  third_party/libcds/src/thread_data.cpp
  third_party/libcds/src/hp.cpp
  third_party/libcds/src/dhp.cpp
  third_party/libcds/src/hp_thread_local.cpp)
if (APPLE)
  list(APPEND CDS_RT_SRCS third_party/libcds/src/topology_osx.cpp)
elseif (UNIX)
  list(APPEND CDS_RT_SRCS third_party/libcds/src/topology_linux.cpp)
endif()
add_library(cds-rt STATIC ${CDS_RT_SRCS})
target_include_directories(cds-rt PUBLIC third_party/libcds)
target_compile_options(cds-rt PRIVATE -Wno-unused-parameter)

# --- cds_avl_bench: BronsonAVLTreeMap with pluggable per-node lock ---
add_executable(cds_avl_bench bench/cds_avl_bench.cpp)
target_include_directories(cds_avl_bench PRIVATE include)
target_link_libraries(cds_avl_bench PRIVATE cds-rt Threads::Threads)

# --- cds_avl_test: correctness probe for BronsonAVLTreeMap ---
add_executable(cds_avl_test bench/cds_avl_test.cpp)
target_include_directories(cds_avl_test PRIVATE include)
target_link_libraries(cds_avl_test PRIVATE cds-rt Threads::Threads)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(cds_avl_bench PRIVATE -march=native -fno-omit-frame-pointer)
  target_compile_options(cds_avl_test  PRIVATE -march=native -fno-omit-frame-pointer)
endif()

# --- wormhole: per-lock static lib + bench + test variants ---
# WH_LOCKS = default + the lockbench primitives. The "default" variant
# compiles upstream wormhole untouched (no shim); each other variant
# compiles wh_lock_shim.cpp with WH_LOCK_<name> selecting the lock.
set(WH_C_SRCS
  third_party/wormhole/lib.c
  third_party/wormhole/kv.c
  third_party/wormhole/wh.c)
# Ticket lock excluded: its try_lock approximation (empty-queue CAS) gives
# pathological numbers under wormhole's reader fast path. See plan/EXPERIMENT.md.
#
# occ-opt is a special variant: same write-side as cas, but enables
# WH_OCC_OPTIMISTIC in wh.c — readers walk the leaf without the leaflock
# and validate via a per-leaf seqlock counter. See plan/EXPERIMENT.md.
set(WH_LOCKS default rw tas ttas cas occ occ-opt)

foreach(lk IN LISTS WH_LOCKS)
  string(TOUPPER "${lk}" LK_UPPER)
  set(libname wormhole-rt-${lk})
  if (lk STREQUAL "default")
    add_library(${libname} STATIC ${WH_C_SRCS})
  elseif (lk STREQUAL "occ-opt")
    add_library(${libname} STATIC ${WH_C_SRCS}
        third_party/wormhole/wh_lock_shim.cpp)
    target_compile_definitions(${libname} PUBLIC
        WH_LOCK_SHIM WH_LOCK_CAS WH_OCC_OPTIMISTIC)
  else()
    add_library(${libname} STATIC ${WH_C_SRCS}
        third_party/wormhole/wh_lock_shim.cpp)
    target_compile_definitions(${libname} PUBLIC WH_LOCK_SHIM WH_LOCK_${LK_UPPER})
  endif()
  target_include_directories(${libname} PUBLIC
      third_party/wormhole include)
  set_source_files_properties(${WH_C_SRCS} PROPERTIES LANGUAGE C)
  target_compile_definitions(${libname} PUBLIC NOSIGNAL)
  if (WH_FAIR_MM)
    # Use the no-op-free kvmap_mm for ALL variants so per-op free() cost
    # is uniform. Required for an apples-to-apples wh-occ-opt comparison;
    # otherwise the locked variants pay a per-op free() cost the
    # optimistic variant avoids, which inflates the read-strategy delta.
    target_compile_definitions(${libname} PUBLIC WH_FAIR_MM)
  endif()
  target_compile_options(${libname} PRIVATE
      -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable
      -Wno-unused-but-set-variable)
  if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${libname} PRIVATE
        -march=native -fno-omit-frame-pointer)
  endif()
  # Wormhole's wh.c uses NEON intrinsics with vector reinterpret patterns
  # that GCC rejects under default strictness on aarch64 (e.g. wh.c:1610
  # passes a uint16x8_t to vandq_u8). Apple's clang is permissive; GCC
  # needs -flax-vector-conversions. Apply for both C and C++ TUs in this
  # static lib regardless of compiler — flag is harmless on clang.
  if (CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    target_compile_options(${libname} PRIVATE -flax-vector-conversions)
  endif()

  add_executable(wh_bench_${lk} bench/wh_bench.cpp)
  target_include_directories(wh_bench_${lk} PRIVATE
      include third_party/wormhole)
  target_compile_definitions(wh_bench_${lk} PRIVATE
      WH_LOCK_LABEL="wh-${lk}")
  target_link_libraries(wh_bench_${lk} PRIVATE ${libname} Threads::Threads)

  add_executable(wh_test_${lk} bench/wh_test.cpp)
  target_include_directories(wh_test_${lk} PRIVATE
      include third_party/wormhole)
  target_compile_definitions(wh_test_${lk} PRIVATE
      WH_LOCK_LABEL="wh-${lk}")
  target_link_libraries(wh_test_${lk} PRIVATE ${libname} Threads::Threads)

  if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(wh_bench_${lk} PRIVATE -march=native -fno-omit-frame-pointer)
    target_compile_options(wh_test_${lk}  PRIVATE -march=native -fno-omit-frame-pointer)
  endif()
endforeach()

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(lockbench PRIVATE -march=native -fno-omit-frame-pointer)
  target_compile_options(indexbench PRIVATE -march=native -fno-omit-frame-pointer)
  target_compile_options(arraybench PRIVATE -march=native -fno-omit-frame-pointer)
  target_compile_options(locktest PRIVATE -march=native -fno-omit-frame-pointer)
  target_compile_options(skiplistbench PRIVATE -march=native -fno-omit-frame-pointer)
  target_compile_options(bptreebench   PRIVATE -march=native -fno-omit-frame-pointer)
  target_compile_options(indextest     PRIVATE -march=native -fno-omit-frame-pointer)
  target_compile_options(cdsbench      PRIVATE -march=native -fno-omit-frame-pointer)
  target_compile_options(cdstest       PRIVATE -march=native -fno-omit-frame-pointer)
endif()